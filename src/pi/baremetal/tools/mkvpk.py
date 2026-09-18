#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Build / inspect a vmpu68 release package (.vpk): Pi kernel + FPGA bitstream
# in one file (format: pi/baremetal/vpk.h).
#   mkvpk.py build <out.vpk> <kernel8-rpi4.img> [<vmpu68_top.bin>] [-l <label>] [-f <fpga version>]
#   (FPGA 版数は既定で src/fpga/rtl/VERSION から読み、ヘッダの flags に major<<16|minor<<8|patch で入る)
#   mkvpk.py info  <file.vpk>
#   mkvpk.py extract <file.vpk> [<outdir>]     -> kernel8-rpi4.img, vmpu68_top.bin
# The label defaults to the kernel's build timestamp (read from the image).
import re, struct, sys

MAGIC, HDR, LABEL = b"VPK1", 64, 24
FMT = "<4sI24s3I3III"        # magic hdr_size label kernel(off,len,sum) fpga(off,len,sum) flags hdr_sum


def checksum(data):
    s = 0
    for b in data:
        s = (s * 31 + b) & 0xFFFFFFFF
    return s


def kernel_label(img):
    # circle's __DATE__ " " __TIME__ string, e.g. "Sep  6 2026 20:09:33"
    m = re.search(rb"[A-Z][a-z]{2} [ \d]\d \d{4} \d\d:\d\d:\d\d", img)
    if not m:
        return "vmpu68"
    mon, day, year, hms = m.group(0).decode().split()
    months = "JanFebMarAprMayJunJulAugSepOctNovDec"
    return f"{year}{months.index(mon) // 3 + 1:02d}{int(day):02d}-{hms.replace(':', '')[:4]}"


def parse(blob):
    if len(blob) < HDR:
        raise SystemExit("too short")
    f = struct.unpack(FMT, blob[:HDR])
    magic, hsz, label, koff, klen, ksum, foff, flen, fsum, flags, hsum = f
    if magic != MAGIC or hsz != HDR:
        raise SystemExit("not a vpk file")
    if hsum != checksum(blob[:HDR - 4]):
        raise SystemExit("header checksum mismatch")
    return dict(label=label.rstrip(b"\0").decode(), koff=koff, klen=klen, ksum=ksum,
                foff=foff, flen=flen, fsum=fsum, flags=flags)


def fpga_version_flags(fpga_path, override=None):
    """rtl/VERSION("0.1.17")を major<<16 | minor<<8 | patch に。無ければ 0(不明)。"""
    import os
    ver = override
    if ver is None:
        for cand in (os.path.join(os.path.dirname(os.path.abspath(fpga_path or "")), "..", "rtl", "VERSION"),
                     os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "fpga", "rtl", "VERSION")):
            try:
                ver = open(cand).read().strip(); break
            except OSError: pass
    if not ver: return 0
    m = re.match(r"(\d+)\.(\d+)\.(\d+)", ver)
    return (int(m.group(1)) << 16 | int(m.group(2)) << 8 | int(m.group(3))) if m else 0

def build(out, kernel_path, fpga_path, label, fpga_ver=None):
    kernel = open(kernel_path, "rb").read()
    fpga = open(fpga_path, "rb").read() if fpga_path else b""
    if not label:
        label = kernel_label(kernel)
    lab = label.encode()[:LABEL - 1].ljust(LABEL, b"\0")
    koff = HDR
    foff = koff + ((len(kernel) + 511) & ~511) if fpga else 0
    flags = fpga_version_flags(fpga_path, fpga_ver) if fpga else 0   # FPGA 版数(major<<16|minor<<8|patch、0 = 不明)
    hdr = struct.pack(FMT, MAGIC, HDR, lab, koff, len(kernel), checksum(kernel),
                      foff, len(fpga), checksum(fpga) if fpga else 0, flags, 0)
    hdr = hdr[:HDR - 4] + struct.pack("<I", checksum(hdr[:HDR - 4]))
    body = hdr + kernel
    if fpga:
        body += b"\0" * (foff - len(body)) + fpga
    open(out, "wb").write(body)
    print(f"{out}: label {label}, kernel {len(kernel)} bytes, "
          f"fpga {len(fpga) or 'none'} bytes, total {len(body)}")


def info(path):
    blob = open(path, "rb").read()
    h = parse(blob)
    k = blob[h["koff"]:h["koff"] + h["klen"]]
    f = blob[h["foff"]:h["foff"] + h["flen"]] if h["flen"] else b""
    print(f"label  : {h['label']}")
    fl = h.get("flags", 0)
    if fl: print(f"fpga ver: {fl >> 16}.{(fl >> 8) & 255}.{fl & 255}")
    print(f"kernel : {h['klen']} bytes sum {h['ksum']:08X} "
          f"{'ok' if checksum(k) == h['ksum'] and len(k) == h['klen'] else 'BAD'}")
    if h["flen"]:
        print(f"fpga   : {h['flen']} bytes sum {h['fsum']:08X} "
              f"{'ok' if checksum(f) == h['fsum'] and len(f) == h['flen'] else 'BAD'}")
    else:
        print("fpga   : none")


def extract(path, outdir):
    import os
    blob = open(path, "rb").read()
    h = parse(blob)
    k = blob[h["koff"]:h["koff"] + h["klen"]]
    if checksum(k) != h["ksum"]:
        raise SystemExit("kernel checksum mismatch")
    open(os.path.join(outdir, "kernel8-rpi4.img"), "wb").write(k)
    print(f"kernel8-rpi4.img: {len(k)} bytes")
    if h["flen"]:
        f = blob[h["foff"]:h["foff"] + h["flen"]]
        if checksum(f) != h["fsum"]:
            raise SystemExit("fpga checksum mismatch")
        open(os.path.join(outdir, "vmpu68_top.bin"), "wb").write(f)
        print(f"vmpu68_top.bin: {len(f)} bytes")


def main():
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__ or "usage: mkvpk.py build|info ...")
    if a[0] == "info":
        info(a[1])
    elif a[0] == "extract":
        extract(a[1], a[2] if len(a) > 2 else ".")
    elif a[0] == "build":
        label = None
        if "-l" in a:
            i = a.index("-l"); label = a[i + 1]; del a[i:i + 2]
        fver = None
        if "-f" in a:
            i = a.index("-f"); fver = a[i + 1]; del a[i:i + 2]
        build(a[1], a[2], a[3] if len(a) > 3 else None, label, fver)
    else:
        raise SystemExit("usage: mkvpk.py build <out.vpk> <kernel.img> [<fpga.bin>] [-l label] | info <file.vpk> | extract <file.vpk> [outdir]")


if __name__ == "__main__":
    main()
