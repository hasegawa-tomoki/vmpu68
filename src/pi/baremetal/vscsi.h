// SPDX-License-Identifier: MIT
// Virtual SCSI targets (docs/design/vscsi-plan.md).  The X68000 reaches them
// through the $ECD100 command port (emu68k.c) and the SRAM-resident
// _SCSIDRV hook (x68k/vmpu68x/vmpu68.s).  Command semantics are a port of
// vhd68's scsi.c: SCSI-1 direct-access targets (hds, 256/512-byte blocks)
// and a CD-ROM (iso, 2048).  Images live on the SD card (SD:/vscsi/...).
// Core use: the worker on core 3 executes commands; file I/O is handed to a
// task on core 0 (circle's FatFs and the EMMC driver belong to core 0).
#pragma once
#include <circle/types.h>
#include <circle/string.h>

#define VSCSI_IDS 7

struct vscsi_slot_info
{
    boolean  mounted;
    boolean  cdrom;
    boolean  wp;
    unsigned block_size;
    unsigned blocks;
    char     path[128];              // "/vscsi/foo.hds" (SD root relative)
};

void vscsi_init (void);             // core 0, before the worker core starts
void vscsi_start_io_task (void);    // core 0, after the SD card is mounted (creates the file I/O task)
void vscsi_worker (void);           // core 3 entry: never returns
void vscsi_stats (CString &Out);    // console 'vs'
void vscsi_log (unsigned n, CString &Out);   // console 'vsl [n]': last n commands (0 clears)

// mount/eject run on core 0 (console, Web API, boot from vmpu68.cfg)
int  vscsi_mount (unsigned id, const char *path, boolean wp);   // 0 ok, <0 error (-1 bad id, -2 open failed, -3 too small)
int  vscsi_eject (unsigned id);                                 // 0 ok (the ID stays ours as NOT READY until the next X68000 reset)
void vscsi_on_reset (void);                                     // X68000 reset: release ejected IDs to the real bus
const vscsi_slot_info *vscsi_slot (unsigned id);
int  vscsi_new_image (const char *path, unsigned mb);           // blank image (zeroed head, preallocated): 0 ok
void vscsi_status_json (CString &Out);
void vscsi_bench (unsigned id, unsigned mb, CString &Out);   // console vsb                          // "ids":[...] like vhd68
