#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Upload a file to the vmpu68 bare-metal SD via the console.
#   putfile.py /dev/cu.usbmodemXXXX local-file SD:/remote/path
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vmpu68_serial import open_console, send_file

tty, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, "rb").read()

fd = open_console(tty)
print(send_file(fd, f"put {len(data)} {dst}", data))
os.close(fd)
