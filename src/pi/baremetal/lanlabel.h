// SPDX-License-Identifier: MIT
// "Who is at which SCSI ID / FDD unit" for VMPU68.X -d: the Pi's own virtual
// disks plus what the vhd68 / vfd68 on the LAN report (/api/status), written
// into the virtual SCSI port's label table (emu68k_vscsi_set_label).
#pragma once
#include <circle/net/netsubsystem.h>
void lanlabel_init (CNetSubSystem *pNet);
void lanlabel_poll (void);          // main loop: serves a pending request (blocks core 0 for the HTTP round trips, ~0.5 s)
