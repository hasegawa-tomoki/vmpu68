// Minimal read-only ISO9660 (ECMA-119) directory reader, so the Web UI can
// browse a CD image (.iso) and pull files (e.g. FDD images) out of it.
// Level-1 primary volume descriptor only — no Joliet / Rock Ridge /
// multi-extent; names are reported with the ";version" suffix stripped.
//
// Clean-room implementation from the published ECMA-119 field layout.
// Directories are identified by their extent (LBA of 2048-byte sectors) and
// byte size, both taken from the parent record; lba==0 means "the root".
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <fatfs/ff.h>

#define ISO_SECTOR 2048u

typedef struct {
    char     name[64];       // "FOO.XDF" (";1" stripped)
    uint32_t size;           // file size in bytes (dirs: extent size)
    uint32_t extent;         // starting LBA
    bool     is_dir;
} iso_entry_t;

// Validate the image and locate the root directory.  Returns false if the
// file has no primary volume descriptor ("CD001" at sector 16).
bool iso_open(FIL *f, uint32_t *root_lba, uint32_t *root_size);

// List one directory ("." / ".." records are skipped).  lba==0 lists the
// root (root_size may then also be 0).  Returns entry count, -1 on error.
int iso_list_dir(FIL *f, uint32_t lba, uint32_t size,
                 void (*cb)(const iso_entry_t *e, void *ud), void *ud);

// Find `name` in a directory; fills *out on success.
bool iso_find(FIL *f, uint32_t lba, uint32_t size,
              const char *name, iso_entry_t *out);
