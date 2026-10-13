// Human68k FS — read side (partition + BPB + root directory).
// Derived from byte-level analysis of real images.  Endianness: X68 SCSI layer is BE,
// Human68k FS layer (BPB/FAT/dir) is mostly LE.
#include "human68k.h"

#include <string.h>

// ---- little / big endian readers --------------------------------------
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// ---- parsed geometry --------------------------------------------------
static h68k_io_t io_;
static bool      opened_;
static uint64_t  part_start_;        // bytes: first Human68k partition
static uint32_t  bytes_per_sector_;
static uint32_t  bytes_per_cluster_;
static uint8_t   fat_count_;
static uint16_t  reserved_sectors_;
static uint16_t  root_entries_;
static uint16_t  sectors_per_fat_;
static uint32_t  sector_count_;
static uint64_t  fat_start_;         // bytes
static uint64_t  root_start_;        // bytes
static uint64_t  data_start_;        // bytes: cluster 2 lands here
static uint32_t  max_cluster_;       // highest usable cluster index

int h68k_open(const h68k_io_t *io) {
    io_ = *io;
    opened_ = false;

    // --- X68K partition table ---------------------------------------
    // SCSI (.hds) keeps it at 0x800 addressed in 1024-byte logical sectors;
    // SASI (.hdf) at 0x400 addressed in 256-byte physical sectors.  The FS
    // that follows is identical, so only the partition base differs.
    uint8_t hdr[16];
    uint64_t pt_off;
    uint32_t pt_unit;
    if (io_.read(io_.ctx, 0x800, hdr, 4) && memcmp(hdr, "X68K", 4) == 0) {
        pt_off = 0x800; pt_unit = 1024;                  // SCSI
    } else if (io_.read(io_.ctx, 0x400, hdr, 4) && memcmp(hdr, "X68K", 4) == 0) {
        pt_off = 0x400; pt_unit = 256;                   // SASI
    } else {
        return -2;                                       // no X68K partition table
    }
    uint8_t ent[16];
    if (!io_.read(io_.ctx, pt_off + 0x10, ent, 16)) return -1;  // first partition entry
    uint32_t start_sec = be32(ent + 8);
    part_start_ = (uint64_t)start_sec * pt_unit;

    // --- BPB at the partition head ----------------------------------
    uint8_t b[0x24];
    if (!io_.read(io_.ctx, part_start_, b, sizeof b)) return -1;

    // Distinguish extended (OEM 15) from standard (OEM 8) by which decode
    // yields a sane bytes-per-sector.
    uint16_t bps_ext = be16(b + 0x12);
    if (bps_ext == 256 || bps_ext == 512 || bps_ext == 1024 || bps_ext == 2048) {
        bytes_per_sector_ = bps_ext;
        bytes_per_cluster_ = bps_ext * b[0x14];
        fat_count_ = b[0x15];
        reserved_sectors_ = be16(b + 0x16);
        root_entries_ = be16(b + 0x18);      // measured: entry count, not bytes
        sectors_per_fat_ = b[0x1d];
        uint16_t sc16 = be16(b + 0x1a);
        sector_count_ = sc16 ? sc16 : be32(b + 0x1e);
    } else {
        bytes_per_sector_ = le16(b + 0x0b);
        bytes_per_cluster_ = bytes_per_sector_ * b[0x0d];
        fat_count_ = b[0x0e];
        reserved_sectors_ = (uint16_t)(be16(b + 0x0f) + 1);
        root_entries_ = le16(b + 0x11);      // (standard BPB; verify on an 8-char-OEM image)
        sectors_per_fat_ = b[0x16];
        uint16_t sc16 = le16(b + 0x13);
        sector_count_ = sc16 ? sc16 : le32(b + 0x17);
    }
    if (bytes_per_sector_ == 0 || bytes_per_cluster_ == 0) return -3;

    fat_start_  = part_start_ + (uint64_t)reserved_sectors_ * bytes_per_sector_;
    root_start_ = fat_start_ + (uint64_t)fat_count_ * sectors_per_fat_ * bytes_per_sector_;
    uint32_t root_bytes = ((uint32_t)root_entries_ * 32 + bytes_per_sector_ - 1)
                          / bytes_per_sector_ * bytes_per_sector_;
    data_start_ = root_start_ + root_bytes;

    uint32_t spc = bytes_per_cluster_ / bytes_per_sector_;
    uint32_t data_clusters = spc ? sector_count_ / spc : 0;
    uint32_t fat_entries = (uint32_t)((uint64_t)sectors_per_fat_ * bytes_per_sector_ / 2);
    max_cluster_ = data_clusters + 1;                 // clusters run 2..data_clusters+1
    if (fat_entries && max_cluster_ > fat_entries - 1) max_cluster_ = fat_entries - 1;

    opened_ = true;
    return 0;
}

void h68k_close(void) { opened_ = false; }

// Human68k name = 8-char main field (0x00) + up to a 10-char continuation
// (0x0C, TwentyOne long names) + 3-char extension (0x08): "MAINcont.EXT".
// The continuation is only present when the 8-char main field is full.
static void format_name(const uint8_t *e, char *out, unsigned n) {
    unsigned p = 0;
    for (int k = 0; k < 8 && e[k] != ' ' && e[k]; k++) if (p < n - 1) out[p++] = (char)e[k];
    if (e[7] != ' ' && e[7])                          // main field full -> long name may continue
        for (int k = 0x0c; k < 0x16 && e[k] != ' ' && e[k]; k++) if (p < n - 1) out[p++] = (char)e[k];
    if (e[8] != ' ' && e[8]) {
        if (p < n - 1) out[p++] = '.';
        for (int k = 8; k < 11 && e[k] != ' ' && e[k]; k++) if (p < n - 1) out[p++] = (char)e[k];
    }
    out[p] = 0;
}

// ---- FAT read + cluster geometry (also used by list_dir) ---------------
static uint64_t clu_off(uint32_t c) { return data_start_ + (uint64_t)(c - 2) * bytes_per_cluster_; }
static bool fat_get(uint32_t c, uint16_t *v) {
    uint8_t b[2];
    if (!io_.read(io_.ctx, fat_start_ + (uint64_t)c * 2, b, 2)) return false;
    *v = (uint16_t)((b[0] << 8) | b[1]);              // FAT entries are big-endian
    return true;
}

static void emit_entry(const uint8_t *e, void (*cb)(const h68k_entry_t *, void *), void *ud) {
    h68k_entry_t ent;
    memset(&ent, 0, sizeof ent);
    format_name(e, ent.name, sizeof ent.name);
    ent.attr    = e[0x0b];
    ent.is_dir  = (e[0x0b] & 0x10) != 0;
    ent.size    = le32(e + 0x1c);
    ent.cluster = le16(e + 0x1a);
    cb(&ent, ud);
}

int h68k_list_dir(uint32_t dir_cluster, void (*cb)(const h68k_entry_t *e, void *ud), void *ud) {
    if (!opened_) return -1;
    int count = 0;
    if (dir_cluster == 0) {                           // root: fixed region
        for (uint16_t i = 0; i < root_entries_; i++) {
            uint8_t e[32];
            if (!io_.read(io_.ctx, root_start_ + (uint64_t)i * 32, e, 32)) return -1;
            if (e[0] == 0x00) break;
            if (e[0] == 0xE5 || e[0] == '.') continue;
            emit_entry(e, cb, ud);
            count++;
        }
    } else {                                          // subdir: follow the cluster chain
        uint32_t per = bytes_per_cluster_ / 32, clu = dir_cluster, guard = 0;
        while (clu >= 2 && clu < 0xFFF8 && guard++ < 1000000u) {
            for (uint32_t k = 0; k < per; k++) {
                uint8_t e[32];
                if (!io_.read(io_.ctx, clu_off(clu) + (uint64_t)k * 32, e, 32)) return -1;
                if (e[0] == 0x00) return count;
                if (e[0] == 0xE5 || e[0] == '.') continue;
                emit_entry(e, cb, ud);
                count++;
            }
            uint16_t nx;
            if (!fat_get(clu, &nx)) return -1;
            clu = nx;
        }
    }
    return count;
}

int h68k_list_root(void (*cb)(const h68k_entry_t *e, void *ud), void *ud) {
    return h68k_list_dir(0, cb, ud);
}

// Locate a named entry in a directory; return its first cluster + size.
static bool find_entry(uint32_t dir_cluster, const char *name, uint32_t *ocl, uint32_t *osz) {
    char nm[24];
    uint32_t per = bytes_per_cluster_ / 32, clu = dir_cluster, guard = 0;
    for (;;) {
        int nent = (dir_cluster == 0) ? root_entries_ : (int)per;
        for (int i = 0; i < nent; i++) {
            uint64_t off = (dir_cluster == 0) ? root_start_ + (uint64_t)i * 32
                                              : clu_off(clu) + (uint64_t)i * 32;
            uint8_t e[32];
            if (!io_.read(io_.ctx, off, e, 32)) return false;
            if (e[0] == 0x00) return false;
            if (e[0] == 0xE5 || e[0] == '.') continue;
            format_name(e, nm, sizeof nm);
            if (!strcmp(nm, name)) { *ocl = le16(e + 0x1a); *osz = le32(e + 0x1c); return true; }
        }
        if (dir_cluster == 0) return false;
        uint16_t nx; if (!fat_get(clu, &nx)) return false;
        if (nx < 2 || nx >= 0xFFF8 || guard++ > 1000000u) return false;
        clu = nx;
    }
}

int h68k_stat(uint32_t dir_cluster, const char *name, h68k_file_t *f, uint32_t *size) {
    if (!opened_) return -1;
    uint32_t cl, sz;
    if (!find_entry(dir_cluster, name, &cl, &sz)) return -1;
    f->cluster = cl; f->remaining = sz; f->clu_pos = 0;
    if (size) *size = sz;
    return 0;
}

int h68k_file_read(h68k_file_t *f, uint8_t *buf, uint32_t len) {
    if (!opened_) return -1;
    uint32_t got = 0;
    while (len > 0 && f->remaining > 0) {
        if (f->clu_pos >= bytes_per_cluster_) {       // advance to the next cluster
            uint16_t nx; if (!fat_get(f->cluster, &nx)) return -1;
            if (nx < 2 || nx >= 0xFFF8) break;
            f->cluster = nx; f->clu_pos = 0;
        }
        uint32_t avail = bytes_per_cluster_ - f->clu_pos;
        uint32_t chunk = len < avail ? len : avail;
        if (chunk > f->remaining) chunk = f->remaining;
        if (!io_.read(io_.ctx, clu_off(f->cluster) + f->clu_pos, buf + got, chunk)) return -1;
        got += chunk; len -= chunk; f->clu_pos += chunk; f->remaining -= chunk;
    }
    return (int)got;
}

// ---- FAT write (big-endian, mirrored into all copies) ------------------
static bool fat_set(uint32_t c, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    for (uint8_t k = 0; k < fat_count_; k++) {        // mirror into every FAT copy
        uint64_t off = fat_start_ + (uint64_t)k * sectors_per_fat_ * bytes_per_sector_ + (uint64_t)c * 2;
        if (!io_.write(io_.ctx, off, b, 2)) return false;
    }
    return true;
}

// Delete a plain file: mark its directory entry free and release the FAT
// chain.  Directories are refused.  Returns 0, -1 (not found / IO), -2 (dir).
// ---- entry helpers shared by the create/rename paths -------------------

// DOS-FAT timestamp applied to newly created entries (see h68k_set_stamp).
static uint16_t stamp_time_, stamp_date_;
void h68k_set_stamp(uint16_t dos_time, uint16_t dos_date) {
    stamp_time_ = dos_time;
    stamp_date_ = dos_date;
}
static void put_stamp(uint8_t *e) {                    // entry 0x16/0x18, LE
    e[0x16] = (uint8_t)stamp_time_; e[0x17] = (uint8_t)(stamp_time_ >> 8);
    e[0x18] = (uint8_t)stamp_date_; e[0x19] = (uint8_t)(stamp_date_ >> 8);
}

// Split "MAINcontinuation.EXT" into the 8 + 10 (TwentyOne) + 3 name fields.
// 0 = ok, -2 = empty / too long (max 18 stem + 3 extension).
static int parse_name83(const char *name, char nm[8], char n2[10], char ex[3]) {
    memset(nm, ' ', 8); memset(n2, 0, 10); memset(ex, ' ', 3);
    const char *dot = strrchr(name, '.');
    unsigned nlen = dot ? (unsigned)(dot - name) : (unsigned)strlen(name);
    unsigned elen = dot ? (unsigned)strlen(dot + 1) : 0;
    if (nlen == 0 || nlen > 18 || elen > 3) return -2;
    memcpy(nm, name, nlen < 8 ? nlen : 8);
    if (nlen > 8) memcpy(n2, name + 8, nlen - 8);
    if (elen) memcpy(ex, dot + 1, elen);
    return 0;
}

// True (1) if a directory holds no live entries (only "."/".."/free), 0 if it
// holds a file or subdir, -1 on I/O error.  Used to gate directory removal.
static int dir_is_empty(uint32_t clu) {
    uint32_t per = bytes_per_cluster_ / 32, guard = 0;
    while (clu >= 2 && clu < 0xFFF8 && guard++ < 1000000u) {
        for (uint32_t i = 0; i < per; i++) {
            uint8_t e[32];
            if (!io_.read(io_.ctx, clu_off(clu) + (uint64_t)i * 32, e, 32)) return -1;
            if (e[0] == 0x00) return 1;                 // no more entries
            if (e[0] == 0xE5 || e[0] == '.') continue;  // deleted, or "."/".."
            return 0;                                   // a live entry -> not empty
        }
        uint16_t nx; if (!fat_get(clu, &nx)) return -1;
        clu = nx;
    }
    return 1;
}

// Rename a file or directory in place: only the name fields (main 8 + ext 3 +
// TwentyOne continuation 10) of the entry are rewritten; attribute, stamp,
// cluster and size stay, so data and FAT are untouched and directories are
// safe to rename ("." / ".." reference the parent by cluster, not by name).
int h68k_rename(uint32_t dir_cluster, const char *name, const char *newname) {
    if (!opened_ || !io_.write) return -1;
    char nm[8], n2[10], ex[3], cur[24];
    if (parse_name83(newname, nm, n2, ex) != 0) return -2;
    if (strcmp(name, newname) != 0) {                 // same name = no-op below
        uint32_t cl, sz;
        if (find_entry(dir_cluster, newname, &cl, &sz)) return -5;
    }
    uint32_t per = bytes_per_cluster_ / 32, clu = dir_cluster, guard = 0;
    for (;;) {
        int nent = (dir_cluster == 0) ? root_entries_ : (int)per;
        for (int i = 0; i < nent; i++) {
            uint64_t off = (dir_cluster == 0) ? root_start_ + (uint64_t)i * 32
                                              : clu_off(clu) + (uint64_t)i * 32;
            uint8_t e[32];
            if (!io_.read(io_.ctx, off, e, 32)) return -1;
            if (e[0] == 0x00) return -1;
            if (e[0] == 0xE5 || e[0] == '.') continue;
            format_name(e, cur, sizeof cur);
            if (strcmp(cur, name)) continue;
            memcpy(e, nm, 8);
            memcpy(e + 8, ex, 3);
            memcpy(e + 0x0c, n2, 10);
            return io_.write(io_.ctx, off, e, 32) ? 0 : -1;
        }
        if (dir_cluster == 0) return -1;
        uint16_t nx; if (!fat_get(clu, &nx)) return -1;
        if (nx < 2 || nx >= 0xFFF8 || guard++ > 1000000u) return -1;
        clu = nx;
    }
}

// Remove a file, or an EMPTY directory (DOS RMDIR semantics: a directory that
// still holds files/subdirs is refused, never recursively deleted).  Returns
// 0 on success, -1 not found / I/O error, -6 directory not empty.
int h68k_delete(uint32_t dir_cluster, const char *name) {
    if (!opened_) return -1;
    char nm[24];
    uint32_t per = bytes_per_cluster_ / 32, clu = dir_cluster, guard = 0;
    for (;;) {
        int nent = (dir_cluster == 0) ? root_entries_ : (int)per;
        for (int i = 0; i < nent; i++) {
            uint64_t off = (dir_cluster == 0) ? root_start_ + (uint64_t)i * 32
                                              : clu_off(clu) + (uint64_t)i * 32;
            uint8_t e[32];
            if (!io_.read(io_.ctx, off, e, 32)) return -1;
            if (e[0] == 0x00) return -1;
            if (e[0] == 0xE5 || e[0] == '.') continue;
            format_name(e, nm, sizeof nm);
            if (strcmp(nm, name)) continue;
            if (e[0x0b] & 0x10) {                       // directory: only if empty
                int em = dir_is_empty(le16(e + 0x1a));
                if (em < 0) return -1;
                if (em == 0) return -6;                 // not empty -> refuse
            }
            uint32_t c = le16(e + 0x1a);
            e[0] = 0xE5;
            if (!io_.write(io_.ctx, off, e, 32)) return -1;
            uint32_t g2 = 0;
            while (c >= 2 && c < 0xFFF8 && g2++ < 1000000u) {
                uint16_t nx;
                if (!fat_get(c, &nx)) return -1;
                if (!fat_set(c, 0)) return -1;
                c = nx;
            }
            return 0;
        }
        if (dir_cluster == 0) return -1;
        uint16_t nx; if (!fat_get(clu, &nx)) return -1;
        if (nx < 2 || nx >= 0xFFF8 || guard++ > 1000000u) return -1;
        clu = nx;
    }
}

// In-RAM payload adapter, so the classic buffer API and the tests keep working.
struct h68k_membuf { const uint8_t *p; uint32_t n; };
static bool h68k_mem_src(void *ctx, uint32_t off, uint8_t *buf, uint32_t len) {
    struct h68k_membuf *m = ctx;
    if ((uint64_t)off + len > m->n) return false;
    memcpy(buf, m->p + off, len);
    return true;
}
int h68k_inject(const char *name83, const uint8_t *data, uint32_t size) {
    struct h68k_membuf m = { data, size };
    return h68k_inject_stream(name83, size, h68k_mem_src, &m);
}

static uint32_t find_free_cluster(void) {
    for (uint32_t c = 2; c <= max_cluster_; c++) {
        uint16_t v; if (!fat_get(c, &v)) return 0;
        if (v == 0) return c;
    }
    return 0;
}

// Locate a free directory slot's byte offset.  For a subdir, extend the
// cluster chain by one zeroed cluster if all entries are used.
static int find_dir_slot(uint32_t dir_cluster, uint64_t *slot_off) {
    if (dir_cluster == 0) {                           // root: fixed region
        for (uint16_t i = 0; i < root_entries_; i++) {
            uint8_t f; if (!io_.read(io_.ctx, root_start_ + (uint64_t)i * 32, &f, 1)) return -3;
            if (f == 0x00 || f == 0xE5) { *slot_off = root_start_ + (uint64_t)i * 32; return 0; }
        }
        return -5;                                    // root directory full
    }
    uint32_t per = bytes_per_cluster_ / 32, clu = dir_cluster, guard = 0;
    while (clu >= 2 && clu < 0xFFF8 && guard++ < 1000000u) {
        for (uint32_t k = 0; k < per; k++) {
            uint8_t f; if (!io_.read(io_.ctx, clu_off(clu) + (uint64_t)k * 32, &f, 1)) return -3;
            if (f == 0x00 || f == 0xE5) { *slot_off = clu_off(clu) + (uint64_t)k * 32; return 0; }
        }
        uint16_t nx; if (!fat_get(clu, &nx)) return -3;
        if (nx < 2 || nx >= 0xFFF8) {                 // end of chain -> extend
            uint32_t nc = find_free_cluster();
            if (!nc) return -4;
            static const uint8_t z[512] = {0};
            for (uint32_t p = 0; p < bytes_per_cluster_; p += sizeof z) {
                uint32_t w = (bytes_per_cluster_ - p < sizeof z) ? (bytes_per_cluster_ - p) : (uint32_t)sizeof z;
                if (!io_.write(io_.ctx, clu_off(nc) + p, z, w)) return -3;
            }
            if (!fat_set(clu, (uint16_t)nc) || !fat_set(nc, 0xFFFF)) return -3;
            *slot_off = clu_off(nc);
            return 0;
        }
        clu = nx;
    }
    return -3;
}

int h68k_inject_stream_at(const char *name83, uint32_t dir_cluster, uint32_t size,
                          h68k_src_fn src, void *ctx) {
    if (!opened_ || !io_.write) return -1;

    // name -> main(8) + TwentyOne continuation(<=10) + ext(3)
    char nm[8], n2[10], ex[3];
    if (parse_name83(name83, nm, n2, ex) != 0) return -2;

    uint32_t bpc = bytes_per_cluster_;
    uint32_t nclu = size ? (size + bpc - 1) / bpc : 0;

    // reserve the directory slot first (may consume a cluster to extend)
    uint64_t slot_off;
    int rs = find_dir_slot(dir_cluster, &slot_off);
    if (rs != 0) return rs;

    // enough free data clusters?
    uint32_t freecnt = 0;
    for (uint32_t c = 2; c <= max_cluster_ && freecnt < nclu; c++) {
        uint16_t v; if (!fat_get(c, &v)) return -3;
        if (v == 0) freecnt++;
    }
    if (freecnt < nclu) return -4;

    // allocate + stream data + chain FAT (only 512 B buffered)
    static uint8_t sbuf[512];
    uint32_t scan = 2, prev = 0, first = 0, off = 0;
    for (uint32_t j = 0; j < nclu; j++) {
        uint32_t c = 0;
        for (; scan <= max_cluster_; scan++) {
            uint16_t v; if (!fat_get(scan, &v)) return -3;
            if (v == 0) { c = scan++; break; }
        }
        if (!c) return -4;
        if (!first) first = c;
        if (prev && !fat_set(prev, (uint16_t)c)) return -3;
        uint64_t base = clu_off(c);
        for (uint32_t p = 0; p < bpc; p += 512) {
            uint32_t w = (bpc - p < 512) ? (bpc - p) : 512;
            uint32_t dl = (off < size) ? ((size - off < w) ? (size - off) : w) : 0;
            if (dl && !src(ctx, off, sbuf, dl)) return -3;
            if (dl < w) memset(sbuf + dl, 0, w - dl);
            if (!io_.write(io_.ctx, base + p, sbuf, w)) return -3;
            off += dl;
        }
        prev = c;
    }
    if (nclu && !fat_set(prev, 0xFFFF)) return -3;

    // directory entry (cluster/size little-endian; long name in 0x0C)
    uint8_t e[32];
    memset(e, 0, sizeof e);
    memcpy(e, nm, 8);
    memcpy(e + 8, ex, 3);
    e[0x0b] = 0x20;                                    // archive
    memcpy(e + 0x0c, n2, sizeof n2);                  // TwentyOne continuation
    put_stamp(e);
    uint16_t fc = (uint16_t)(nclu ? first : 0);
    e[0x1a] = (uint8_t)fc;       e[0x1b] = (uint8_t)(fc >> 8);
    e[0x1c] = (uint8_t)size;     e[0x1d] = (uint8_t)(size >> 8);
    e[0x1e] = (uint8_t)(size >> 16); e[0x1f] = (uint8_t)(size >> 24);
    if (!io_.write(io_.ctx, slot_off, e, 32)) return -3;
    return 0;
}

int h68k_inject_stream(const char *name83, uint32_t size, h68k_src_fn src, void *ctx) {
    return h68k_inject_stream_at(name83, 0, size, src, ctx);
}

// Move a plain file between directories: copy the raw 32-byte entry into a
// slot of the destination and free the source slot.  Data clusters and the
// FAT chain stay untouched.  0 = ok, -1 = not found / IO, -2 = refused
// (directory), -4 = destination full, -5 = name exists in destination.
int h68k_move(uint32_t src_dir, const char *name, uint32_t dst_dir) {
    if (!opened_ || !io_.write) return -1;
    if (src_dir == dst_dir) return 0;
    { uint32_t cl, sz; if (find_entry(dst_dir, name, &cl, &sz)) return -5; }

    char nm[24];
    uint32_t per = bytes_per_cluster_ / 32, clu = src_dir, guard = 0;
    for (;;) {
        int nent = (src_dir == 0) ? root_entries_ : (int)per;
        for (int i = 0; i < nent; i++) {
            uint64_t off = (src_dir == 0) ? root_start_ + (uint64_t)i * 32
                                          : clu_off(clu) + (uint64_t)i * 32;
            uint8_t e[32];
            if (!io_.read(io_.ctx, off, e, 32)) return -1;
            if (e[0] == 0x00) return -1;
            if (e[0] == 0xE5 || e[0] == '.') continue;
            format_name(e, nm, sizeof nm);
            if (strcmp(nm, name)) continue;
            if (e[0x0b] & 0x10) return -2;
            uint64_t slot_off;
            int rs = find_dir_slot(dst_dir, &slot_off);   // may extend dst dir
            if (rs != 0) return rs;
            if (!io_.write(io_.ctx, slot_off, e, 32)) return -1;
            e[0] = 0xE5;
            if (!io_.write(io_.ctx, off, e, 32)) return -1;
            return 0;
        }
        if (src_dir == 0) return -1;
        uint16_t nx; if (!fat_get(clu, &nx)) return -1;
        if (nx < 2 || nx >= 0xFFF8 || guard++ > 1000000u) return -1;
        clu = nx;
    }
}

// Create a subdirectory: allocate one cluster, initialise the "." / ".."
// entries and add a directory entry (attr 0x10) to the parent.
// 0 = ok, -2 bad name, -3 IO, -4 disk full, -5 already exists.
int h68k_mkdir(uint32_t dir_cluster, const char *name) {
    if (!opened_ || !io_.write) return -1;
    { uint32_t cl, sz; if (find_entry(dir_cluster, name, &cl, &sz)) return -5; }

    char nm[8], n2[10], ex[3];
    if (parse_name83(name, nm, n2, ex) != 0) return -2;

    uint64_t slot_off;
    int rs = find_dir_slot(dir_cluster, &slot_off);
    if (rs != 0) return rs;

    uint32_t c = find_free_cluster();
    if (!c) return -4;
    if (!fat_set(c, 0xFFFF)) return -3;

    static const uint8_t z[512] = {0};
    uint64_t base = clu_off(c);
    for (uint32_t p = 0; p < bytes_per_cluster_; p += 512) {
        uint32_t w = bytes_per_cluster_ - p < 512 ? bytes_per_cluster_ - p : 512;
        if (!io_.write(io_.ctx, base + p, z, w)) return -3;
    }
    uint8_t de[32];
    memset(de, 0, sizeof de); memset(de, ' ', 11);
    de[0] = '.'; de[0x0b] = 0x10;
    de[0x1a] = (uint8_t)c; de[0x1b] = (uint8_t)(c >> 8);
    if (!io_.write(io_.ctx, base, de, 32)) return -3;
    memset(de, 0, sizeof de); memset(de, ' ', 11);
    de[0] = '.'; de[1] = '.'; de[0x0b] = 0x10;
    de[0x1a] = (uint8_t)dir_cluster; de[0x1b] = (uint8_t)(dir_cluster >> 8);
    if (!io_.write(io_.ctx, base + 32, de, 32)) return -3;

    uint8_t e[32];
    memset(e, 0, sizeof e);
    memcpy(e, nm, 8); memcpy(e + 8, ex, 3);
    e[0x0b] = 0x10;                                   // directory
    memcpy(e + 0x0c, n2, sizeof n2);                  // TwentyOne continuation
    put_stamp(e);
    e[0x1a] = (uint8_t)c; e[0x1b] = (uint8_t)(c >> 8);
    if (!io_.write(io_.ctx, slot_off, e, 32)) return -3;
    return 0;
}
