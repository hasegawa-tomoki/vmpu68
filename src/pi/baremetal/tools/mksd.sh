#!/bin/sh
# mksd.sh <version> <kernel8-rpi4.img> <vmpu68-<ver>.vpk> <circle/boot dir> <out dir>
#   Raspberry Pi Imager で焼ける SD イメージ(MBR + FAT32 1 パーティション、64 MB)を作る。root 権限不要(mtools)。
#   中身: circle のブートファイル、config.txt、tryboot.txt(A/B の試験起動用)、kernel8-rpi4.img、vmpu68.vpk(初回起動で FPGA を自動書込み)、
#         vmpu68.cfg(sramboot=1)、wpa_supplicant.conf.example、Wi-Fi ファームウェア、README.txt。
#   出力: <out>/vmpu68-sd-<ver>.img.xz と、既存カードに上書きする人向けの <out>/vmpu68-sd-<ver>.zip。
set -e
VER=$1; KERNEL=$2; VPK=$3; BOOT=$4; OUT=$5
[ -n "$OUT" ] || { echo "usage: mksd.sh <version> <kernel8-rpi4.img> <pkg.vpk> <circle/boot> <outdir>"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
S=$WORK/sd; mkdir -p "$S" "$OUT"
for f in start4.elf fixup4.dat bcm2711-rpi-4-b.dtb; do cp "$BOOT/$f" "$S/"; done
cp "$BOOT/armstub/armstub8-rpi4.bin" "$S/" 2>/dev/null || cp "$BOOT/armstub8-rpi4.bin" "$S/"
cp "$KERNEL" "$S/kernel8-rpi4.img"
cp "$VPK" "$S/vmpu68.vpk"
cp "$HERE/../wlan-files/"brcmfmac* "$S/" 2>/dev/null || true
mkdir -p "$S/firmware"; cp "$HERE/../wlan-files/"brcmfmac* "$S/firmware/" 2>/dev/null || true
cp "$HERE/../wlan-files/wpa_supplicant.conf.example" "$S/"
cp "$HERE/../../../../release/config/vmpu68.cfg.sample" "$S/vmpu68.cfg.sample"
cat > "$S/config.txt" <<'CFG'
arm_64bit=1
kernel=kernel8-rpi4.img
kernel_address=0x80000
armstub=armstub8-rpi4.bin
max_framebuffers=2
disable_splash=1
CFG
# 試験起動(A/B)用: 次の 1 回だけ tryboot.txt で起動し kernel8-try.img を試す。無いと「お試し起動」が効かない
sed 's/^kernel=kernel8-rpi4.img$/kernel=kernel8-try.img/' "$S/config.txt" > "$S/tryboot.txt"
cat > "$S/vmpu68.cfg" <<'CFG'
# vmpu68 の設定(書式は vmpu68.cfg.sample 参照)。この行以下は Web UI などから書き換えられます。
name=X68000
mhz=0
wb=1
jit=0
sramboot=1
CFG
cat > "$S/README.txt" <<TXT
vmpu68 $VER SD card image
=========================
1. Raspberry Pi Imager: "Use custom" でこの .img.xz を選び、SD カードに書き込む。
2. (任意) SD の wpa_supplicant.conf.example を wpa_supplicant.conf にコピーして SSID とパスフレーズを書く。
   あとから基板のシリアルコンソール(Pi の USB-C、115200)で  wifi <ssid> <passphrase>  でも書ける。
3. 基板に挿して電源 ON。初回起動は SD の vmpu68.vpk から FPGA に自動で書き込む(約 1〜2 分、Pi 本体の ACT LED が速く点滅。基板の RGB LED は消灯)。
   終わると再起動して X68000 が動き出す。SRAM の起動画面プログラムも自動で入る(vmpu68.cfg の sramboot=1)。
4. 復旧したいときも同じ手順で焼き直せばよい(FPGA は毎回書き直される。Wi-Fi の設定は消えるので再度書く)。
   初回書込みに失敗すると SD に provision.failed ができて次回は再試行しない。消せばもう一度試す。
Web UI: http://<基板の IP>/  コンソール: wifi / upd / fpga / rm / st など
TXT
IMG=$WORK/sd.img; SIZE_MB=64
dd if=/dev/zero of="$IMG" bs=1M count=$SIZE_MB status=none
# MBR: 1 パーティション(FAT32 LBA、開始 1 MiB)
printf 'label: dos\nunit: sectors\nstart=2048, type=c, bootable\n' | sfdisk -q "$IMG"
OFF=$((2048*512)); PSZ=$(( SIZE_MB*1024*1024 - OFF ))
mformat -i "$IMG@@$OFF" -F -v VMPU68 -T $((PSZ/512)) ::
mcopy -i "$IMG@@$OFF" -s "$S"/* ::
mdir -i "$IMG@@$OFF" :: | tail -3
xz -T0 -9 -c "$IMG" > "$OUT/vmpu68-sd-$VER.img.xz"
(cd "$S" && zip -q -r "$OUT/vmpu68-sd-$VER.zip" .)
ls -l "$OUT/vmpu68-sd-$VER.img.xz" "$OUT/vmpu68-sd-$VER.zip"
