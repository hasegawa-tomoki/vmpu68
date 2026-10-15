#!/bin/sh
# build.sh: VSCSITST.X を作る。vasm は ~/.cache/vmpu68/tools/vasm に置く(x68k/vmpu68x/build.sh と同じ)。
V=${VASM:-$HOME/.cache/vmpu68/tools/vasm/vasmm68k_mot}
cd "$(dirname "$0")" || exit 1
$V -Fbin -m68000 -quiet -o vscsitst.bin vscsitst.s || exit 1
python3 ../vmpu68x/mkx.py vscsitst.bin VSCSITST.X || exit 1
ls -la VSCSITST.X
