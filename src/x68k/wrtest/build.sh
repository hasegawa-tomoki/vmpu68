#!/bin/sh
# build.sh: WRTEST.X(実 RAM 書込みの化け数を X68000 側から数える検査)を作る。vasm は ~/.cache/vmpu68/tools/vasm に置く。
V=${VASM:-$HOME/.cache/vmpu68/tools/vasm/vasmm68k_mot}
cd "$(dirname "$0")" || exit 1
$V -Fbin -m68000 -quiet -o wrtest.bin wrtest.s || exit 1
python3 ../vmpu68x/mkx.py wrtest.bin WRTEST.X || exit 1
ls -la WRTEST.X
