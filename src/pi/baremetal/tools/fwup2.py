#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Firmware push tolerant of the one-transaction-delayed CDC issue: does not
# rely on the GO handshake, pads the stream to flush the last packet, and
# pumps extra newlines so delayed responses/commands come through.
#   fwup2.py /dev/cu.usbmodemXXXX kernel8-rpi4.img
import os, sys, termios, time

tty, img = sys.argv[1], sys.argv[2]
data = open(img, "rb").read()

fd = os.open(tty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
a[4] = termios.B115200; a[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, a)

def drain(w):
    buf = b""
    end = time.time() + w
    while time.time() < end:
        try:
            buf += os.read(fd, 4096)
        except BlockingIOError:
            time.sleep(0.02)
    return buf

# clear stale input line, pump any delayed output
os.write(fd, b"\r"); drain(0.5)
os.write(fd, b"\r"); drain(0.5)

# send the fw command and the image back-to-back: with the one-transaction
# input delay the command line is only processed once the first data packet
# arrives, so no drain in between (a stray byte would shift the image)
print(f"sending fw header + {len(data)} bytes...")
payload = f"fw {len(data)}\r".encode() + data
sent = 0
t0 = time.time()
while sent < len(payload):
    try:
        n = os.write(fd, payload[sent:sent + 2048])
        sent += n
    except BlockingIOError:
        if time.time() - t0 > 120:
            raise SystemExit(f"stalled at {sent}/{len(payload)}")
        time.sleep(0.05)
        continue
    time.sleep(0.002)
# pad so the delayed last packet gets pushed through; the pad byte lands in
# the console line buffer after FileReceive stops at the exact length
os.write(fd, b"\r")
r = drain(3.0)
print("after data:", r.decode(errors="replace").strip() or "(no reply)")
if b"ok " not in r:
    r += drain(5.0)
    print("late:", r.decode(errors="replace").strip())

for i in range(3):
    os.write(fd, b"reboot\r")
    time.sleep(0.3)
os.close(fd)
print("reboot pumped; watch for device cycle")
