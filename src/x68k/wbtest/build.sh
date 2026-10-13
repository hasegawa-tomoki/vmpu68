#!/bin/sh
# build.sh: WBTEST.X(ライトバックと PSRAM 割当範囲への DMA の整合試験)を作る。vasm は ~/.cache/vmpu68/tools/vasm に置く。
V=${VASM:-$HOME/.cache/vmpu68/tools/vasm/vasmm68k_mot}
cd "$(dirname "$0")" || exit 1
$V -Fbin -m68000 -quiet -o wbtest.bin wbtest.s || exit 1
python3 ../vmpu68x/mkx.py wbtest.bin WBTEST.X || exit 1
ls -la WBTEST.X
