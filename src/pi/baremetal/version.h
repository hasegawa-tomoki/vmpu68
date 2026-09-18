// SPDX-License-Identifier: MIT
// Firmware version, reported by /api/status, the Web UI, the CLI banner and
// the UDP discovery beacon.  Bump on every release (release/vmpu68-<ver>.vpk).
#pragma once

#define VMPU68_VERSION  "1.0.0"      // package (kernel + bitstream released together)
#define VMPU68_PI_VERSION "1.0.0"    // the Pi kernel itself
#define VMPU68_BUILD    __DATE__ " " __TIME__
