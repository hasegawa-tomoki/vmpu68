// Minimal read-only ISO9660 reader.  See iso9660.h.
#include "iso9660.h"

#include <string.h>

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool rd(FIL *f, uint64_t off, void *buf, uint32_t n) {
    UINT br;
    return f_lseek(f, (FSIZE_t)off) == FR_OK &&
           f_read(f, buf, n, &br) == FR_OK && br == n;
}

bool iso_open(FIL *f, uint32_t *root_lba, uint32_t *root_size) {
    // Primary volume descriptor: sector 16, type 1, id "CD001".
    uint8_t d[190];
    if (!rd(f, 16u * ISO_SECTOR, d, sizeof d)) return false;
    if (d[0] != 1 || memcmp(d + 1, "CD001", 5) != 0) return false;
    // Root directory record lives at offset 156 (34 bytes):
    //   +2 extent LBA (little-endian half of the both-endian field)
    //   +10 data length
    *root_lba  = le32(d + 156 + 2);
    *root_size = le32(d + 156 + 10);
    return true;
}

// Copy a record's file identifier, stripping the ";version" suffix and a
// trailing '.' (files with no extension are stored as "NAME.").
static void copy_name(const uint8_t *id, unsigned len, char *out, unsigned n) {
    unsigned p = 0;
    for (unsigned i = 0; i < len && p < n - 1; i++) {
        if (id[i] == ';') break;
        out[p++] = (char)id[i];
    }
    if (p && out[p - 1] == '.') p--;
    out[p] = 0;
}

// Walk one directory extent, invoking cb per real entry.  Directory records
// never straddle a sector: a zero length byte means "skip to next sector".
static int walk(FIL *f, uint32_t lba, uint32_t size,
                void (*cb)(const iso_entry_t *e, void *ud), void *ud,
                const char *want, iso_entry_t *found) {
    static uint8_t sec[ISO_SECTOR];   // static: too big for the 2 KB core stack
    int count = 0;
    for (uint32_t done = 0; done < size; done += ISO_SECTOR) {
        uint32_t chunk = size - done < ISO_SECTOR ? size - done : ISO_SECTOR;
        if (!rd(f, (uint64_t)(lba + done / ISO_SECTOR) * ISO_SECTOR, sec, chunk))
            return -1;
        for (uint32_t off = 0; off + 33 < chunk;) {
            uint8_t len = sec[off];
            if (len == 0) break;                       // rest of sector is padding
            const uint8_t *r = sec + off;
            uint8_t nlen = r[32];
            if (33u + nlen > len) return -1;           // malformed
            // skip the "." (id 0x00) and ".." (id 0x01) self-references
            if (!(nlen == 1 && (r[33] == 0x00 || r[33] == 0x01))) {
                iso_entry_t e;
                memset(&e, 0, sizeof e);
                copy_name(r + 33, nlen, e.name, sizeof e.name);
                e.extent = le32(r + 2);
                e.size   = le32(r + 10);
                e.is_dir = (r[25] & 0x02) != 0;
                if (want) {
                    if (!strcmp(e.name, want)) { *found = e; return 1; }
                } else {
                    cb(&e, ud);
                    count++;
                }
            }
            off += len;
        }
    }
    return want ? 0 : count;
}

int iso_list_dir(FIL *f, uint32_t lba, uint32_t size,
                 void (*cb)(const iso_entry_t *e, void *ud), void *ud) {
    uint32_t rl, rs;
    if (lba == 0) {
        if (!iso_open(f, &rl, &rs)) return -1;
        lba = rl; size = rs;
    }
    if (size == 0 || size > 4u * 1024 * 1024) return -1;   // sanity bound
    return walk(f, lba, size, cb, ud, NULL, NULL);
}

bool iso_find(FIL *f, uint32_t lba, uint32_t size,
              const char *name, iso_entry_t *out) {
    uint32_t rl, rs;
    if (lba == 0) {
        if (!iso_open(f, &rl, &rs)) return false;
        lba = rl; size = rs;
    }
    if (size == 0 || size > 4u * 1024 * 1024) return false;
    return walk(f, lba, size, NULL, NULL, name, out) == 1;
}
