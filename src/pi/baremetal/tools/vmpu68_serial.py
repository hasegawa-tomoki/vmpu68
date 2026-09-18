#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Shared console/transfer helpers for the vmpu68 bare-metal firmware.
#
# Transfer protocol (fw/put/eld): send the command, wait for "GO", then send
# at most CHUNK bytes and wait for a '.' ACK before the next chunk.  The
# firmware replies "ok <n> bytes sum=<hex> ..." with a sum=sum*31+byte
# checksum over the payload.
import os, termios, time

CHUNK = 2048

def checksum(data):
    s = 0
    for b in data:
        s = (s * 31 + b) & 0xFFFFFFFF
    return s

def open_console(tty):
    fd = os.open(tty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0
    a[4] = termios.B115200; a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    # clear any stale firmware line-buffer fragment and drain old output
    os.write(fd, b"\r")
    drain(fd, 1.0)
    return fd

def drain(fd, wait):
    buf = b""
    end = time.time() + wait
    while time.time() < end:
        try:
            buf += os.read(fd, 4096)
        except BlockingIOError:
            time.sleep(0.02)
    return buf

def read_until(fd, tokens, timeout):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        try:
            c = os.read(fd, 4096)
            if c:
                buf += c
                for t in tokens:
                    if t in buf:
                        return buf
        except BlockingIOError:
            pass
        time.sleep(0.01)
    raise SystemExit(f"timeout; got {buf!r}")

def command(fd, cmd, tokens, timeout=5.0):
    os.write(fd, cmd.encode() + b"\r")
    return read_until(fd, tokens, timeout)

def send_file(fd, cmd, data, progress=True):
    os.write(fd, cmd.encode() + b"\r")
    read_until(fd, [b"GO"], 10)
    sent = 0
    while sent < len(data):
        n = min(CHUNK, len(data) - sent)
        chunk = data[sent:sent + n]
        off = 0
        while off < len(chunk):
            try:
                off += os.write(fd, chunk[off:])
            except BlockingIOError:
                time.sleep(0.01)
        sent += n
        read_until(fd, [b"."], 15)          # per-chunk ACK
        if progress and sent % (64 * CHUNK) == 0:
            print(f"  {sent}/{len(data)}")
    out = read_until(fd, [b"ok ", b"FAILED", b"timeout"], 30)
    line = out.decode(errors="replace").strip().splitlines()[-1]
    want = f"sum={checksum(data):08X}"
    if line.startswith("ok ") and want not in line:
        raise SystemExit(f"CHECKSUM MISMATCH: expected {want}, got: {line}")
    return line
