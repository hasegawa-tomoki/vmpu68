#!/usr/bin/env python3
"""Create a Human68k-formatted 2HD floppy image (.xdf) holding given files.

(vfd68 プロジェクトの tools/mkxdf をそのまま流用。OEM 名だけ VMPU68 に変更。ブートスタブ bootstub.bin も同梱。)

The filesystem layout follows Human68k's FORMAT.X for the 2HD (1232 KB)
medium: 1024-byte sectors, FAT12 with a 0xFE marker, 192 root entries.
Structure cross-checked against the public XEiJ emulator source
(FDMedia/HumanMedia).  The image is a data disk; its boot sector carries a
valid BPB plus a small stub (bootstub.s) that — like the stock boot code
FORMAT.X writes — prints a message and ejects itself if someone boots from
it, then restarts the machine through the ROM reset vector so the boot
scan continues with the next device (e.g. SCSI).

usage: mkxdf.py OUT.XDF [FILE ...]
"""
import os
import struct
import sys

SEC = 1024
TOTAL = 1232                  # sectors: 77 cyl x 2 heads x 8
RESERVED = 1
NFAT = 2
FATSECS = 2
ROOTENT = 192
ROOT_SEC = RESERVED + NFAT * FATSECS            # 5
DATA_SEC = ROOT_SEC + ROOTENT * 32 // SEC       # 11
FAT0 = RESERVED * SEC
ROOT0 = ROOT_SEC * SEC
DATA0 = DATA_SEC * SEC
CLUSTERS = TOTAL - DATA_SEC                     # 1221 (FAT12)
FAT_TAIL = 0xFFF


def build(files):
    img = bytearray(DATA0) + bytearray(b"\xe5" * ((TOTAL - DATA_SEC) * SEC))

    # Boot sector: BPB + the message/eject/reboot stub (bootstub.s,
    # assembled copy in bootstub.bin — rebuild with the commands in its
    # header if you change the source).
    stub = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "bootstub.bin"), "rb").read()
    assert 0x40 + len(stub) <= 0x162, "boot stub collides with geometry block"
    img[0:2] = b"\x60\x3e"                      # bra.s past the BPB to 0x40
    img[2:10] = b"VMPU68  "
    img[0x40:0x40 + len(stub)] = stub
    img[0x0B:0x1D] = (
        struct.pack("<HBHBHH", SEC, 1, RESERVED, NFAT, ROOTENT, TOTAL)
        + bytes([0xFE, FATSECS])
        + struct.pack(">HH", 8, 2)      # sectors/track, tracks/cyl (BE)
        + b"\x00\x00"
    )
    # The block the stock IPL reads its geometry from; kept for fidelity.
    img[0x162:0x170] = struct.pack(
        ">IIHHH", SEC, 0x03 << 24 | (ROOT_SEC // 8) << 8 | (1 + ROOT_SEC % 8),
        8, SEC // 32 - 1, DATA_SEC - 2)

    # FAT markers.
    for i in range(NFAT):
        base = FAT0 + i * FATSECS * SEC
        img[base:base + 4] = bytes([0xFE, 0xFF, 0xFF, 0x00])

    def set_fat(cluster, value):
        i = FAT0 + 3 * (cluster >> 1)
        if cluster & 1 == 0:
            img[i] = value & 0xFF
            img[i + 1] = (img[i + 1] & 0xF0) | (value >> 8)
        else:
            img[i + 1] = ((value & 0xF) << 4) | (img[i + 1] & 0x0F)
            img[i + 2] = value >> 4

    next_cluster = 2
    next_entry = ROOT0
    for path in files:
        data = open(path, "rb").read()
        base = os.path.basename(path).upper()
        stem, _, ext = base.partition(".")
        if len(stem) > 18 or len(ext) > 3 or not stem.isascii():
            sys.exit(f"mkxdf: {base}: need an ASCII 18.3 name")
        clusters = max(1, (len(data) + SEC - 1) // SEC) if data else 0
        if next_cluster + clusters - 2 > CLUSTERS:
            sys.exit(f"mkxdf: {base}: disk full")

        # Directory entry.  Human68k stores names beyond 8 chars in the
        # 10-byte area at +12 that plain FAT keeps reserved (18.3 names);
        # unused extension bytes are zero.  Date/time, start cluster and
        # size are little-endian as on FAT.
        e = next_entry
        img[e:e + 11] = f"{stem[:8]:<8}{ext:<3}".encode("ascii")
        img[e + 11] = 0x20                      # archive
        img[e + 12:e + 22] = f"{stem[8:]:\0<10}".encode("ascii")
        img[e + 22:e + 24] = struct.pack("<H", 12 << 11)          # 12:00:00
        img[e + 24:e + 26] = struct.pack("<H", (2026 - 1980) << 9 | 1 << 5 | 1)
        start = next_cluster if data else 0
        img[e + 26:e + 28] = struct.pack("<H", start)
        img[e + 28:e + 32] = struct.pack("<I", len(data))
        next_entry += 32

        if data:
            for k in range(clusters - 1):
                set_fat(start + k, start + k + 1)
            set_fat(start + clusters - 1, FAT_TAIL)
            off = DATA0 + (start - 2) * SEC
            img[off:off + len(data)] = data
            next_cluster += clusters
        print(f"  {base:<12} {len(data):>7} bytes, cluster {start}")

    # Mirror FAT 0 into FAT 1.
    img[FAT0 + FATSECS * SEC:FAT0 + 2 * FATSECS * SEC] = \
        img[FAT0:FAT0 + FATSECS * SEC]
    assert len(img) == TOTAL * SEC == 1261568
    return bytes(img)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip())
    out = sys.argv[1]
    data = build(sys.argv[2:])
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {out} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
