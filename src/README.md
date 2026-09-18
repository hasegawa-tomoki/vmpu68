# ビルド手順

VMPU68 のソースは 3 つに分かれています。

| ディレクトリ | 内容                                                      | 成果物 |
|---|---------------------------------------------------------|---|
| `fpga/rtl/` `fpga/sim/` `fpga/fpga/` | iCE40HX4K の FPGA<br/>バス制御と Raspberry Pi インタフェース      | `fpga/fpga/vmpu68_top.bin` |
| `pi/baremetal/` `pi/core/` `pi/pi/` `pi/mgmtd/` | Raspberry Pi 4 のベアメタルカーネル<br/>68000 エミュレータと JIT | `pi/baremetal/kernel8-rpi4.img` |
| `x68k/vmpu68x/` | X68000 側のコマンド VMPU68.X と SRAM 起動プログラム                   | `VMPU68.X`、`pi/baremetal/sramboot_bin.h` |

配布物(`release/`)はこれらをまとめたものです: `vmpu68-<ver>.vpk`(カーネル + FPGA)、`vmpu68-sd-<ver>.img.xz`(SD イメージ)、`Human68k/VMPU68.X` と `VMPU68.XDF`。

## 必要なもの

Linux(x86_64 で確認)で作業します。

- FPGA: `yosys`、`nextpnr-ice40`、`icepack`(icestorm)、シミュレーションに `iverilog`
- カーネル: Arm GNU Toolchain (aarch64-none-elf、`/opt/arm-gnu-toolchain/` に展開。`pi/baremetal/Makefile` がこのパスの `libm.a` を参照します)と circle
  - circle は `git clone https://github.com/rsta2/circle src/third_party/circle` で取得し、`Config.mk` を次の内容にして `./makeall` を実行、さらに `addon/wlan` と `addon/wlan/hostap/wpa_supplicant` で `make` します
  - circle は 2026 年 5 月時点の master で確認しています
    ```
    AARCH = 64
    RASPPI = 4
    PREFIX64 = /opt/arm-gnu-toolchain/bin/aarch64-none-elf-
    DEFINE += -DKERNEL_MAX_SIZE=0x400000
    DEFINE += -DARM_ALLOW_MULTI_CORE
    ```
  - Wi-Fi ファームウェア (`brcmfmac43455-sdio.*`) は circle の `addon/wlan/firmware` から `pi/baremetal/wlan-files/` にコピー済みです
- Musashi(68000 エミュレータの核): `git clone https://github.com/kstenerud/Musashi src/third_party/Musashi` で取得し(commit 313ebf1 で確認)、`cd src/third_party/Musashi && patch -p1 < ../patches/musashi-local.patch` で vmpu68 用の設定を当てます
- circle にも `cd src/third_party/circle && git apply ../patches/circle-local.patch` でパッチを当てます(CDC ガジェットの受信の修正と HTTP のフォームデータ上限。詳細は `src/third_party/patches/README.md`)
- VMPU68.X: vasm (`vasmm68k_mot`。既定の置き場所は `~/.cache/vmpu68/tools/vasm/`、環境変数 `VASM` で変更可)
- FD イメージ: `m68k-xelf-as` / `m68k-xelf-objcopy`(ブートスタブを作り直すときだけ)
- SD イメージ: `mtools`、`sfdisk`、`xz`、`zip`
- その他: `python3`

## 1. FPGA

```sh
cd src/fpga/fpga
make sim     # シミュレーション(ALL TESTS PASSED を確認)
make bit     # vmpu68_top.bin を生成
```

RTL を変更したときは `src/fpga/rtl/VERSION` を上げてください。基板への書込みはカーネルが行うので、ここでは書きません。

## 2. カーネル

```sh
cd src/pi/baremetal
make WITH_EMU=1 WITH_WATCHDOG=1
```

`kernel8-rpi4.img` ができます。版数は `version.h` です。

## 3. VMPU68.X と SRAM 起動プログラム

```sh
cd src/x68k/vmpu68x
sh build.sh
```

`VMPU68.X` と、カーネルに埋め込む `pi/baremetal/sramboot_bin.h` を生成します。`sramboot_bin.h` を更新したらカーネルを作り直してください。

## 4. 配布物

```sh
cd src/pi/baremetal/tools
python3 mkvpk.py build ../../../../release/vmpu68-<ver>.vpk ../kernel8-rpi4.img ../../../fpga/fpga/vmpu68_top.bin -l <ver> -f <FPGA の版数>
sh mksd.sh <ver> ../kernel8-rpi4.img ../../../../release/vmpu68-<ver>.vpk ../sdcard ../../../../release
python3 mkxdf/mkxdf.py ../../../../release/Human68k/VMPU68.XDF ../../../../release/Human68k/VMPU68.X
```

一括で行うには `sh release.sh <ver>` を使います(`--tag` で git のタグ、`--github` で GitHub Release まで)。

## 5. 基板への書込み

- 初回: `vmpu68-sd-<ver>.img.xz` を Raspberry Pi Imager で SD に書き、基板に挿して電源を入れます。初回起動時にFPGAファームウェアが書き込まれます。
- 更新: Web UI の「ファームウェア」から `.vpk` を選んで「適用+再起動」。コマンドラインなら `python3 httpmgmt.py <IP アドレス> update ../../../../release/vmpu68-<ver>.vpk`。
- Human68k 側: `release/Human68k/VMPU68.X` を X68000 にコピーします(`VMPU68.XDF` は同じものを入れた 2HD イメージ)。
