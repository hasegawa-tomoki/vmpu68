// SPDX-License-Identifier: MIT
// Virtual SCSI targets: see vscsi.h.
#include "vscsi.h"
#include "sdimg.h"
extern "C" {
#include "../core/emu68k.h"
#include "../pi/vmpu68.h"
}
#include <circle/string.h>
#include <circle/alloc.h>
#include <circle/util.h>
#include <circle/logger.h>
#include <circle/sched/task.h>
#include <circle/sched/scheduler.h>
#include <fatfs/ff.h>
#include <fatfs/diskio.h>

// ---- SCSI status / sense ------------------------------------------------
#define ST_GOOD              0x00
#define ST_CHECK_CONDITION   0x02
#define SK_NO_SENSE        0x00
#define SK_NOT_READY       0x02
#define SK_MEDIUM_ERROR    0x03
#define SK_ILLEGAL_REQUEST 0x05
#define SK_UNIT_ATTENTION  0x06
#define SK_DATA_PROTECT    0x07
#define ASC_INVALID_CMD    0x20
#define ASC_INVALID_LUN    0x25
#define ASC_LBA_RANGE      0x21
#define ASC_MEDIUM_NOT_PRESENT 0x3A
#define ASC_MEDIUM_CHANGED 0x28
#define ASC_WRITE_PROTECT  0x27

struct Target
{
    volatile boolean present;       // image mounted
    boolean  configured;            // ever mounted since the machine booted: the Pi answers for this ID
    boolean  cdrom, wp;
    unsigned bs;                    // block size: 256 / 512 / 2048
    unsigned blocks;
    FIL      fil;                   // core 0 only
    // direct card access: the image's cluster chain as runs of card sectors
    // (built at mount; the FatFs path is kept for fragmented images and
    // 256-byte-block images with unaligned requests)
    struct Extent { u32 fsec, csec, count; };   // file sector, card sector, sectors
    Extent  *ext;
    unsigned n_ext;
    boolean  direct;
    char     path[128];
    u8       sk, asc, ascq;
    volatile boolean ua;            // UNIT ATTENTION pending (mount/eject)
    unsigned reads, writes, errors;
};
static Target s_t[VSCSI_IDS];

// ---- file I/O bridge: core 3 asks, the core-0 task does FatFs ---------------
struct IoReq
{
    volatile u32 state;             // 0 idle, 1 posted, 2 done
    unsigned op;                    // 0 read, 1 write
    unsigned id;
    u32      lba, count;            // blocks
    u8      *buf;
    volatile int result;            // 1 ok, 0 failed
};
static IoReq s_io __attribute__ ((aligned (64)));
static unsigned s_cmds, s_io_reads, s_io_writes, s_io_fail;
struct CdbLog { u8 id, dir, cdb[12]; u32 len, xfer; u8 st; };
static CdbLog s_log[512]; static unsigned s_log_n;  // last 512 commands (console 'vs' shows 32, 'vsl <n>' up to 512)
static u64 s_io_ticks;              // core-0 time inside FatFs / the card driver
static unsigned s_io_direct;        // requests served by the direct card path

static const char *fs_path (const char *p, char *buf, unsigned n) { return sdimg_path (p, buf, n); }   // "/x.hds" -> the image volume (sdimg.h)

// Walk the image's FAT chain once and record it as runs of card sectors, so
// a request becomes one multi-block card command per run instead of FatFs's
// one command per cluster (512-byte clusters on the small SD volume: one
// command per sector, 2.9 MB/s read / 0.4 MB/s write).  The FAT is read
// through disk_read() under the volume lock.  Fails (FatFs path kept) on
// FAT12, a broken chain, or more runs than fit.
static boolean build_extents (Target &t)
{
    FATFS *fs = t.fil.obj.fs;
    t.direct = FALSE; delete [] t.ext; t.ext = 0; t.n_ext = 0;
    if (!fs || (fs->fs_type != FS_FAT16 && fs->fs_type != FS_FAT32) || t.fil.obj.sclust < 2) return FALSE;
    const unsigned MAXEXT = 8192;
    Target::Extent *e = new Target::Extent[MAXEXT];
    if (!e) return FALSE;
    u32 csize = fs->csize, clusters = (u32) ((f_size (&t.fil) + (FSIZE_t) csize * 512 - 1) / ((FSIZE_t) csize * 512));
    u32 c = t.fil.obj.sclust, fsec = 0, cached = 0xFFFFFFFF; unsigned n = 0; boolean ok = TRUE;
    static u8 fat[512] __attribute__ ((aligned (16)));
    ff_mutex_take (0);
    for (u32 i = 0; i < clusters && ok; i++)
    {
        if (c < 2 || c >= fs->n_fatent) { ok = FALSE; break; }
        u32 csec = (u32) (fs->database + (LBA_t) (c - 2) * csize);
        if (n && e[n - 1].csec + e[n - 1].count == csec) e[n - 1].count += csize;
        else if (n < MAXEXT) { e[n].fsec = fsec; e[n].csec = csec; e[n].count = csize; n++; }
        else { ok = FALSE; break; }
        fsec += csize;
        if (i + 1 == clusters) break;
        u32 bpe = fs->fs_type == FS_FAT32 ? 4 : 2, sec = (u32) (fs->fatbase + (c * bpe) / 512), o = (c * bpe) % 512;
        if (sec != cached) { if (disk_read (fs->pdrv, fat, sec, 1) != RES_OK) { ok = FALSE; break; } cached = sec; }
        c = bpe == 4 ? (((u32) fat[o] | ((u32) fat[o + 1] << 8) | ((u32) fat[o + 2] << 16) | ((u32) fat[o + 3] << 24)) & 0x0FFFFFFF)
                     : ((u32) fat[o] | ((u32) fat[o + 1] << 8));
    }
    ff_mutex_give (0);
    if (!ok || !n) { delete [] e; return FALSE; }
    t.ext = new Target::Extent[n];
    if (!t.ext) { delete [] e; return FALSE; }
    memcpy (t.ext, e, n * sizeof (Target::Extent)); delete [] e;
    t.n_ext = n; t.direct = TRUE;
    return TRUE;
}

static int io_direct (Target &t, unsigned op, u32 sec, u32 cnt, u8 *buf)   // core 0, 512-byte card sectors within the image
{
    unsigned i = 0, lo = 0, hi = t.n_ext;      // run holding sec (runs are sorted by fsec)
    while (lo + 1 < hi) { unsigned m = (lo + hi) / 2; if (t.ext[m].fsec <= sec) lo = m; else hi = m; }
    i = lo;
    BYTE pdrv = t.fil.obj.fs->pdrv; int ok = 1;
    ff_mutex_take (0);
    while (cnt && ok)
    {
        if (i >= t.n_ext || t.ext[i].fsec > sec || sec >= t.ext[i].fsec + t.ext[i].count) { ok = 0; break; }
        u32 n = t.ext[i].fsec + t.ext[i].count - sec; if (n > cnt) n = cnt;
        LBA_t card = t.ext[i].csec + (sec - t.ext[i].fsec);
        ok = (op == 0 ? disk_read (pdrv, buf, card, n) : disk_write (pdrv, buf, card, n)) == RES_OK;
        sec += n; cnt -= n; buf += n * 512; i++;
    }
    ff_mutex_give (0);
    return ok;
}

static int io_do (IoReq &r)         // core 0
{
    Target &t = s_t[r.id];
    if (!t.present) return 0;
    if (r.lba + r.count > t.blocks) return 0;
    FSIZE_t off = (FSIZE_t) r.lba * t.bs; UINT want = r.count * t.bs, n = 0;
    if (r.op == 1 && t.wp) return 0;
    if (t.direct && (off % 512) == 0 && (want % 512) == 0)
    {
        s_io_direct++;
        return io_direct (t, r.op, (u32) (off / 512), want / 512, r.buf);   // writes go to the card before GOOD, nothing to sync
    }
    if (f_lseek (&t.fil, off) != FR_OK) return 0;
    if (r.op == 0) return f_read (&t.fil, r.buf, want, &n) == FR_OK && n == want;
    if (t.wp) return 0;
    if (f_write (&t.fil, r.buf, want, &n) != FR_OK || n != want) return 0;
    return f_sync (&t.fil) == FR_OK;        // write-through: the data is on the card when the X68000 sees GOOD
}

class CVscsiIoTask : public CTask
{
public:
    CVscsiIoTask (void) : CTask (16384) { SetName ("vscsi-io"); }
    void Run (void) override
    {
        for (;;)
        {
            if (__atomic_load_n (&s_io.state, __ATOMIC_ACQUIRE) == 1)
            {
                u64 t0 = vmpu68_ticks ();
                int ok = io_do (s_io);
                s_io_ticks += vmpu68_ticks () - t0;
                if (s_io.op == 0) s_io_reads++; else s_io_writes++;
                if (!ok) s_io_fail++;
                s_io.result = ok;
                __atomic_store_n (&s_io.state, 2, __ATOMIC_RELEASE);
            }
            else
                CScheduler::Get ()->Yield ();
        }
    }
};
static CVscsiIoTask *s_pIoTask;

static int io_request (unsigned op, unsigned id, u32 lba, u32 count, u8 *buf)   // core 3: blocks until done
{
    if (!s_pIoTask) return 0;
    s_io.op = op; s_io.id = id; s_io.lba = lba; s_io.count = count; s_io.buf = buf; s_io.result = 0;
    __atomic_store_n (&s_io.state, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n (&s_io.state, __ATOMIC_ACQUIRE) != 2) asm volatile ("yield");
    int r = s_io.result;
    __atomic_store_n (&s_io.state, 0, __ATOMIC_RELEASE);
    return r;
}

// ---- command execution (core 3) ----------------------------------------------
static void put32 (u8 *p, u32 v) { p[0] = (u8) (v >> 24); p[1] = (u8) (v >> 16); p[2] = (u8) (v >> 8); p[3] = (u8) v; }
static void sense_set (Target &t, u8 key, u8 asc, u8 ascq) { t.sk = key; t.asc = asc; t.ascq = ascq; }

// Returns the SCSI status byte; *xfer = bytes placed in / taken from buf.
static unsigned execute (Target &t, const u8 *c, unsigned dir, u8 *buf, u32 len, u32 *xfer)
{
    *xfer = 0;
    u8 op = c[0];
    unsigned lun = (c[1] >> 5) & 7;
    if (lun != 0)
    {
        if (op == 0x12) { u8 q[36]; memset (q, 0, sizeof q); q[0] = 0x7F; q[3] = 1; q[4] = 31; u32 n = c[4] < 36 ? c[4] : 36; if (n > len) n = len; memcpy (buf, q, n); *xfer = n; return ST_GOOD; }
        sense_set (t, SK_ILLEGAL_REQUEST, ASC_INVALID_LUN, 0); return ST_CHECK_CONDITION;
    }
    if (t.ua && op != 0x12 && op != 0x03) { t.ua = FALSE; sense_set (t, SK_UNIT_ATTENTION, ASC_MEDIUM_CHANGED, 0); return ST_CHECK_CONDITION; }
    if (!t.present && op != 0x12 && op != 0x03) { sense_set (t, SK_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0); return ST_CHECK_CONDITION; }
    u32 lba = 0, n = 0;
    switch (op)
    {
    case 0x00: return ST_GOOD;                                              // TEST UNIT READY
    case 0x12: {                                                            // INQUIRY
        u8 q[36]; memset (q, 0, sizeof q);
        q[0] = t.cdrom ? 0x05 : 0x00; q[1] = 0x80; q[2] = t.cdrom ? 0x02 : 0x01; q[3] = 0x01; q[4] = 31;
        memcpy (q + 8, "SHARP   ", 8); memcpy (q + 16, t.cdrom ? "vmpu68 SCSI CD-ROM" : "vmpu68 SCSI HD  ", 16); memcpy (q + 32, "1.0 ", 4);
        u32 a = c[4] < 36 ? c[4] : 36; if (a > len) a = len; memcpy (buf, q, a); *xfer = a; return ST_GOOD; }
    case 0x03: {                                                            // REQUEST SENSE
        u8 s[18]; memset (s, 0, sizeof s); s[0] = 0x70; s[2] = t.sk; s[7] = 10; s[12] = t.asc; s[13] = t.ascq;
        u32 a = c[4] ? c[4] : 4; if (a > 18) a = 18; if (a > len) a = len; memcpy (buf, s, a); *xfer = a;
        sense_set (t, SK_NO_SENSE, 0, 0); return ST_GOOD; }
    case 0x25: {                                                            // READ CAPACITY
        u8 d[8]; put32 (d, t.blocks ? t.blocks - 1 : 0); put32 (d + 4, t.bs);
        u32 a = len < 8 ? len : 8; memcpy (buf, d, a); *xfer = a; return ST_GOOD; }
    case 0x43: {                                                            // READ TOC: one data track + lead-out
        if (!t.cdrom) { sense_set (t, SK_ILLEGAL_REQUEST, ASC_INVALID_CMD, 0); return ST_CHECK_CONDITION; }
        boolean msf = (c[1] & 2) != 0; u8 start = c[6]; u8 d[20]; u32 k = 4; d[2] = 1; d[3] = 1;
        for (int pass = 0; pass < 2; pass++)
        {
            u8 trk = pass == 0 ? 1 : 0xAA; u32 l = pass == 0 ? 0 : t.blocks;
            if (start != 0 && start != 0xAA && trk < start && trk != 0xAA) continue;
            d[k] = 0; d[k + 1] = 0x14; d[k + 2] = trk; d[k + 3] = 0;
            if (msf) { u32 m2 = l + 150; d[k + 4] = 0; d[k + 5] = (u8) (m2 / 4500); d[k + 6] = (u8) ((m2 / 75) % 60); d[k + 7] = (u8) (m2 % 75); }
            else put32 (d + k + 4, l);
            k += 8;
        }
        d[0] = (u8) ((k - 2) >> 8); d[1] = (u8) (k - 2);
        u32 a = ((u32) c[7] << 8) | c[8]; if (a == 0 || a > k) a = k; if (a > len) a = len; memcpy (buf, d, a); *xfer = a; return ST_GOOD; }
    case 0x08: case 0x0A: lba = ((u32) (c[1] & 0x1F) << 16) | ((u32) c[2] << 8) | c[3]; n = c[4] ? c[4] : 256; break;
    case 0x28: case 0x2A: case 0x2F: lba = ((u32) c[2] << 24) | ((u32) c[3] << 16) | ((u32) c[4] << 8) | c[5]; n = ((u32) c[7] << 8) | c[8]; break;
    case 0x01: case 0x0B: case 0x2B: case 0x04: case 0x1B: case 0x1E: case 0x15: case 0x06: case 0x07: case 0x0E: case 0xC1:
        *xfer = (dir == 2 && len) ? len : 0;                                // accept, consume any data-out
        return ST_GOOD;
    case 0x1A: {                                                            // MODE SENSE(6): header + block descriptor
        boolean dbd = (c[1] & 8) != 0; u8 d[12]; memset (d, 0, sizeof d); u8 bdl = dbd ? 0 : 8;
        d[0] = (u8) (3 + bdl); d[2] = t.wp ? 0x80 : 0; d[3] = bdl;
        if (!dbd) { d[5] = (u8) (t.blocks >> 16); d[6] = (u8) (t.blocks >> 8); d[7] = (u8) t.blocks; d[9] = (u8) (t.bs >> 16); d[10] = (u8) (t.bs >> 8); d[11] = (u8) t.bs; }
        u32 total = 4u + bdl, a = c[4] ? c[4] : total; if (a > total) a = total; if (a > len) a = len; memcpy (buf, d, a); *xfer = a; return ST_GOOD; }
    case 0xC2: { u8 d[10]; memset (d, 0, sizeof d); u32 a = c[4] ? c[4] : 10; if (a > 10) a = 10; if (a > len) a = len;   // Sharp vendor: drive params
        if (dir == 1) { memcpy (buf, d, a); *xfer = a; } else *xfer = len; return ST_GOOD; }
    default:
        sense_set (t, SK_ILLEGAL_REQUEST, ASC_INVALID_CMD, 0); return ST_CHECK_CONDITION;
    }
    // READ / WRITE / VERIFY
    if (lba + n > t.blocks) { sense_set (t, SK_ILLEGAL_REQUEST, ASC_LBA_RANGE, 0); t.errors++; return ST_CHECK_CONDITION; }
    u32 bytes = n * t.bs; if (bytes > len) bytes = len;
    u32 nb = bytes / t.bs; if (!nb) { *xfer = 0; return ST_GOOD; }
    if (op == 0x08 || op == 0x28)
    {
        if (dir != 1) { sense_set (t, SK_ILLEGAL_REQUEST, ASC_INVALID_CMD, 0); return ST_CHECK_CONDITION; }
        if (!io_request (0, (unsigned) (&t - s_t), lba, nb, buf)) { sense_set (t, SK_MEDIUM_ERROR, 0x11, 0); t.errors++; return ST_CHECK_CONDITION; }
        // present every HD as removable: X68SCSI1 header byte +$0F 0 -> 1 on the wire (the file is untouched)
        if (!t.cdrom && lba == 0 && t.bs >= 16 && memcmp (buf, "X68SCSI1", 8) == 0 && buf[0x0F] == 0) buf[0x0F] = 1;
        t.reads++; *xfer = nb * t.bs; return ST_GOOD;
    }
    if (op == 0x2F) { *xfer = dir == 2 ? nb * t.bs : 0; return ST_GOOD; }   // VERIFY: nothing to compare against
    if (dir != 2) { sense_set (t, SK_ILLEGAL_REQUEST, ASC_INVALID_CMD, 0); return ST_CHECK_CONDITION; }
    if (t.wp) { sense_set (t, SK_DATA_PROTECT, ASC_WRITE_PROTECT, 0); return ST_CHECK_CONDITION; }
    if (!io_request (1, (unsigned) (&t - s_t), lba, nb, buf)) { sense_set (t, SK_MEDIUM_ERROR, 0x03, 0); t.errors++; return ST_CHECK_CONDITION; }
    t.writes++; *xfer = nb * t.bs; return ST_GOOD;
}

void vscsi_worker (void)
{
    for (;;)
    {
        emu68k_vscsi_req_t r; u8 *buf; u32 bufsize;
        if (!emu68k_vscsi_take (&r, &buf, &bufsize)) { asm volatile ("yield"); continue; }
        s_cmds++;
        if (r.id >= VSCSI_IDS || !s_t[r.id].configured) { emu68k_vscsi_done (r.gen, 0, r.len, -1); continue; }
        u32 xfer = 0;
        unsigned st = execute (s_t[r.id], r.cdb, r.dir, buf, r.len, &xfer);
        CdbLog &L = s_log[s_log_n++ & 511]; L.id = r.id; L.dir = r.dir; memcpy (L.cdb, r.cdb, 12); L.len = r.len; L.xfer = xfer; L.st = (u8) st;
        emu68k_vscsi_done (r.gen, st, r.len > xfer ? r.len - xfer : 0, 0);
    }
}

// ---- core 0: mount / eject / create ------------------------------------------
static void update_mask (void)
{
    unsigned m = 0;
    for (unsigned i = 0; i < VSCSI_IDS; i++) if (s_t[i].configured) m |= 1u << i;
    emu68k_vscsi_set_mask (m);
}

// X68000 reset: an ejected virtual ID stops being ours (it answered NOT READY so
// Human68k saw a removal; after a reset the machine enumerates afresh and a real
// device at that ID - the vhd68 at ID 0, say - must be reachable again)
void vscsi_on_reset (void)
{
    for (unsigned i = 0; i < VSCSI_IDS; i++) if (!s_t[i].present) s_t[i].configured = FALSE;
    update_mask ();
}

int vscsi_mount (unsigned id, const char *path, boolean wp)
{
    if (id >= VSCSI_IDS || !path || !path[0]) return -1;
    Target &t = s_t[id];
    if (t.present) vscsi_eject (id);
    char fp[160]; fs_path (path, fp, sizeof fp);
    unsigned l = strlen (fp);
    boolean cd = l > 4 && (strcmp (fp + l - 4, ".iso") == 0 || strcmp (fp + l - 4, ".ISO") == 0);
    if (cd) wp = TRUE;
    if (f_open (&t.fil, fp, wp ? FA_READ : (FA_READ | FA_WRITE)) != FR_OK) return -2;
    FSIZE_t sz = f_size (&t.fil);
    unsigned bs = cd ? 2048 : 512;
    if (!cd)                            // SASI dump: X68K table at 0x400 addressed in 256-byte sectors matching the size
    {
        u8 hdr[0x408]; UINT br = 0; memset (hdr, 0, sizeof hdr);
        if (f_read (&t.fil, hdr, sizeof hdr, &br) == FR_OK && br >= 0x408 && memcmp (hdr + 0x400, "X68K", 4) == 0)
        {
            u32 total = ((u32) hdr[0x404] << 24) | ((u32) hdr[0x405] << 16) | ((u32) hdr[0x406] << 8) | hdr[0x407];
            if ((u64) total * 256 == sz) bs = 256;
        }
    }
    if (sz < bs) { f_close (&t.fil); return -3; }
    t.cdrom = cd; t.wp = wp; t.bs = bs; t.blocks = (unsigned) (sz / bs);
    strncpy (t.path, path, sizeof t.path - 1); t.path[sizeof t.path - 1] = 0;
    t.sk = 0; t.asc = 0; t.ascq = 0;
    build_extents (t);
    DataSyncBarrier ();
    t.configured = TRUE; t.present = TRUE; t.ua = TRUE;     // media inserted -> UNIT ATTENTION
    update_mask ();
    CLogger::Get ()->Write ("vscsi", LogNotice, "ID %u: %s (%u x %u%s%s), %s", id, path, t.blocks, bs, cd ? ", CD-ROM" : "", wp ? ", read-only" : "",
                            t.direct ? (t.n_ext == 1 ? "contiguous, direct card access" : "direct card access") : "through FatFs (fragmented or not FAT16/32)");
    if (t.direct && t.n_ext > 1) CLogger::Get ()->Write ("vscsi", LogNotice, "ID %u: image in %u runs", id, t.n_ext);
    return 0;
}

int vscsi_eject (unsigned id)
{
    if (id >= VSCSI_IDS) return -1;
    Target &t = s_t[id];
    if (!t.present) return 0;
    t.present = FALSE; DataSyncBarrier ();
    f_close (&t.fil);
    t.direct = FALSE; delete [] t.ext; t.ext = 0; t.n_ext = 0;
    t.blocks = 0; t.path[0] = 0; t.ua = TRUE;                // media removed -> UNIT ATTENTION, the ID stays ours (NOT READY)
    CLogger::Get ()->Write ("vscsi", LogNotice, "ID %u: ejected", id);
    return 0;
}

const vscsi_slot_info *vscsi_slot (unsigned id)
{
    static vscsi_slot_info info;
    if (id >= VSCSI_IDS) return 0;
    Target &t = s_t[id];
    info.mounted = t.present; info.cdrom = t.cdrom; info.wp = t.wp; info.block_size = t.bs; info.blocks = t.blocks;
    strncpy (info.path, t.path, sizeof info.path); info.path[sizeof info.path - 1] = 0;
    return &info;
}

int vscsi_new_image (const char *path, unsigned mb)
{
    if (!path || !path[0] || mb < 1 || mb > 4095) return -1;
    char fp[160]; fs_path (path, fp, sizeof fp);
    FIL f; FILINFO fi;
    { char d[160]; strncpy (d, fp, sizeof d - 1); d[sizeof d - 1] = 0;   // make the folder if the path has one
      char *last = 0; for (char *q = d; *q; q++) if (*q == '/') last = q;
      if (last && last > d + 3) { *last = 0; f_mkdir (d); } }
    if (f_stat (fp, &fi) == FR_OK) return -2;                // exists
    if (f_open (&f, fp, FA_WRITE | FA_CREATE_NEW) != FR_OK) return -3;
    FSIZE_t size = (FSIZE_t) mb * 1024 * 1024;
    // seeking past the end allocates the clusters without writing them (circle's FatFs has no f_expand);
    // then zero the first 1 MB so no stale header / partition table / driver area is left from a deleted image
    if (f_lseek (&f, size) != FR_OK || f_tell (&f) != size) { f_close (&f); f_unlink (fp); return -4; }
    if (f_lseek (&f, 0) != FR_OK) { f_close (&f); f_unlink (fp); return -4; }
    static u8 z[16384]; memset (z, 0, sizeof z);
    UINT bw; FSIZE_t done = 0, head = size < 1048576 ? size : 1048576;
    while (done < head)
    {
        UINT n = (UINT) (head - done < sizeof z ? head - done : sizeof z);
        if (f_write (&f, z, n, &bw) != FR_OK || bw != n) { f_close (&f); return -5; }
        done += n;
        CScheduler::Get ()->Yield ();                         // keep the main loop (watchdog) alive
    }
    f_sync (&f); f_close (&f);
    return 0;
}

void vscsi_status_json (CString &Out)
{
    Out = "\"ids\":[";
    for (unsigned i = 0; i < VSCSI_IDS; i++)
    {
        Target &t = s_t[i]; CString e;
        if (t.present)
        {
            CString name; const char *b = t.path; for (const char *p = t.path; *p; p++) if (*p == '/') b = p + 1;
            for (const char *p = b; *p; p++) { if (*p == '"' || *p == '\\') name.Append ("\\"); char c[2] = {*p, 0}; name.Append (c); }
            e.Format ("{\"mounted\":true,\"name\":\"%s\",\"path\":\"%s\",\"cd\":%s,\"mb\":%u,\"wp\":%s,\"direct\":%s,\"runs\":%u}", (const char *) name, t.path, t.cdrom ? "true" : "false",
                      (unsigned) (((u64) t.blocks * t.bs) >> 20), t.wp ? "true" : "false", t.direct ? "true" : "false", t.n_ext);
        }
        else e.Format ("{\"mounted\":false%s}", t.configured ? ",\"empty\":true" : "");
        Out.Append (e); if (i + 1 < VSCSI_IDS) Out.Append (",");
    }
    Out.Append ("]");
}

// console 'vsb <id> [MB]': the card path alone, through io_do (direct runs or
// FatFs, whichever the image uses): sequential read of the image in 64 KB
// requests, then the same data written back to the same place (so the image
// is unchanged) in 64 KB requests.
void vscsi_bench (unsigned id, unsigned mb, CString &Out)
{
    if (id >= VSCSI_IDS || !s_t[id].present) { Out = "vsb: ID not mounted\r\n"; return; }
    u8 *buf = new u8[65536]; u64 hz = vmpu68_tick_hz ();
    if (!buf) { Out = "vsb: out of memory\r\n"; return; }
    Target &t = s_t[id];
    if (!mb) mb = 4; u64 bytes = (u64) mb << 20; if (bytes > (u64) t.blocks * t.bs) bytes = (u64) t.blocks * t.bs;
    u32 per = 65536 / t.bs, done, w = 0; IoReq r; r.id = id; r.buf = buf; r.count = per;
    u64 t0 = vmpu68_ticks ();
    for (done = 0; (u64) (done + per) * t.bs <= bytes; done += per) { r.op = 0; r.lba = done; if (!io_do (r)) break; }
    u64 t1 = vmpu68_ticks (), t2 = t1, t3 = t1;
    if (!t.wp)
    {
        for (w = 0; (u64) (w + per) * t.bs <= bytes; w += per)
        {
            r.op = 0; r.lba = w; if (!io_do (r)) break;                  // the block's own data ...
            u64 a = vmpu68_ticks (); r.op = 1; int ok = io_do (r); t3 += vmpu68_ticks () - a;   // ... written back in place
            if (!ok) break;
        }
    }
    Out.Format ("vsb: %s | read %u KB in %u ms = %u KB/s | write (same data back) %u KB in %u ms = %u KB/s%s\r\n",
                t.direct ? (t.n_ext == 1 ? "direct, contiguous" : "direct") : "FatFs",
                (unsigned) (((u64) done * t.bs) >> 10), (unsigned) ((t1 - t0) * 1000 / hz), (unsigned) ((u64) done * t.bs * hz / 1024 / (t1 - t0 + 1)),
                (unsigned) (((u64) w * t.bs) >> 10), (unsigned) ((t3 - t2) * 1000 / hz), (unsigned) ((u64) w * t.bs * hz / 1024 / (t3 - t2 + 1)),
                t.wp ? " (read-only: no write test)" : "");
    delete [] buf;
}
void vscsi_init (void) { memset (&s_io, 0, sizeof s_io); }
void vscsi_start_io_task (void) { if (!s_pIoTask) s_pIoTask = new CVscsiIoTask; }

void vscsi_stats (CString &Out)
{
    emu68k_vscsi_stats_t s; emu68k_vscsi_stats (&s);
    u32 hook, oldv, kept, resets; unsigned mask = emu68k_vscsi_hook_info (&hook, &oldv, &kept, &resets);
    u64 hz = vmpu68_tick_hz (); if (!hz) hz = 1;
    Out.Format ("vscsi: port %u commands (overrun %u, too long %u, state %u), worker %u, io reads %u writes %u failed %u (%llu ms on the card, %u direct), "
                "last busy %u us commit %u us, avg busy %u us commit %u us, chunks fast %u slow %u\r\n"
                "       hook %06X old vector %06X kept %u resets %u mask %02X\r\n",
                s.count, s.overrun, s.toolong, s.state, s_cmds, s_io_reads, s_io_writes, s_io_fail, (unsigned long long) (s_io_ticks * 1000 / hz), s_io_direct,
                s.last_busy_us, s.last_commit_us, s.avg_busy_us, s.avg_commit_us, s.fast, s.slow, hook, oldv, kept, resets, mask);
    { u32 tr[64]; unsigned n = emu68k_vscsi_trace (tr, 64); CString L ("       passed on (d1:d4): ");
      for (unsigned i = 0; i < n; i++) { CString e; e.Format ("%02X:%X=%03X ", tr[i] >> 16, tr[i] & 0xF, (tr[i] >> 4) & 0xFFF); L.Append (e); }   // subfunction:ID=result (low 12 bits of d0, FFF = -1)
      L.Append ("\r\n"); Out.Append (L); }
    vscsi_log (32, Out);
    for (unsigned i = 0; i < VSCSI_IDS; i++)
    {
        Target &t = s_t[i]; if (!t.configured) continue;
        CString L; L.Format ("       ID %u: %s%s %u x %u, reads %u writes %u errors %u%s, %s", i, t.present ? t.path : "(ejected)", t.cdrom ? " CD" : "", t.blocks, t.bs, t.reads, t.writes, t.errors, t.ua ? ", UA pending" : "",
                             !t.present ? "-" : t.direct ? (t.n_ext == 1 ? "direct (contiguous)" : "direct") : "FatFs");
        if (t.present && t.direct && t.n_ext > 1) { CString R; R.Format (" in %u runs", t.n_ext); L.Append (R); }
        L.Append ("\r\n");
        Out.Append (L);
    }
}

// console 'vsl [n]': the last n commands (up to 512); 'vsl 0' clears the log
void vscsi_log (unsigned n, CString &Out)
{
    if (n == 0) { s_log_n = 0; Out.Append ("vscsi: command log cleared\r\n"); return; }
    if (n > 512) n = 512;
    for (unsigned k = s_log_n > n ? s_log_n - n : 0; k < s_log_n; k++)
    {
        CdbLog &L = s_log[k & 511]; CString C;
        unsigned n = (L.cdb[0] >> 5) == 1 || (L.cdb[0] >> 5) == 2 ? 10 : (L.cdb[0] >> 5) == 5 ? 12 : 6;
        for (unsigned i = 0; i < n; i++) { CString b; b.Format ("%02X", L.cdb[i]); C.Append (b); }
        CString Ln; Ln.Format ("       %3u: ID%u %s dir%u len %u xfer %u st %02X\r\n", k, L.id, (const char *) C, L.dir, L.len, L.xfer, L.st);
        Out.Append (Ln);
    }
}
