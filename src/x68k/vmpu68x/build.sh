#!/bin/sh
# build.sh: VMPU68.X と SRAM 起動プログラム(sramboot_bin.h)を作る(VMPU68.SYS/.R は 2026-09-16 に廃止)。vasm は ~/.cache/vmpu68/tools/vasm に置く。
V=${VASM:-$HOME/.cache/vmpu68/tools/vasm/vasmm68k_mot}
cd "$(dirname "$0")" || exit 1
$V -Fbin -m68000 -quiet -DXMODE=1 -o vmpu68x.bin vmpu68.s || exit 1
python3 mkx.py vmpu68x.bin VMPU68.X || exit 1
$V -Fbin -m68000 -quiet -DSRAMMODE=1 -o vmpu68sram.bin vmpu68.s || exit 1
python3 - <<'PY'
# SRAM 起動版をカーネルに埋め込むヘッダに(pi/baremetal/sramboot_bin.h)
b=open('vmpu68sram.bin','rb').read()
assert b[0]==0x60 and b[4:12]==b'VMPU68SB'
out=["// 生成物: x68k/vmpu68x/build.sh が vmpu68.s(-DSRAMMODE)から作る。編集しないこと。",
     "// SRAM 起動プログラム(IPL が $ED0100 を jsr で呼ぶ)。先頭 $60 = BRA、+4 に署名 'VMPU68SB'、+12 に版数。",
     "#pragma once","#include <stdint.h>",f"#define SRAMBOOT_LEN {len(b)}u","static const uint8_t sramboot_bin[SRAMBOOT_LEN] = {"]
for i in range(0,len(b),16): out.append("    "+", ".join(f"0x{x:02X}" for x in b[i:i+16])+",")
out.append("};")
open('../../pi/baremetal/sramboot_bin.h','w').write("\n".join(out)+"\n")
PY
ls -la VMPU68.X vmpu68sram.bin
