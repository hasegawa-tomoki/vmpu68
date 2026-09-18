#!/bin/sh
# release.sh <version> [--tag] [--github]
#   リリース一式を作る(Linux 機で実行。aarch64 ツールチェーン、circle、mtools、vasm は前提)。
#     1. version.h の VMPU68_VERSION を <version> に(既に同じなら触らない)
#     2. カーネルをビルド(WITH_EMU=1 WITH_WATCHDOG=1)
#     3. release/vmpu68-<ver>.vpk(カーネル + src/fpga/rtl/VERSION の FPGA ビットストリーム)
#     4. release/vmpu68-sd-<ver>.img.xz / .zip(tools/mksd.sh)
#     5. release/Human68k/VMPU68.XDF(VMPU68.X 入り 2HD イメージ、tools/mkxdf)と release/Human68k/VMPU68.X
#     6. --tag: git tag v<ver>(注釈付き)   --github: gh release create v<ver> に上記を添付(git push --tags 込み)
#   前提: リポジトリ直下(vmpu68/)が git 管理下で、gh auth login 済み。
set -e
VER=$1; [ -n "$VER" ] || { echo "usage: release.sh <version> [--tag] [--github]"; exit 1; }
shift
HERE=$(cd "$(dirname "$0")" && pwd)           # src/pi/baremetal/tools
BM=$HERE/..; SW=$BM/..; SRC=$SW/..; ROOT=$SRC/..
REL=$ROOT/release; mkdir -p "$REL" "$REL/Human68k" "$REL/config"
FPGA_VER=$(cat "$SRC/fpga/rtl/VERSION")
FPGA_BIN=$SRC/fpga/fpga/vmpu68_top.bin
BOOT=$SRC/third_party/circle/boot
X68=$SRC/x68k/vmpu68x

echo "== version"
if ! grep -q "\"$VER\"" "$BM/version.h"; then
  sed -i.bak "s/#define VMPU68_VERSION .*/#define VMPU68_VERSION \"$VER\"/" "$BM/version.h" && rm -f "$BM/version.h.bak"
  echo "version.h -> $VER"
fi
echo "== kernel"
(cd "$BM" && make WITH_EMU=1 WITH_WATCHDOG=1 >/dev/null)
echo "== VMPU68.X / SRAM boot program"
if [ -x "${VASM:-$HOME/.cache/vmpu68/tools/vasm/vasmm68k_mot}" ]; then
  (cd "$X68" && sh build.sh >/dev/null) && cp "$X68/VMPU68.X" "$REL/Human68k/VMPU68.X"
else
  echo "   (vasm が無いので x68k/vmpu68x/VMPU68.X と sramboot_bin.h はコミット済みのものを使う)"
  cp "$X68/VMPU68.X" "$REL/Human68k/VMPU68.X"
fi
echo "== package"
python3 "$HERE/mkvpk.py" build "$REL/vmpu68-$VER.vpk" "$BM/kernel8-rpi4.img" "$FPGA_BIN" -l "$VER" -f "$FPGA_VER"
echo "== SD image"
sh "$HERE/mksd.sh" "$VER" "$BM/kernel8-rpi4.img" "$REL/vmpu68-$VER.vpk" "$BOOT" "$REL"
echo "== FD image"
python3 "$HERE/mkxdf/mkxdf.py" "$REL/Human68k/VMPU68.XDF" "$REL/Human68k/VMPU68.X" >/dev/null
cp "$BM/wlan-files/wpa_supplicant.conf.example" "$REL/config/wpa_supplicant.conf"
ls -l "$REL/vmpu68-$VER.vpk" "$REL/vmpu68-sd-$VER.img.xz" "$REL/vmpu68-sd-$VER.zip" "$REL/Human68k/VMPU68.XDF" "$REL/Human68k/VMPU68.X"

for opt in "$@"; do
  case "$opt" in
  --tag)
    (cd "$ROOT" && git add -A release src/pi/baremetal/version.h && git commit -q -m "release $VER" || true
     git tag -a "v$VER" -m "vmpu68 $VER") && echo "tagged v$VER";;
  --github)
    (cd "$ROOT" && git push && git push --tags
     if [ -f "$REL/notes-$VER.md" ]; then NOTES="--notes-file $REL/notes-$VER.md"; else NOTES="--notes vmpu68_$VER"; fi
     gh release create "v$VER" --title "vmpu68 $VER" $NOTES \
        "$REL/vmpu68-$VER.vpk" "$REL/vmpu68-sd-$VER.img.xz" "$REL/vmpu68-sd-$VER.zip" "$REL/Human68k/VMPU68.XDF" "$REL/Human68k/VMPU68.X") && echo "GitHub release v$VER created";;
  esac
done
