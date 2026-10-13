// SPDX-License-Identifier: MIT
// Pull a file from a LAN peer (vhd68 / vfd68 / another vmpu68) onto the image
// volume: POST /api/fetch?ip=&port=&path=&dst=[&slot=] starts a background
// task that GETs http://ip:port<path> and writes it to dst; GET /api/fetch
// reports progress the way vhd68's does ({"state":"idle|run|done|error",...}).
// With slot=<0-6> the image is mounted on that virtual SCSI ID when done.
#pragma once
#include <circle/types.h>
#include <circle/string.h>
#include <circle/net/netsubsystem.h>

void netfetch_init (CNetSubSystem *pNet);
int  netfetch_start (const char *ip, unsigned port, const char *path, const char *dst, int slot,
                     const char *img, const char *name, unsigned dir, unsigned t);   // img/name: inject into that image folder when done (dst is the temp file); 0 ok, -1 busy, -2 bad args
void netfetch_status_json (CString &Out);
const char *netfetch_busy_path (void);              // dst of the running transfer, "" when idle (sdimg refuses to remove/rename it)
