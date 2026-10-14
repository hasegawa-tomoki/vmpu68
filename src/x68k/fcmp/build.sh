#!/bin/sh
# build.sh: FCMP.X(2 つのファイルの比較)を作る。vasm は ~/.cache/vmpu68/tools/vasm に置く。
V=${VASM:-$HOME/.cache/vmpu68/tools/vasm/vasmm68k_mot}
cd "$(dirname "$0")" || exit 1
$V -Fbin -m68000 -quiet -o fcmp.bin fcmp.s || exit 1
python3 ../vmpu68x/mkx.py fcmp.bin FCMP.X || exit 1
ls -la FCMP.X
