# third_party/circle へのローカル変更

circle は本リポジトリには含めません。VMPU68ビルドには以下のパッチが必要です。

## 1. Config.mk(リポジトリ外ファイル、新規作成)

```make
AARCH = 64
RASPPI = 4
PREFIX64 = /opt/arm-gnu-toolchain/bin/aarch64-none-elf-
DEFINE += -DKERNEL_MAX_SIZE=0x400000
```

- `KERNEL_MAX_SIZE=0x400000`(4MB)は必須。Musashi の BSS
  (512KB 命令ジャンプテーブル等)込みで既定 2MB を超えると
  EL1 スタックが BSS に重なり無言でブート死する。
  変更後は circle の全ライブラリを再ビルド(`./makeall clean && ./makeall`
  + addon の SDCard/fatfs/wlan)。

## 2. circle-local.patch — CDC ガジェットの受信飢餓バグ修正

`lib/usb/gadget/usbcdcgadgetendpoint.cpp`:

- 症状: RX リングバッファが一度満杯になると overrun で sticky な
  `m_nStatus=-1` がセットされ、`Read()` がデータより先にエラーを
  返してデータを取り出さない → リングが空かない → 以後の受信も
  全て overrun → 永久飢餓。`Write()` も同じステータスを見るため
  送信まで死ぬ。ホストから見ると「応答が1つずつ遅れる」
  「コマンドが無視される」「大量転送が化ける」。
- 修正: `Read()` はキューのデータを優先して返し、エラー報告は
  キューが空になってから。`Write()` から RX ステータス処理を除去。
- 適用: `cd src/third_party/circle && git apply ../patches/circle-local.patch`
- upstream (rsta2/circle) への報告候補。

## 3. addon/wlan/hostap のビルド

`hostap` は circle 内のサブモジュール(`git submodule update --init`)。
wpa_supplicant は同梱の .config そのままで `make`
(`cp defconfig .config` は dbus 有効になり失敗するので不可)。

# third_party/Musashi へのローカル変更(musashi-local.patch)

- 上流: https://github.com/kstenerud/Musashi(commit 313ebf1、2026-03-08 で確認)。
- 変更は 2 ファイルだけ: `m68kconf.h`(vmpu68 用の設定: RESET 命令・IACK・トレースなどのフックを emu68k に直結)と
  `m68kcpu.h`(例外ベクタへのジャンプ時に `M68K_EXCEPTION_HOOK` を呼ぶ 3 行の追加。例外の記録に使う)。
- 適用: `cd src/third_party/Musashi && patch -p1 < ../patches/musashi-local.patch`
- 命令表 `pi/core/gen/m68kops.c` はビルド時に Musashi の m68kmake で生成される(pi/core/Makefile)。
