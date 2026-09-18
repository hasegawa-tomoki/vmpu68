# vmpu68 bare-metal firmware (circle)

Raspberry Pi 4 上でベアメタル(circle)動作。起動 → vmpu68 hw 層初期化(FPGA 応答が
あれば実機、なければモック)→ 68000 エミュレータ(Musashi)を core1 で自動起動 →
Wi-Fi 経由の HTTP 管理サーバ + UDP ビーコンで LAN 上の vhd68/vfd68 と相互発見。
UART(基板 USB-C デバッグポート)と Pi 本体 USB-C の CDC シリアルの両方で
対話コンソール。ACT LED 毎秒点滅。

LED1(RGB)の表示(優先順、上ほど優先):
- 白の速い点滅(4 Hz): Web UI の「この基板を探す」(/api/locate)
- 白の点灯: 起動中(メインループに入るまで)
- 赤: SD カードにアクセスした直後(約 0.1 秒)
- 青の点滅(2 Hz): Wi-Fi 接続を試行中(wpa_supplicant.conf はあるがまだつながっていない)。起動から 60 秒たってもつながらなければ緑になる(その後つながれば青)。パスフレーズ違い・country 無し・WPA3 専用などが原因
- 青の点灯: Wi-Fi 接続済み
- 緑: Wi-Fi なし(SD に wpa_supplicant.conf が無い、無線の起動に失敗、または 60 秒つながらない)。コンソール(J3 の USB シリアル)は使える

Wi-Fi は 2.4 GHz と 5 GHz の両方で接続できる(country=JP のとき 5 GHz W52 の 5210 MHz で接続を確認)。WPA2-PSK のみ。
コンソールの `led <0-7>` で色を固定でき、`led auto` で戻る。
FPGA のフラッシュを読み書きしている間(初回プロビジョニングの照合・書込み、ファームウェア更新、`fpga` コマンド)は FPGA を止めるので RGB LED は消灯し、代わりに Pi 本体の ACT LED(緑)が速く点滅する(約 8 Hz)。

## Web UI(http://<board-ip>/)

vhd68/vfd68 と同じ見た目のカード構成:

- **情報** — ファームウェア(Pi 版数と FPGA 版数。FPGA 版数は SD:/fpga.sum の 3 番目の欄で、無ければ起動時に SD:/update.vpk のヘッダかカーネル内蔵のリリース済み sum 表から補う)、名前、メイン RAM、バスクロック、Wi-Fi MAC、IP、稼働時間
- **MPU 設定**(左)— 速度(10/16/24 MHz 相当・Max)、JIT ON|OFF、メイン RAM(ライトバック/ライトスルー)
- **システム設定**(右)— 起動画面 ON|OFF(スイッチの下に「ONにすると起動時にマシン情報を表示します」)と再起動ボタン(確認ダイアログ付き。エミュレータを止めて X68000 の RESET+HALT を駆動してから Pi を再起動する)
- **ファームウェア** — 「ファイルを選択」(.vpk)→「適用+再起動」。
  「上書き更新」チェックで RPi カーネル・FPGA が同じ内容でも書き換える
  (未チェックで両方同じなら「同じファームウェアが既にインストールされています」で拒否)
- **近くの機器** — UDP ビーコン(/api/peers)+ /24 スキャンで見つけた vmpu68/vhd68/vfd68 と Web UI ボタン

HTTP API は webserver.h 冒頭のコメント参照(/api/status, /api/peers, /api/put, /api/update, /api/reboot …)。

## リリースパッケージ(.vpk)

RPi カーネルと FPGA ビットストリームを 1 ファイルにまとめた形式(64 バイトヘッダ "VPK1"、
ラベル、各部のオフセット/長さ/チェックサム)。`release/vmpu68-<version>.vpk`。
バージョンは version.h の `VMPU68_VERSION`(リリースごとに上げる)。

    tools/mkvpk.py build <out.vpk> <kernel8-rpi4.img> [<vmpu68_top.bin>] [-l <label>]
    tools/mkvpk.py info <pkg.vpk>
    tools/mkvpk.py extract <pkg.vpk> <outdir>

インストール(SD 抜き差し不要):

    tools/httpmgmt.py <board-ip> update <pkg.vpk> [force]   # Web UI と同じ経路
    (コンソール)  upd [SD:/pkg.vpk] [force]                # SD 上のパッケージを適用

FPGA 部は sum が現在のビットストリームと同じならスキップ、カーネル部は SD:/kernel8-rpi4.img
と同一ならスキップ。両方同じなら `already installed` で何もしない。`force`(Web UI の
「上書き更新」)で両方とも書き換える。FPGA 書換え中はエミュレータを静止させ、
erase → program → verify の後に再起動する。

## 設定ファイル SD:/vmpu68.cfg(config.{h,cpp})

vhd68.cfg / vfd68.cfg と同じ `key=value` 形式(release/config/vmpu68.cfg.sample 参照)。

    name=X68000 XVI    # 表示名(Web UI ヘッダ・/api/status・ビーコン)。未設定なら "vmpu68"
    mhz=0 / wb=1 / jit=0   # 動作設定(速度上限・メイン RAM ライトバック・JIT)。Web UI / VMPU68.X / コンソールで変えると書き換えられる
    sramboot=0             # 1 = 起動画面(X68030 風の MPU/RAM/CLOCK 表示)。X68000 の SRAM に起動プログラムを書き込む(下記)
    hw=2.1                 # 基板の版数を LAN の「近くの機器」に HW x.y として見せる(省略時は判別した 1.x → 1.0、2.x → 2.0.1。V2.1 は判別できないので hw=2.1 を書く)

Wi-Fi は従来どおり SD:/wpa_supplicant.conf(country=JP・ssid・psk・proto=WPA2・key_mgmt=WPA-PSK。country が無いと日本のチャネルで見つからないことがある)。

## LAN 発見(discovery.{h,cpp})

vhd68/vfd68 の discovery.h と同じ書式で UDP 6868 に 2 秒ごと
`x68pico vmpu68 <version> 80 [name]` をブロードキャストし、受信したビーコンを
最大 8 台・10 秒で失効のテーブルに保持。`/api/peers` で JSON 返却。

## コンソールコマンド(主なもの)

    st / ver / ip / reboot / tbr     ステータス・バージョン・ネット状態・再起動
    rd <a> [b] / wr <a> <v> [b]      X68000 バス読み書き(b=バイト)
    rr <reg> / rw <reg> <v>          FPGA レジスタ生読み書き
    led <0-7> / id / sn / snd / sns  RGB LED・W25Q32 JEDEC ID・スヌープ
    fpga SD:/path.bin / fpgadiag     ビットストリーム書換え・診断
    upd [SD:/pkg.vpk] [force]        .vpk 適用
    put <len> SD:/path / fw <len>    シリアル経由ファイル転送・カーネル自己更新
    ers / ern <cycles> / erg / erun  エミュレータ reset / run / regs / free-run
    espd [MHz] / ecpu [0|30] / auto  エミュレータ速度・MPU 選択・X68000 電源連動
    ebrk / etr / eil / ewt / ewl     ブレークポイント・PC/IACK/ウォッチ履歴
    ebm / eio / bench / vfy / tct    タイミング測定・バス検証
    yt <us> <secs>                   yield 間隔テスト(ネット送信の応答性確認)

### 診断コマンド詳細(0.1.18 で追加・変更したもの)

    erun [0|1]                       1=実行、0=停止。引数なしは状態表示のみ(以前は停止していた)
    ebrk <pc>                        実行番地でのブレークポイント。到達でコアが止まる(ebrk で再アーム、erun 1 で再開)
    ewb <lo> [hi] [pclo] [pchi]      書込み番地でのブレークポイント。[lo,hi] への CPU 書込み直後に止まる。
                                     書込み命令の PC が [pclo,pchi] のときだけ(ROM のクリア等を除外できる)。
                                     ヒット時に命令履歴(etr)を凍結。引数なしは最終ヒット PC の表示のみ
    ewt <a> <len> [a2 <len2>]        アクセス監視(止めない)。2 範囲まで。/api/watch?skip=&n= でバイナリ一括取得
    ewf <0|1>                        1=監視は書込みのみ記録(ポーリングの読みで溢れるのを防ぐ)
    etr [n]                          直近 n 命令の PC(ewb ヒット時は凍結された経路が残る)
    euv <0|1>                        IPL 未設定ベクタ(項目の上位バイトが例外番号)経由の割込みが来たら核を止める。eex はこの種の割込みを sr=8xxx(xxx=レベル)で常に記録(docs §44)
    pw [addr [words [us]]]           phantom 監視: バス書込みの後(間隔 us、既定 1000)に addr から words ワードを読み戻し、CPU が書いていないのに変わったら監視リングを凍結して停止(docs §43)
    joy [port] [val] [ms]            ジョイスティック注入($E9A001/3 の読みを横取り、val は負論理、既定 0xDF=ボタン A)
                                     Web: /api/joy?port=0&btn=a|b|up|down|left|right|start&ms=200
    jit [0|1|f|t|s]                  68000→AArch64 の実行時翻訳(docs/design/jit-plan.md)。引数なし=統計、1/0=有効/無効(既定 0)、
                                     f=翻訳済みコードを全部捨てる、t=現在 PC のブロックを翻訳して機械語を 16 進表示(実行しない)、
                                     s=1 ブロックだけ実行して止める(erun 1 で再開)。全速力で診断(ebrk/ewb/ewt/jprof/espd)が無いときだけ使われる
    jprof [1|0]                      命令・ブロックの分布集計(Web /api/jprof、tools/jprof.py)
    wifi <ssid> <passphrase>         SD:/wpa_supplicant.conf を書く(再起動で接続)。引数なしでファイルの有無
    rm SD:/path                      SD のファイルを消す(fpga.sum を消すと次回起動で初回プロビジョニングが走る)

Web API の追加: /api/joy、/api/watch、/api/config?mhz=&wb=&jit=&sramboot=&name=(/api/status に jit も出る)。情報ポートの設定: +$F0 速度、+$F2 ライトバック、+$F4 JIT、+$F6 起動画面、+$F8 RAM 容量(MB、0=自動)、+$C0〜$DF ホスト名(32 バイト、+$FA に 1 を書くと保存)。

0.1.19: 配布 SD イメージに tryboot.txt(A/B の試験起動で 1 回だけ kernel8-try.img を起動する設定)が入っていなかったのを修正。無いと「お試し起動」は試験カーネルを書くだけで起動せず、安定版のまま再起動していた(docs §41 補足)。Wi-Fi 設定が無いときは無線を起動せず LED は緑、設定があっても 60 秒つながらなければ緑。

0.1.19: RESET 命令が周辺機器をリセットしていなかった(Musashi のフック未登録)のを修正(docs §44)。FD ブートスタブ(mkxdf/bootstub.s)の待ちは VDISP 120 フレームの実時間に変更。

0.1.19 / FPGA 0.1.18(docs §43): §30/§32 の「CRTC の意図しない書込み」は FPGA の Pi インタフェースがホールド読みに古い先読みワードを返していたのが真因で、pi_if.v で修正。対症療法だった tvhs(非表示テキスト VRAM のシャドウ)、fk 監視、bspin、CRTC 書込み後の VRAM 書込み待ちは削除。再現・計測の道具: x68k/vmpu68x/tvstress.s、tools/tvsweep.py(4 面全体の差分、KIND=1 の転送照合)、tools/rbtest.py(書いて→読む)、コンソール `pw`。Web UI の MPU 設定カードは 速度 / JIT ON|OFF / メイン RAM の 3 行、起動画面と再起動はシステム設定カード。

Linux 版と同じ hw.c / vmpu68_io.c をそのままベアメタルでコンパイルしている
(VMPU68_BAREMETAL、GPIO は MMIO 直叩き、ロックはスピンロック)。

## ビルド(Linux 機)

前提: /opt/arm-gnu-toolchain(aarch64-none-elf)、third_party/circle ビルド済み
(circle/Config.mk: AARCH=64, RASPPI=4)。

    cd src/pi/baremetal && make WITH_EMU=1 WITH_WATCHDOG=1   # -> kernel8-rpi4.img

## 配布 SD イメージ(tools/mksd.sh)と初回起動の自動プロビジョニング

    tools/mksd.sh <version> <kernel8-rpi4.img> <vmpu68-<ver>.vpk> <circle/boot> <outdir>   # Linux 機(mtools が要る、root 不要)

MBR + FAT32(64 MB)の `vmpu68-sd-<ver>.img.xz`(Raspberry Pi Imager の「カスタムイメージ」で焼ける)と、既存カードに
上書きする人向けの `vmpu68-sd-<ver>.zip` を作る。中身: circle のブートファイル、config.txt、kernel8-rpi4.img、
同じ版の `vmpu68.vpk`、vmpu68.cfg(sramboot=1、jit=0)、vmpu68.cfg.sample、wpa_supplicant.conf.example、Wi-Fi ファームウェア、README.txt。
Wi-Fi の資格情報と A/B ブートの残骸、fpga.sum は入れない。

初回起動(SD:/vmpu68.vpk があって SD:/fpga.sum が無い = 焼いた直後)にカーネルは hw_init の前に:
1. パッケージのビットストリームと基板の SPI フラッシュを読み比べる(FpgaFlashCompare、約 35 秒。この間と書込み中は Pi の ACT LED が速く点滅、RGB LED は消灯)。
2. 同じなら fpga.sum に版数を記録するだけ。違う/読めないなら `upd` と同じ経路で書き込んで再起動(約 75 秒)。
3. 失敗したら SD:/provision.failed を作って次回は再試行しない(消すか `upd SD:/vmpu68.vpk` でやり直す)。
SRAM の起動画面プログラムは cfg sramboot=1 により RPi カーネルのブートストラップで毎回確認・更新する(他のソフトの
SRAM プログラムが入っていれば触らない)。

「何も書かれていない基板」も「brick からのやり直し」も同じ手順: 焼く → (任意で wpa_supplicant.conf を置く) → 挿して電源 ON。
Wi-Fi はあとからコンソール(Pi の USB-C CDC か J3)で `wifi <ssid> <passphrase>` でも書ける(再起動で接続)。`rm SD:/path` でファイル削除。

## FD イメージとリリース手順(tools/mkxdf、tools/release.sh)

    tools/mkxdf/mkxdf.py <out.xdf> <file>...   # vfd68 プロジェクトの mkxdf.py をそのまま使う(Human68k FORMAT.X と同じ 2HD レイアウト、
                                               # 18.3 名、起動しようとすると案内を出してイジェクト→次のデバイスへ進むブートスタブ入り)。
                                               # mtools の mformat は 1024 バイトセクタでデータ開始位置がずれて Human68k で読めないので使わない。
                                               # vfd68 にマウントして DIR と実行を確認済み。構造の情報源は XEiJ の公開ソースと
                                               # HUMAN302.XDF の自前逆アセンブルのみ(vfd68 と同じ方針。FDX68 由来の情報は使わない)。
    tools/release.sh <version> [--tag] [--github]
                                          # version.h 更新 → カーネルビルド → release/vmpu68-<ver>.vpk → SD イメージ →
                                          # release/Human68k/VMPU68.XDF(VMPU68.X 入り)→ --tag で git tag v<ver> → --github で
                                          # git push --tags + gh release create(上記を添付。release/notes-<ver>.md をリリースノートに)

## テスト用 SD の作り方(FAT32 でフォーマットした SD に)

1. third_party/circle/boot/ の内容一式をコピー
   (make 済み: start4.elf, fixup4.dat, bcm2711-rpi-4-b.dtb ほか)
2. config.txt に以下があることを確認(circle boot/README 準拠):
       arm_64bit=1
       kernel=kernel8-rpi4.img
3. release/*.vpk を `mkvpk.py extract` して kernel8-rpi4.img をコピー
   (FPGA ビットストリームは初回起動後に Web UI から .vpk を適用すれば書き込まれる)
4. 表示名を付けるなら release/config/vmpu68.cfg.sample を vmpu68.cfg としてコピーし name= を書く
5. WLAN を使うなら wlan-files/ の brcmfmac* と、wlan-files/wpa_supplicant.conf.example を
   自分の SSID/パスフレーズで埋めた wpa_supplicant.conf をコピー

挿して電源 ON → ACT LED が毎秒瞬く。Mac と Pi の USB-C を繋ぐと
CDC シリアル(/dev/tty.usbmodem*)にハートビートが出る。
基板があれば USB-C デバッグポート(CH340E)側にもログが出る。

## 注意(circle の協調スケジューラ)

ネットワークタスクはメインループが Yield/MsSleep したときだけ送受信する。
メインループは 20 ms 以内に必ず yield すること(長い SPI 転送やフラッシュ書換え中は
ServiceDuringUpdate() で Poll + Yield を挟む)。詳細は docs/design/baremetal-findings.md。

## 基板 1.x / 2.x 共通カーネル(基板プロファイル)

同じカーネルが core 1.x と core 2.x の両方で動く。GPIO の割当は実行時に選ぶ。

- 起動時に SD の vmpu68.cfg に `board=1` または `board=2` があればそれを使う。無ければ 2.x の配線 → 1.x の配線の順に
  FPGA の署名(0x56)を読んで判別する。どちらも応答しない(FPGA 未書込みの新品基板など)ときは 2.x とみなす。
- コンソール UART も基板で変わる: 1.x = UART0(GPIO14/15、J3)、2.x = UART2(GPIO0/1、J3)。
- 判別結果はブートログの `board: core 1.x (probed), console UART0` と、`/api/status` の `board`、コンソールの `board` で見える。
- `board 1` / `board 2` / `board auto` で vmpu68.cfg の board= を書き換える(次回起動から有効)。新品の 2.x 基板で
  FPGA を書く前に判別が 1.x に転ぶことはない(1.x は必ず FPGA が応答する)が、念のため `board 2` を書いておいてもよい。
- 1.x / 2.x の割当表は vmpu68.h の先頭コメント(2.x は SMI の SD0-15 = GPIO8-23 配列)。
- 2.x の FPGA は Pi から給電される(1.x は X68000 から)。X68000 が OFF でも FPGA は署名に応答するが、内部クロック
  (バスクロック 16 MHz を PLL で逓倍)が止まっているのでレジスタ書込みは残らない(hello マーカーが立たない)。
  監視は PLL ロック無しのときは「再コンフィグ」扱いにせず `hw: FPGA present but X68000 clock stopped (machine off?) - waiting`
  を 1 回出して待ち、クロックが戻ったら `hw: X68000 clock back - re-initialising the FPGA` → 通常の電源 ON 手順で起動する。
  この間 CORE の RGB LED は消灯(FPGA が駆動できない)。REG2 の低位バイトは 0x04(ai_ok のみ)で固定。
- 2.0 基板の立上げ(2026-09-18): 1.0.0 の SD をそのまま挿して起動 → `board: core 2.x (default)`、J3 は UART2 で通る、
  FPGA 未書込みなのでモックモード → `httpmgmt.py <IP> update vmpu68-1.0.0.vpk force` でフラッシュ(erase/program/verify OK、
  flash id EF 40 16)→ 再起動で `core 2.x (probed)`、fpga_sum 64DA5713(ビットストリームは 1.x と共通、.pcf 不変)。

## SMI 転送(core 2.x、smi=)

2.x の Pi 側配線は SoC の SMI(Secondary Memory Interface、並列バス機能)の固定ピンに合わせてある
(SA0/SA1 = GPIO5/4 = REG_A0/A1、SWE_N/SOE_N = GPIO7/6 = WR#/RD#、SD0-15 = GPIO8-23 = AD0-15)。
`smi=1`(既定)のとき、レジスタの読み書きは GPIO のストローブ操作でなく SMI の直接モード(1 転送ずつ
SMIDA/SMIDD/SMIDCS を叩く)で行う。プロトコル(REG0〜3、自動開始、アドレス自動更新、PI_IRQ 完了)は
同じ。ホールド読み(RD# を下げたまま PI_IRQ を待つ)は SMI ではできないので、待ち付き読みのあいだだけ
AD/REG_A/RD# を GPIO に戻して(GPFSEL の書込み 4 回、投入のみで数 ns)GPIO と同じホールド読みを行い、終わったら
SMI に返す(vmpu68_io.c hold_read の smi_on 分岐、smi_capture がその GPFSEL 像を持つ)。最初に試した
「PI_IRQ を待ってから DATA を SMI で 1 回読む」形は DATA 転送 ≈340 ns が余計で、si.x の machine が
177 → 164 % に落ちた。混成にして 199 %(system 131 %)。

- タイミングは SMI クロック(PLLD 750 MHz / 6 = 125 MHz、8 ns)単位: 書込み setup/strobe/hold = 1/4/2、
  読出し 1/9/1、転送間 2。FPGA は 61 MHz でストローブを同期し、WR# が低い最初のクロックで AD/REG_A を
  取り込む。1/3/2 まで詰めても vfy は通ったが余裕を見て 1/4/2。
- 効果: 1 転送の中身は SMI の MMIO 往復(読み 73 ns ×2〜3 回)が主で、ストローブ幅ではない。書込みは投入のみで
  CPU 側 ≈10 ns(転送は裏で進む)。`bench`: reg_write 276→214 ns、reg_read 337→343 ns(同等)、posted write
  451→393。待ち付き読み(`ewait` の read phases)は GPIO 248+92+441 ≈ 850 ns → 混成 11+4+391 ≈ 450 ns(+SMI の
  完了待ち)。**si.x: processor 2355 → 2358 %、system 120 → 131 %、machine 177.6 → 199.1 %**(同じ 2.0 基板、
  GPIO → SMI 混成)。CPU 側で決まる換算クロック(VMPU68.X の CLOCK)は 164 MHz で変わらない。
- 切替: コンソール `smi 0|1 [div ws wst wh rs rst rh pace]`(エミュレータを止める。cfg に保存、`erun 1` で再開)、
  `smif <flags>`(実験: bit0 SMIDA 省略、bit1 DONE クリア省略、bit2 書込みの完了待ち省略 = **書込みが落ちる**ので不可)、
  `smid <ns>`(読出しの最初の DONE ポーリング前の待ち: 効果なし)、`smib`(MMIO の所要時間)。
  `/api/status` の `smi`。1.x では常に無効(`smi=` は無視)。
- 検証(2026-09-18、core 2.0): 署名 + hello の往復、vfy テキスト VRAM 16384 語 ×5・GVRAM 8192 語、tvsweep 0/0 ×3、
  Human68k 起動・FD の DMA・VMPU68.X。注意: vfy をメイン RAM に掛けるとライトバックの書き戻しが試験パターンを
  上書きして「bad」が出る(GPIO でも同じ)。RAM の検証はテキスト VRAM で行う。
- 主な余地は FIFO/DMA モードでの連続転送だが、レジスタが 1 語ごとに違う(ADDR/CTRL/DATA)のと先読み FIFO の
  語数確認が要るので、現行のレジスタ設計のままでは効かない。

## ADPCM 再生中の投入書込みの上限(pmax)

FPGA はバス要求(BR)より前に投入された書込みを先に全部済ませてから DMAC にバスを渡すので、投入キューが深いと
DMAC が数十 µs 待つ。ADPCM(MSM6258)は 64〜128 µs ごとに 1 バイトを DMA で受け取りバッファが無いため、この待ちが
そのまま音の乱れになる(悪魔城ドラキュラで確認)。FD 転送中に上限を 6 に絞るのと同じ仕組みで、ADPCM の DMA が
動いている間(CPU の $E92000〜3 / DMAC ch3 へのアクセス、または DMAC の $E92002 への書込みのスヌープ記録から 300 ms)は
上限を 2 に絞る。コンソール `pmax [loose] [adpcm]` で変更、表示に fdc/adpcm の tight/idle。

## 起動画面(SRAM 起動プログラム、sramboot=)

X68000 の IPL は通常起動では画面に何も出さない。システム設定の「起動画面 ON」にすると、Pi が X68000 の電池バックアップ SRAM に
起動プログラム(x68k/vmpu68x/vmpu68.s の SRAM 版、`build.sh` が sramboot_bin.h に埋め込む)を書き、$ED0018 を $B000
(SRAM 起動)にする。IPL は $ED0100 の先頭が BRA なら jsr で呼び(IPL ROM $FF0310)、RTS で戻ると通常の起動(FD → HD)に進むので、
プログラムは 768x512 のテキスト画面にして X68030 風の 4 行(SHARP 〜 X68000 SERIES / BIOS ROM version / MPU / CLOCK)と
設定行(SPEED / RAM / JIT)を描き、1.5 秒見せてから戻る(起動は約 2 秒遅くなる。Human68k が起動すると画面は消える)。ROM は書き換えない。
OFF にすると $ED0018 を $0000 に戻し、$ED0100 の先頭を 0 にする(他のソフトの SRAM プログラムが入っているときは触らない)。
RPi カーネルのブートストラップのたびに SRAM の内容を埋め込みと比べ、消えていたり古ければ書き直す(コンソール/ログ `sramboot:`)。
SRAM への書込みは $E8E00D = $31 で許可してから行う(emu68k_sram_write)。

## 起動画面 1 行目の加工(斜体の X68000、縦 2/3 の SHARP)

1 行目は普通に文字で印字したあと、テキスト VRAM を直接いじる(行は IOCS _B_LOCATE で得たカーソル行 − 4)。
- 水色「X68000」(桁 29 から 6 セル): 16 ラスタを上ほど右へずらす(4 ラスタごとに 1 ドット、最大 3 ドット。右隣の空白 1 セルに
  はみ出す)斜体風(shear_x68000)。
- 黄「SHARP」(桁 0 から 5 セル): 16 ラスタを 11 ラスタ(縦 2/3)に潰して下寄せ(ラスタ 5〜15)に置く(squash_sharp。
  1 列ずつ 16 ラスタをスタックに写してから sq_tab の対応で OR する。1/2 は小さすぎるとユーザー判断で 2/3 に)。
VMPU68.X / VMPU68 -b / SRAM 起動画面の 3 つとも同じ。

## VMPU68.X(Human68k 側のコマンド、release/Human68k/VMPU68.X)

x68k/vmpu68x/vmpu68.s(`build.sh`)。情報ポート $ECD000(1.0.0 は $ECFF00。FineScanner-X68 のスロット F と BANK RAM ボードの $ECFFFF に重なるので 1.1.0 で移動。VMPU68.X は新旧両方を探す)を読んで表示・設定する。VMPU68.SYS(CONFIG.SYS のドライバ)は
2026-09-16 に廃止し、起動時の表示は「起動画面」設定(SRAM 起動プログラム)か AUTOEXEC.BAT の `VMPU68 -b` で行う。

    VMPU68            X68030 風の 4 行 + 現在の設定(SPEED / RAM / JIT、速度上限なしは Max)+ 1 行空けて使い方
    VMPU68 -b         4 行だけ(AUTOEXEC.BAT 用)
    VMPU68 -s<MHz>    速度 -s10 / -s16 / -s24 / -s0(Max)。基板が適用して vmpu68.cfg に保存
    VMPU68 -i1|-i0    起動画面(SRAM 起動プログラム)の ON/OFF
    VMPU68 -m<MB>     メイン RAM 容量の固定値(1〜12、-m0 で自動検出)。次の起動から有効
    VMPU68 -n<name>   ホスト名(31 バイトまで。次の " -" まで、空白を含められる)。Web UI の「ホスト名を設定」と同じ
    VMPU68 -w<0|1>    メイン RAM ライトバック(1)/ライトスルー(0)
    VMPU68 -j<0|1>    JIT 有効(1)/無効(0)
