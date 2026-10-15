// Human68k filesystem access on a .hds image, for Web-UI file injection
// (phase 2).  Clean-room implementation of the on-disk format, derived from
// byte-level analysis of real .hds images.
//
// Block I/O is injected (h68k_io_t) so the exact same code runs on the Pico
// (over hdimage/FatFs) and on a host (over fread/fwrite) for testing.  All
// offsets are absolute byte offsets into the .hds.  Operates on an UNMOUNTED
// image only — no contention with the live SCSI engine.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool (*read)(void *ctx, uint64_t off, void *buf, uint32_t len);
    bool (*write)(void *ctx, uint64_t off, const void *buf, uint32_t len);
    void *ctx;
} h68k_io_t;

typedef struct {
    char     name[24];      // "NAME.EXT", NUL-terminated
    uint32_t size;
    uint32_t cluster;       // this entry's first cluster (descend into dirs)
    uint8_t  attr;          // raw attribute byte (0x10=dir, 0x08=vol, ...)
    bool     is_dir;
} h68k_entry_t;

// Parse the X68K partition table, first Human68k partition and its BPB.
// Returns 0 on success; negative if not a recognised Human68k SCSI disk.
int  h68k_open(const h68k_io_t *io);
void h68k_close(void);

// List a directory (dir_cluster==0 => root); cb runs once per entry ("." and
// ".." are skipped; long TwentyOne names are joined).  Returns count, or -1.
int  h68k_list_dir(uint32_t dir_cluster, void (*cb)(const h68k_entry_t *e, void *ud), void *ud);
int  h68k_list_root(void (*cb)(const h68k_entry_t *e, void *ud), void *ud);   // = dir 0

// Sequential file reader (for Web-UI download).  h68k_stat locates `name` in
// dir_cluster (0=root) and seeds `f` + *size; h68k_file_read then pulls bytes
// in order (following the FAT chain), 512 B of internal state only.
typedef struct { uint32_t cluster, remaining, clu_pos; } h68k_file_t;
int  h68k_stat(uint32_t dir_cluster, const char *name, h68k_file_t *f, uint32_t *size);
int  h68k_file_read(h68k_file_t *f, uint8_t *buf, uint32_t len);   // bytes read, 0=EOF, -1 err

// Streaming payload source: fill buf[0..len) with the `len` bytes at byte
// offset `off` of the file being injected.  Lets inject handle payloads far
// larger than RAM (e.g. 1–2 MB FDD images) — only 512 bytes are buffered
// internally, so the limit is the image's free space, not memory.
typedef bool (*h68k_src_fn)(void *ctx, uint32_t off, uint8_t *buf, uint32_t len);

// Inject a file into directory `dir_cluster` (0 => root), streaming from `src`.
// name83 may use TwentyOne long names (main up to 18 chars + ".EXT").  A full
// subdirectory is extended by one cluster.  0 on success, negative on error.
int  h68k_inject_stream_at(const char *name83, uint32_t dir_cluster, uint32_t size, h68k_src_fn src, void *ctx);
int  h68k_inject_stream(const char *name83, uint32_t size, h68k_src_fn src, void *ctx);  // = root

// Timestamp (DOS-FAT 16-bit time/date, same encoding as FatFs FILINFO
// ftime/fdate) applied to entries created by subsequent inject/mkdir calls.
// Defaults to 0/0; set once per operation from the HTTP layer.
void h68k_set_stamp(uint16_t dos_time, uint16_t dos_date);

// Rename a file or directory in place (entry name fields only; data, FAT,
// attributes, timestamp and cluster are untouched, so renaming a directory is
// safe).  0 = ok, -1 not found / IO error, -2 bad new name, -5 name exists.
int  h68k_rename(uint32_t dir_cluster, const char *name, const char *newname);

// Delete a file, or an EMPTY directory (mark the entry free + release its FAT
// chain).  A directory holding files/subdirs is refused (never recursive).
// 0 = deleted, -1 = not found / IO error, -6 = directory not empty.
int  h68k_delete(uint32_t dir_cluster, const char *name);

// Move a plain file's entry from src_dir to dst_dir (data stays in place).
// 0 = ok, -1 not found/IO, -2 refused (directory), -4 full, -5 name exists.
int  h68k_move(uint32_t src_dir, const char *name, uint32_t dst_dir);

// Create a subdirectory in dir_cluster (0 = root).
// 0 = ok, -2 bad name, -3 IO, -4 full, -5 already exists.
int  h68k_mkdir(uint32_t dir_cluster, const char *name);

// Convenience wrapper for an in-RAM payload (used by tests).
int  h68k_inject(const char *name83, const uint8_t *data, uint32_t size);
