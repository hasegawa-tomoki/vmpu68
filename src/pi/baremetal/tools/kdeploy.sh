#!/bin/sh
# kdeploy.sh [ラベル] [fpga]: Linux 機(VMPU68_BUILD_HOST)でカーネルをビルドして基板へ入れる(試験用)
#   ラベルは vpk の版名(既定 test)。第 2 引数を付けると $WORK/fpga-new.bin も同梱する。
#   ビットストリームは rsync 対象外。作業ファイルは $VMPU68_WORK(既定 ~/.cache/vmpu68)。
WORK=${VMPU68_WORK:-$HOME/.cache/vmpu68}; mkdir -p "$WORK"
TOOLS=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$TOOLS/../../../../.." && pwd)     # リポジトリのルート(vmpu68/ の親)
BUILD=${VMPU68_BUILD_HOST:?"VMPU68_BUILD_HOST=user@host(ビルドする Linux 機)を設定してください"}; BOARD=${VMPU68_BOARD:-192.168.0.20}
cd "$ROOT" || exit 1
rsync -rlpgoD --no-t --exclude '.git' --exclude 'third_party/circle' --exclude 'fpga/vmpu68_top.bin' vmpu68/src "$BUILD":~/dev/vmpu68/ || exit 1
ssh "$BUILD" 'cd ~/dev/vmpu68/src/pi/baremetal && rm -f kernel8-rpi4.img && make WITH_EMU=1 WITH_WATCHDOG=1 2>&1 | grep -i "error" ; ls kernel8-rpi4.img' || exit 1
scp -q "$BUILD":~/dev/vmpu68/src/pi/baremetal/kernel8-rpi4.img "$WORK/kernel-new.img" || exit 1
python3 "$TOOLS/mkvpk.py" build "$WORK/test.vpk" "$WORK/kernel-new.img" ${2:+"$WORK/fpga-new.bin"} -l "${1:-test}" | tail -1
python3 -u "$TOOLS/httpmgmt.py" "$BOARD" update "$WORK/test.vpk" 2>&1 | tail -1
