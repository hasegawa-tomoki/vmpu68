#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Load a test 68k program into the bare-metal emulator over the CDC console
# and verify execution (PC parks at the final bra-self, counters written
# through to the mock bus RAM).
#   emutest.py /dev/cu.usbmodemXXXX
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vmpu68_serial import open_console, send_file, command

tty = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem112201"

# vectors: SP=0x2000, PC=0x400; code at 0x400:
#   moveq #0,d0; lea $1000.w,a0
#   loop: move.w d0,(a0)+; addq #1,d0; cmpi.w #$10,d0; bne loop; bra self
img = bytearray(0x400 + 18)
img[0:8] = bytes.fromhex("0000200000000400")
img[0x400:0x400 + 18] = bytes.fromhex("700041F8100030C052400C40001066F660FE")

fd = open_console(tty)
r = command(fd, "ver", [b"vmpu68 baremetal"])
print(r.decode(errors="replace").strip())

print("load:", send_file(fd, f"eld {len(img)} 0", bytes(img), progress=False))

print(command(fd, "ers", [b"emu reset"]).decode(errors="replace").strip())
r = command(fd, "ern 100000", [b"pc="], timeout=15).decode(errors="replace")
print(r.strip())
assert "pc=000410" in r, f"PC did not reach 0x410: {r!r}"
print(command(fd, "erg", [b"a0-3"]).decode(errors="replace").strip())
ok = True
for i in range(16):
    r = command(fd, f"rd 0x{0x1000 + i*2:X}", [b"]"], timeout=3).decode(errors="replace")
    if f"= {i:04X}" not in r:
        ok = False
        print(f"  MISMATCH at 0x{0x1000 + i*2:X}: {r.strip()}")
print("EMU BAREMETAL", "ALL PASS" if ok else "FAIL")
os.close(fd)
