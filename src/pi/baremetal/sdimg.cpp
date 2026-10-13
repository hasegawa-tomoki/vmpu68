// SPDX-License-Identifier: MIT
// Image volume on the SD card: see sdimg.h.
#include "sdimg.h"
#include "vscsi.h"
#include "config.h"
#include "netfetch.h"
#include <circle/devicenameservice.h>
#include <circle/fs/partition.h>
#include <circle/logger.h>
#include <circle/util.h>
#include <circle/sched/scheduler.h>
#include <fatfs/ff.h>

static FATFS   s_fs2;
static boolean s_img;                   // SD2: mounted
static u64     s_card;                  // card size in bytes
static u32     s_p1_start, s_p1_count, s_p2_start, s_p2_count;   // MBR entries 1 and 2 (sectors)
static const char *From = "sdimg";

static CDevice *card_dev (void) { return CDeviceNameService::Get ()->GetDevice ("emmc1", TRUE); }
static u32 le32 (const u8 *p) { return (u32) p[0] | ((u32) p[1] << 8) | ((u32) p[2] << 16) | ((u32) p[3] << 24); }
static void put16 (u8 *p, u32 v) { p[0] = (u8) v; p[1] = (u8) (v >> 8); }
static void put32 (u8 *p, u32 v) { p[0] = (u8) v; p[1] = (u8) (v >> 8); p[2] = (u8) (v >> 16); p[3] = (u8) (v >> 24); }

static boolean read_mbr (u8 *mbr)
{
    CDevice *d = card_dev ();
    if (!d) return FALSE;
    d->Seek (0);
    if (d->Read (mbr, 512) != 512) return FALSE;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return FALSE;
    s_p1_start = le32 (mbr + 0x1C6); s_p1_count = le32 (mbr + 0x1CA);
    s_p2_start = le32 (mbr + 0x1D6); s_p2_count = le32 (mbr + 0x1DA);
    return TRUE;
}

static boolean mount_img (CDevice *dev)
{
    CDeviceNameService::Get ()->AddDevice ("emmc2", dev, TRUE);      // FatFs volume 1 ("SD2:") is diskio's "emmc2"
    if (f_mount (&s_fs2, "SD2:", 1) != FR_OK) return FALSE;
    s_img = TRUE;
    return TRUE;
}

void sdimg_init (u64 card_bytes)
{
    s_card = card_bytes;
    static u8 mbr[512] __attribute__ ((aligned (16)));
    if (!read_mbr (mbr)) { CLogger::Get ()->Write (From, LogWarning, "cannot read the MBR"); return; }
    if (!s_p2_count)
    {
        f_mkdir ("SD:/vscsi");                                               // images live there until the volume exists
        u32 total = (u32) (s_card / 512), end = s_p1_start + s_p1_count;
        CLogger::Get ()->Write (From, LogNotice, "image volume: none (card %u MB, %u MB after the boot partition)", (unsigned) (s_card >> 20), total > end ? (total - end) / 2048 : 0u);
        return;
    }
    CDevice *p2 = CDeviceNameService::Get ()->GetDevice ("emmc1-2", TRUE);
    if (!p2) { CLogger::Get ()->Write (From, LogWarning, "image volume: partition 2 in the MBR but no device"); return; }
    if (mount_img (p2))
        CLogger::Get ()->Write (From, LogNotice, "image volume SD2: %u MB", s_p2_count / 2048);
    else
        CLogger::Get ()->Write (From, LogWarning, "image volume: partition 2 (%u MB) does not mount", s_p2_count / 2048);
}

boolean sdimg_present (void) { return s_img; }

const char *sdimg_path (const char *p, char *buf, unsigned n)
{
    if (!p) p = "";
    if ((p[0] == 'S' && p[1] == 'D' && (p[2] == ':' || (p[2] == '2' && p[3] == ':'))))
    { strncpy (buf, p, n - 1); buf[n - 1] = 0; return buf; }
    CString s;
    if (s_img)
    {
        if (strncmp (p, "/vscsi/", 7) == 0) p += 6;               // old boot-partition paths: same name on the image volume
        s.Format ("SD2:%s%s", p[0] == '/' ? "" : "/", p);
    }
    else if (strncmp (p, "/vscsi", 6) == 0)
        s.Format ("SD:%s", p);
    else
        s.Format ("SD:/vscsi%s%s", p[0] == '/' ? "" : "/", p);                // no image volume: the boot partition's /vscsi folder
    strncpy (buf, (const char *) s, n - 1); buf[n - 1] = 0;
    return buf;
}

// ---- FAT32 formatter (circle's FatFs is built without f_mkfs) --------------
// One FAT32 volume filling the partition: 32 reserved sectors, 2 FATs, the
// cluster size the largest of 64 KB .. 4 KB that still gives FAT32's minimum
// of 65525 clusters.  Writes through the partition device (sector 0 = BPB).
static boolean fat32_format (CDevice *dev, u32 hidden, u32 nsec, CString &Err)
{
    u32 rsv = 32, csize = 128, fatsz = 0, clusters = 0;
    for (;;)
    {
        fatsz = 0;
        for (int it = 0; it < 4; it++) { u32 data = nsec - rsv - 2 * fatsz; clusters = data / csize; fatsz = ((clusters + 2) * 4 + 511) / 512; }
        clusters = (nsec - rsv - 2 * fatsz) / csize;
        if (clusters >= 65525 + 16 || csize == 8) break;
        csize /= 2;
    }
    if (clusters < 65525 + 16) { Err = "too small for FAT32 (256 MB or more needed)"; return FALSE; }
    u8 *zero = new u8[65536];                                  // heap: the kernel's static data must stay under KERNEL_MAX_SIZE
    if (!zero) { Err = "out of memory"; return FALSE; }
    memset (zero, 0, 65536);
    // zero the reserved area, both FATs and the root cluster
    u32 zsec = rsv + 2 * fatsz + csize, done = 0;
    while (done < zsec)
    {
        u32 n = zsec - done; if (n > 65536 / 512) n = 65536 / 512;
        dev->Seek ((u64) done * 512);
        if (dev->Write (zero, n * 512) != (int) (n * 512)) { Err.Format ("write failed at sector %u", done); delete [] zero; return FALSE; }
        done += n;
        CScheduler::Get ()->Yield ();
    }
    delete [] zero;
    static u8 bs[512] __attribute__ ((aligned (16)));
    memset (bs, 0, sizeof bs);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90; memcpy (bs + 3, "MSWIN4.1", 8);
    put16 (bs + 11, 512); bs[13] = (u8) csize; put16 (bs + 14, rsv); bs[16] = 2; bs[21] = 0xF8;
    put16 (bs + 24, 63); put16 (bs + 26, 255); put32 (bs + 28, hidden); put32 (bs + 32, nsec); put32 (bs + 36, fatsz);
    put32 (bs + 44, 2); put16 (bs + 48, 1); put16 (bs + 50, 6);
    bs[64] = 0x80; bs[66] = 0x29; put32 (bs + 67, 0x56A68001u ^ nsec);
    memcpy (bs + 71, "VMPU68 IMG ", 11); memcpy (bs + 82, "FAT32   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;
    static u8 fsi[512] __attribute__ ((aligned (16)));
    memset (fsi, 0, sizeof fsi);
    put32 (fsi, 0x41615252); put32 (fsi + 484, 0x61417272); put32 (fsi + 488, clusters - 1); put32 (fsi + 492, 3); put32 (fsi + 508, 0xAA550000);
    static u8 fat0[512] __attribute__ ((aligned (16)));
    memset (fat0, 0, sizeof fat0);
    put32 (fat0, 0x0FFFFFF8); put32 (fat0 + 4, 0x0FFFFFFF); put32 (fat0 + 8, 0x0FFFFFFF);   // media, EOC, root directory = cluster 2
    struct { u32 sec; const u8 *d; } w[] = { { 0, bs }, { 6, bs }, { 1, fsi }, { 7, fsi }, { rsv, fat0 }, { rsv + fatsz, fat0 } };
    for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++)
    {
        dev->Seek ((u64) w[i].sec * 512);
        if (dev->Write (w[i].d, 512) != 512) { Err.Format ("write failed at sector %u", w[i].sec); return FALSE; }
    }
    CLogger::Get ()->Write (From, LogNotice, "formatted: %u MB, %u KB clusters, %u clusters, FAT %u sectors", nsec / 2048, csize / 2, clusters, fatsz);
    return TRUE;
}

static int copy_file (const char *src, const char *dst)
{
    FIL a, b; UINT n, m;
    if (f_open (&a, src, FA_READ) != FR_OK) return -1;
    if (f_open (&b, dst, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) { f_close (&a); return -2; }
    u8 *buf = new u8[65536];
    int rc = buf ? 0 : -6;
    while (!rc)
    {
        if (f_read (&a, buf, 65536, &n) != FR_OK) { rc = -3; break; }
        if (!n) break;
        if (f_write (&b, buf, n, &m) != FR_OK || m != n) { rc = -4; break; }
        CScheduler::Get ()->Yield ();
    }
    delete [] buf;
    f_close (&a);
    if (f_close (&b) != FR_OK && !rc) rc = -5;
    if (rc) f_unlink (dst);
    return rc;
}

// the images that lived under SD:/vscsi move to the root of the new volume
static void migrate (void)
{
    struct { boolean had; char path[128]; boolean ro; } was[7];
    for (unsigned id = 0; id < 7; id++)
    {
        const vscsi_slot_info *s = vscsi_slot (id);
        was[id].had = s && s->mounted && strncmp (s->path, "/vscsi/", 7) == 0;
        if (was[id].had) { strncpy (was[id].path, s->path, sizeof was[id].path - 1); was[id].path[sizeof was[id].path - 1] = 0; was[id].ro = s->wp; vscsi_eject (id); }
    }
    DIR d; FILINFO fi; unsigned moved = 0, failed = 0;
    if (f_opendir (&d, "SD:/vscsi") == FR_OK)
    {
        while (f_readdir (&d, &fi) == FR_OK && fi.fname[0])
        {
            if (fi.fattrib & AM_DIR) continue;
            CString src, dst; src.Format ("SD:/vscsi/%s", fi.fname); dst.Format ("SD2:/%s", fi.fname);
            int rc = copy_file (src, dst);
            if (rc == 0) { f_unlink (src); moved++; }
            else { failed++; CLogger::Get ()->Write (From, LogWarning, "move %s: failed (%d)", fi.fname, rc); }
        }
        f_closedir (&d);
    }
    CLogger::Get ()->Write (From, LogNotice, "moved %u image file(s) from SD:/vscsi%s", moved, failed ? " (some failed, left in place)" : "");
    for (unsigned id = 0; id < 7; id++)
    {
        if (!was[id].had) continue;
        const char *np = was[id].path + 6;                                    // "/vscsi/x.hds" -> "/x.hds"
        int rc = vscsi_mount (id, np, was[id].ro);
        if (rc == 0) cfg_set_vscsi (id, np, was[id].ro);
        else { rc = vscsi_mount (id, was[id].path, was[id].ro); if (rc) cfg_set_vscsi (id, "", 0); }   // not moved: still on SD:
    }
    cfg_save ();
}

int sdimg_expand (CString &Err)
{
    if (s_img) { Err = "the image volume already exists"; return -1; }
    static u8 mbr[512] __attribute__ ((aligned (16)));
    if (!read_mbr (mbr)) { Err = "cannot read the MBR"; return -2; }
    if (s_p2_count || le32 (mbr + 0x1E6 + 4) || le32 (mbr + 0x1F6 + 4)) { Err = "the card already has more partitions"; return -2; }
    u32 total = (u32) (s_card / 512), start = (s_p1_start + s_p1_count + 2047) & ~2047u;
    if (total < start + 524288) { Err = "not enough room after the boot partition (256 MB or more needed)"; return -3; }
    u32 count = total - start;
    CDevice *c = card_dev ();
    CPartition *p = new CPartition (c, start, count);
    if (!p) { Err = "out of memory"; return -4; }
    if (!fat32_format (p, start, count, Err)) { delete p; return -5; }
    u8 *e = mbr + 0x1CE;
    memset (e, 0, 16);
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF; e[4] = 0x0C; e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    put32 (e + 8, start); put32 (e + 12, count);
    c->Seek (0);
    if (c->Write (mbr, 512) != 512) { Err = "cannot write the MBR"; delete p; return -6; }
    if (!read_mbr (mbr) || s_p2_start != start) { Err = "the MBR did not take"; delete p; return -6; }
    if (!mount_img (p)) { Err = "the new volume does not mount"; return -7; }
    CLogger::Get ()->Write (From, LogNotice, "image volume SD2: %u MB made (sectors %u..%u)", count / 2048, start, start + count - 1);
    migrate ();
    return 0;
}

void sdimg_info_json (CString &Out)
{
    u32 total = (u32) (s_card / 512), end = s_p1_start + s_p1_count, spare = total > end ? total - end : 0;
    unsigned img_total = 0, img_free = 0;
    if (s_img)
    {
        DWORD nfree = 0; FATFS *fs = 0;
        if (f_getfree ("SD2:", &nfree, &fs) == FR_OK && fs)
        {
            img_total = (unsigned) (((u64) (fs->n_fatent - 2) * fs->csize * 512) >> 20);
            img_free = (unsigned) (((u64) nfree * fs->csize * 512) >> 20);
        }
    }
    Out.Format ("{\"card_mb\":%u,\"boot_mb\":%u,\"img\":%s,\"img_total_mb\":%u,\"img_free_mb\":%u,\"spare_mb\":%u,\"expandable\":%s}",
                (unsigned) (s_card >> 20), s_p1_count / 2048, s_img ? "true" : "false", img_total, img_free,
                s_img ? 0u : spare / 2048, (!s_img && !s_p2_count && spare >= 524288) ? "true" : "false");
}

static void json_str (CString &Out, const char *s)
{
    Out.Append ("\"");
    for (const char *p = s; *p; p++)
    {
        char c[2] = { *p, 0 };
        if (*p == '"' || *p == '\\') Out.Append ("\\");
        if ((unsigned char) *p < 0x20) { CString u; u.Format ("\\u%04X", (unsigned char) *p); Out.Append (u); continue; }
        Out.Append (c);
    }
    Out.Append ("\"");
}

int sdimg_list_json (const char *dir, CString &Out)
{
    char fp[160]; sdimg_path (dir && dir[0] ? dir : "/", fp, sizeof fp);
    DIR d; FILINFO fi;
    if (f_opendir (&d, fp) != FR_OK) { Out = "{\"ok\":false,\"error\":\"cannot open folder\"}"; return -1; }
    Out = "{\"ok\":true,\"dir\":"; json_str (Out, dir && dir[0] ? dir : "/"); Out.Append (",\"files\":[");
    unsigned n = 0;
    while (f_readdir (&d, &fi) == FR_OK && fi.fname[0])
    {
        if (Out.GetLength () > 60000) break;                      // response buffer limit
        CString e; if (n++) Out.Append (",");
        Out.Append ("{\"n\":"); json_str (Out, fi.fname);
        e.Format (",\"s\":%u,\"d\":%u,\"t\":\"%04u-%02u-%02u %02u:%02u\"}", (unsigned) fi.fsize, (fi.fattrib & AM_DIR) ? 1u : 0u,
                  1980 + (fi.fdate >> 9), (fi.fdate >> 5) & 15, fi.fdate & 31, fi.ftime >> 11, (fi.ftime >> 5) & 63);
        Out.Append (e);
    }
    f_closedir (&d);
    Out.Append ("]}");
    return 0;
}

static boolean being_fetched (const char *fp)
{
    char b[160]; const char *d = netfetch_busy_path ();
    return d[0] && strcmp (sdimg_path (d, b, sizeof b), fp) == 0;
}

int sdimg_remove (const char *p)
{
    if (!p || !p[0] || strcmp (p, "/") == 0) return -1;
    char fp[160]; sdimg_path (p, fp, sizeof fp);
    if (being_fetched (fp)) return -2;
    for (unsigned id = 0; id < 7; id++)                       // a mounted image cannot go
    {
        const vscsi_slot_info *s = vscsi_slot (id); char mp[160];
        if (s && s->mounted && strcmp (sdimg_path (s->path, mp, sizeof mp), fp) == 0) return -2;
    }
    return f_unlink (fp) == FR_OK ? 0 : -3;
}

int sdimg_mkdir (const char *p)
{
    if (!p || !p[0]) return -1;
    char fp[160]; sdimg_path (p, fp, sizeof fp);
    return f_mkdir (fp) == FR_OK ? 0 : -3;
}

int sdimg_rename (const char *p, const char *to)
{
    if (!p || !p[0] || !to || !to[0]) return -1;
    char a[160], b[160]; sdimg_path (p, a, sizeof a); sdimg_path (to, b, sizeof b);
    if (being_fetched (a)) return -2;
    for (unsigned id = 0; id < 7; id++)
    {
        const vscsi_slot_info *s = vscsi_slot (id); char mp[160];
        if (s && s->mounted && strcmp (sdimg_path (s->path, mp, sizeof mp), a) == 0) return -2;
    }
    return f_rename (a, b) == FR_OK ? 0 : -3;
}
