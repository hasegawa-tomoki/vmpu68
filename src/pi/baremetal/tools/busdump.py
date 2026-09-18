#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Read a range of the 68000 bus through the bare-metal console and hexdump it.
#   busdump.py /dev/cu.usbserialXXXX <addr> [words] [--twice] [--write <hex16>]
# --twice reads the range two times and reports mismatching words (bus
# stability check); --write writes the given word to <addr> first and reads
# it back (RAM check).
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vmpu68_serial import open_console, drain

tty = sys.argv[1]
addr = int(sys.argv[2], 16)
words = int(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else 64
twice = "--twice" in sys.argv
wval = None
if "--write" in sys.argv:
    wval = int(sys.argv[sys.argv.index("--write") + 1], 16)

fd = open_console(tty)

def cmd(line, wait=0.15):
    os.write(fd, line.encode() + b"\r")
    out = b""
    end = time.time() + 3
    while time.time() < end:
        out += drain(fd, wait)
        if b"\n" in out:
            break
    return out.decode(errors="replace").strip()

def read_range(base, n):
    vals, faults = [], 0
    for i in range(n):
        r = cmd(f"rd 0x{base + 2*i:06X}")
        # "[FF0000] = 1234" or "... FAULT"
        try:
            v = int(r.split("=")[1].split()[0], 16)
        except Exception:
            v = None
        if "FAULT" in r:
            faults += 1
        vals.append(v)
    return vals, faults

if wval is not None:
    print(cmd(f"wr 0x{addr:06X} 0x{wval:04X}"))
    print(cmd(f"rd 0x{addr:06X}"), "(expect %04X)" % wval)

vals, faults = read_range(addr, words)
for i in range(0, words, 8):
    row = vals[i:i+8]
    hexs = " ".join("----" if v is None else f"{v:04X}" for v in row)
    asc = "".join(chr(b) if 32 <= b < 127 else "." for v in row if v is not None
                  for b in (v >> 8, v & 0xFF))
    print(f"{addr + 2*i:06X}: {hexs:40s} {asc}")
print(f"faults={faults}/{words}")

if twice:
    vals2, faults2 = read_range(addr, words)
    diff = sum(1 for a, b in zip(vals, vals2) if a != b)
    print(f"second pass: faults={faults2}/{words}, mismatching words={diff}")
    print(cmd("st"))
os.close(fd)
