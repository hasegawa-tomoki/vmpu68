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

## 4. circle-local.patch — HTTP デーモンの応答を自前で送る経路(vmpu68 の /api/file)

`include/circle/net/http.h`, `include/circle/net/httpdaemon.h`, `lib/net/httpdaemon.cpp`:

- CHTTPDaemon は応答全体を内容バッファ(MAX_CONTENT_SIZE、64 KB)に作って送るので、
  ディスクイメージのような大きなファイルを流せない。`GetContent()` の中で
  `GetSocket()`(protected に追加)を使ってヘッダとファイルを自分で送り、
  `HTTPResponseSent`(= 1、追加)を返すとデーモンは接続を閉じるだけになる。
- 適用後は `lib/net` を再ビルド(`cd lib/net && make`)。

## 5. circle-local.patch — HTTP デーモンは応答後にクライアントの切断を待ってから閉じる(2026-10-13)

`include/circle/net/httpdaemon.h`, `lib/net/httpdaemon.cpp`(`CloseAfterResponse()`):

- 症状: スクリプトや Web UI が短い HTTP 要求を毎秒 20 回以上出し続けると、
  1 分ほどで新しい接続が拒否される(SYN に RST が返り、クライアントは
  「Connection refused」)。カーネルは止まっておらず、しばらくすると回復する。
- 原因: CHTTPDaemon は応答を送るとすぐソケットを削除する(FIN を先に出す
  能動クローズ)ため、閉じた接続が TIME-WAIT(60 秒)に残る。circle の
  TCP は生きている接続オブジェクトが `TCP_MAX_CONNECTIONS`(1000)に達すると
  LISTEN 状態で SYN に RST を返す。tcpdump で確認(RST は seq 0、ack = ISN+1)。
- 修正: 応答を送ったあと受信タイムアウト 300 ms で `Receive()` し、
  クライアント(`Connection: close` なので本文を受け取るとすぐ閉じる)の
  FIN を先に受けてから削除する(受動クローズ: CLOSE-WAIT → LAST-ACK → CLOSED、
  TIME-WAIT なし)。300 ms 以内に閉じないクライアントは従来どおり能動クローズ。
- 適用後は `lib/net` を再ビルド(`cd lib/net && make`)し、カーネルを再リンク。
