// SPDX-License-Identifier: MIT
//
// vmpu68 release package (.vpk): one file carrying the Pi kernel image and
// the iCE40 bitstream, installable from the Web UI, the console (upd) or
// tools/httpmgmt.py.  Built by tools/mkvpk.py.
//
// Layout: 64-byte header, then the payloads at the offsets given in the
// header (byte offsets from the start of the file).  All fields are
// little-endian; sums are the console/HTTP transfer checksum
// (sum = sum * 31 + byte, 32-bit wrap) over the respective payload.
#ifndef _vpk_h
#define _vpk_h

#include <stdint.h>

#define VPK_MAGIC       "VPK1"
#define VPK_HDR_SIZE    64
#define VPK_LABEL_LEN   24

typedef struct
{
    char     magic[4];                 // "VPK1"
    uint32_t hdr_size;                 // VPK_HDR_SIZE
    char     label[VPK_LABEL_LEN];     // build label, NUL padded
    uint32_t kernel_off, kernel_len, kernel_sum;
    uint32_t fpga_off, fpga_len, fpga_sum;     // fpga_len 0: kernel-only package
    uint32_t flags;                    // reserved (0)
    uint32_t hdr_sum;                  // checksum over bytes 0..59
} vpk_hdr_t;

#endif
