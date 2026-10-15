// SPDX-License-Identifier: MIT
// Inside a disk image on the image volume: the Human68k file system of a .hds
// (vhd68's human68k.c) and the ISO9660 tree of a .iso (iso9660.c), for the
// Web UI's image browser and for a peer's remote browse.  Same API shape as
// vhd68 so its UI and ours can talk to either device.  Images that are
// mounted on a virtual SCSI ID are read-only here (the X68000 owns them).
#pragma once
#include <circle/types.h>
#include <circle/string.h>

int  imgfs_ls (const char *img, u32 clu, u32 sz, CString &Out);              // {"files":[{"n","s","d","c"}]} / {"ok":false,"error"}
// streamed download of one file: open (size), then read in pieces, then close.  One at a time.
int  imgfs_get_open (const char *img, u32 clu, u32 sz, const char *name, u32 *size, const char **err);
int  imgfs_get_read (u8 *buf, u32 len);                                       // bytes, 0 at the end, <0 error
void imgfs_get_close (void);
// writes (the image must not be mounted): 0 ok, else a message in *err
int  imgfs_inject (const char *img, const char *src, const char *name, u32 dir, int ow, u32 t, const char **err);   // src: a file on the image volume
int  imgfs_rm     (const char *img, const char *name, u32 dir, const char **err);
int  imgfs_mkdir  (const char *img, const char *name, u32 dir, u32 t, const char **err);
int  imgfs_rename (const char *img, const char *name, const char *to, u32 dir, const char **err);
int  imgfs_move   (const char *img, const char *name, u32 from, u32 to, const char **err);
