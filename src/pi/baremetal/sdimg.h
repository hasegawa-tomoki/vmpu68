// SPDX-License-Identifier: MIT
// Image volume on the SD card: the boot partition (64 MB FAT32, "SD:") is far
// too small for Human68k disk images, so the rest of the card becomes a second
// partition ("SD2:", FAT32 with large clusters) made on request from the Web UI
// (/api/sd/expand).  Virtual SCSI image paths ("/x.hds") live on that volume
// when it exists, else under SD:/vscsi.
#pragma once
#include <circle/types.h>
#include <circle/string.h>

void sdimg_init (u64 card_bytes);                                   // after f_mount ("SD:"): mount the image volume if the card has it
boolean sdimg_present (void);
const char *sdimg_path (const char *p, char *buf, unsigned n);      // "/x.hds" -> "SD2:/x.hds" (or "SD:/x.hds" without the volume); "SD:"/"SD2:" paths pass through
void sdimg_info_json (CString &Out);                                // {"card_mb":..,"boot_mb":..,"img":bool,"img_total_mb":..,"img_free_mb":..,"spare_mb":..,"expandable":bool}
int  sdimg_expand (CString &Err);                                   // make + format + mount the image volume, move SD:/vscsi/* onto it: 0 ok
int  sdimg_list_json (const char *dir, CString &Out);               // {"ok":true,"dir":"/","files":[{"n":"x.hds","s":123,"d":0,"t":"2026-10-12 20:00"},...]}
int  sdimg_remove (const char *p);                                  // file or empty folder: 0 ok
int  sdimg_mkdir (const char *p);
int  sdimg_rename (const char *p, const char *to);
