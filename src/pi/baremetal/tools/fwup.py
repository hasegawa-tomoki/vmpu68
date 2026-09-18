#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Push a new kernel image to the running vmpu68 bare-metal firmware over
# its CDC/UART console, then reboot into it.
#   fwup.py /dev/cu.usbmodemXXXX kernel8-rpi4.img [--try]
# With --try the image is written to SD:/kernel8-try.img and the firmware
# is restarted in tryboot mode (one-shot; falls back to the stable kernel
# on the next reset or watchdog timeout).
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vmpu68_serial import open_console, send_file, drain

tty, img = sys.argv[1], sys.argv[2]
tryboot = "--try" in sys.argv[3:]
data = open(img, "rb").read()

fd = open_console(tty)
if tryboot:
    print(f"sending {len(data)} bytes -> SD:/kernel8-try.img ...")
    print(send_file(fd, f"put {len(data)} SD:/kernel8-try.img", data))
    os.write(fd, b"tbr\r")
    print("tryboot rebooting...")
else:
    print(f"sending {len(data)} bytes...")
    print(send_file(fd, f"fw {len(data)}", data))
    os.write(fd, b"reboot\r")
    print("rebooting...")
drain(fd, 0.5)
os.close(fd)
