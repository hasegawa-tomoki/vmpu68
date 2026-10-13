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
# ビルドが失敗(コンパイルエラー、カーネルサイズ検査の不合格)したら生成物を消して止める: サイズ超過のカーネルを試験起動させると
# Raspberry Pi が起動途中で無言で止まり、電源の抜き差しが要る(2026-10-13 に発生)
ssh "$BUILD" 'cd ~/dev/vmpu68/src/pi/baremetal && rm -f kernel8-rpi4.img && { make WITH_EMU=1 WITH_WATCHDOG=1 > build.log 2>&1 || rm -f kernel8-rpi4.img; }; grep -i "error\|SIZE" build.log; ls kernel8-rpi4.img' || exit 1
scp -q "$BUILD":~/dev/vmpu68/src/pi/baremetal/kernel8-rpi4.img "$WORK/kernel-new.img" || exit 1
python3 "$TOOLS/mkvpk.py" build "$WORK/test.vpk" "$WORK/kernel-new.img" ${2:+"$WORK/fpga-new.bin"} -l "${1:-test}" | tail -1
# 試験起動(A/B)で入れる: 起動しないカーネルでも 20 秒健全でなければ前の版に自動で戻る。本番側を直接書くと
# 起動不能のとき SD を取り出して書き直すしかない(2026-10-13 に発生)
python3 -u "$TOOLS/httpmgmt.py" "$BOARD" update "$WORK/test.vpk" try 2>&1 | tail -1 || exit 1
# 再起動後に、基板に入ったカーネルのチェックサムが送ったものと一致するか(ab の表示)を確かめる
WANT=$(python3 "$TOOLS/httpmgmt.py" "$BOARD" sum "$WORK/kernel-new.img")
for i in $(seq 1 12); do GOT=$(curl -s -m 5 "http://$BOARD/api/con?cmd=ab" | grep -o "kernel8-rpi4.img: [0-9]* bytes sum=[0-9A-F]*" | grep -o "sum=.*" | cut -c5-); [ "$GOT" = "$WANT" ] && break; sleep 5; done   # 試験起動は 20 秒健全で本番側へ昇格するので、昇格まで待つ
if [ "$GOT" = "$WANT" ]; then echo "基板のカーネル = 今回のビルド (チェックサム $WANT)"; else echo "注意: 基板のカーネルのチェックサム $GOT が今回のビルド $WANT と一致しません"; exit 1; fi
