# plltest: X68000 なしで VCCPLL リワーク(パッド 126 → 111)を判定する試験用ビットストリーム

core 1.0 基板は U1 の VCCPLL1(パッド 126)が未給電のため、1.2V(パッド 111)へのジャンパが必要。
その良否を X68000 に取り付けずに判定する。

- 基準クロック: Pi の GPCLK0(BCM4 = PI_AD2 → FPGA ピン 114)。コンソール `gpclk 75` = 10 MHz、`gpclk 50` = 15 MHz、`gpclk 0` = 停止。
- 判定出力: PLL ロックを PI_IRQ(FPGA ピン 119 → Pi GPIO22)に出す。コンソール `glev` の `irq(22)` が 1 ならロック。
  LED1: 緑 = ロック、青点滅(約 2.4 Hz)= 基準クロック到達、赤点滅 = PLL クロック動作。
- パッド 49 を入力として使い、本番と同じく上側 PLL(X16/Y33 = VCCPLL1 側)に配置させる(下側は VCCPLL0 = パッド 54 で無給電)。

手順(Pi と J3 シリアル接続、X68000 不要):
1. `httpmgmt.py <ip> put plltest.bin SD:/plltest.bin`
2. シリアルで `fpga SD:/plltest.bin`(書込み+検証 1〜2 分。「構成待ち」の間はメインループが塞がり HTTP/シリアルが無応答になるが固まっていない)
3. `gpclk 75` → `glev` を数回。irq(22)=1 ならロック。`gpclk 50` でも確認。`gpclk 0` で 0 に戻ることも確認。
4. 本番へ戻す: `put fpga_good.bin SD:/fpga.bin` → `fpga SD:/fpga.bin`(または `update <vpk> force`)。

注意: カーネル 0.1.18 以前はモック時に 1 秒ごとの再プローブが GPFSEL0 を書き換えて GPCLK を止めるため判定できない。
2026-09-15 の修正(`s_gpclk_on` で gpclk 動作中は再プローブしない)入りのカーネルが必要。

ビルド: yosys -p "synth_ice40 -top plltest -json plltest.json" plltest.v;
nextpnr-ice40 --hx4k --package tq144 --json plltest.json --pcf plltest.pcf --asc plltest.asc --freq 64; icepack plltest.asc plltest.bin
(nextpnr のログに "constrained PLL 'u_pll' to X16/Y33/pll_3" と出ることを確認する)
