; SPDX-License-Identifier: MIT
; VMPU68.X / SRAM 起動プログラム - Human68k から VMPU68 基板の情報を表示・設定する
;   (デバイスドライバ形 VMPU68.SYS は 2026-09-16 に廃止。起動時の表示は SRAM 起動プログラム(基板側の「起動画面」)か
;    AUTOEXEC.BAT の VMPU68 -b で行う。デバイスヘッダのコードは残してあるが build.sh では作らない)
;
;   表示(機種、ROM 版数、MPU=VMPU68 の版数と換算クロック、バスクロック、RAM)を出す。
;   表示後は文字デバイス "VMPU68" として常駐し、TYPE VMPU68 でも同じ情報を(計測し直して)読める。
;   1 行目の小さな大文字は ROM の合成字形: $F2xx を印字すると IOCS(_FNTADR)が下位バイトの 8x8 ANK
;   フォントをセルの下半分に置く(030_omake と同じ見た目。$F0xx なら上半分)。外字定義は不要。
;
;   情報は基板側の情報ポート($ECD000〜$ECD0FF、emu68k.c 参照。1.0.0 は $ECFF00 で、X 版はそちらも探す)から読む。
;     +0  'VMPU'            +4  us カウンタ(32 ビット。+4 を読むとラッチ、+6 が下位ワード)
;     +8  換算 MHz x10(基板側計測)   +10 バス MHz x10   +12 RAM MB   +14 フラグ
;     +16 版数(16)   +32 ビルド(32)   +64 自由テキスト(192)
;
;   ビルド(build.sh): vasmm68k_mot -Fbin -m68000 -o vmpu68.bin vmpu68.s
;                     vasmm68k_mot -Fbin -m68000 -DRMODE=1 -o VMPU68.R vmpu68.s   (コマンドラインから試す版)
;   コードは位置独立。Human68k の DEVICE= はヘッダ内のアドレスを X 形式の再配置表で直すので
;   .SYS は X 形式でなければならない(R 形式だとヘッダが再配置されず組み込まれない)。

; 表示バッファの番地を \1 に(SRAM 版は RAM の SRAM_BUF、それ以外は自身の linebuf)
LINEBUF         macro
                ifd     SRAMMODE
                lea     SRAM_BUF,\1
                else
                lea     linebuf(pc),\1
                endc
                endm

INFO            equ     $ECD000         ; 情報ポート(1.0.1〜。拡張 I/O のスロット D 先頭 256 バイト)
INFO_OLD        equ     $ECFF00         ; 1.0.0 のカーネルの番地(FineScanner-X68 スロット F / BANK RAM ボードと重なるため移動)
; LEAINFO \1: 情報ポートの番地を \1 へ。X 版は build_text が見つけた番地(infobase)、SRAM 版は固定
LEAINFO         macro
                ifd     XMODE
                move.l  infobase(pc),\1
                elseif
                lea     INFO,\1
                endc
                endm
IOCS_B_PRINT    equ     $21
IOCS_ROMVER     equ     $8F
IOCS_CRTMOD     equ     $10
LOOPS_OUTER     equ     100             ; 計測ループ: 100 x 65536 回の dbra(1 回 10 サイクル)
CYC_X10         equ     655360000       ; 65536000 サイクル x 10 (MHz x10 = CYC_X10 / us)

                ifd     RMODE
                bra.w   rstart
                endc
                ifd     XMODE
                bra.w   xstart
                endc
                ifd     SRAMMODE
; SRAM 起動プログラム(SRAM 版: build.sh -DSRAMMODE): 基板側が $ED0100 に書き込み、$ED0018 = $B000 にする。
; IPL は $ED0100 の先頭が BRA($60)なら jsr で呼び、RTS で戻ると通常の起動(FD → HD)に進む(IPL ROM $FF0310)。
; 通常起動では IPL は画面に何も出さないので、ここで X68030 風の 4 行を描いて SRAM_HOLD_US だけ見せる。
; SRAM は書き込み禁止なので、表示バッファは RAM の SRAM_BUF(IPL の作業域 $0〜$1FFF、スタック $2000 より上)を使う。
                org     $ED0100
SRAM_BUF        equ     $3000
SRAM_HOLD_US    equ     1500000
                bra.w   sstart
                dc.b    'VMPU68SB'      ; +4: 基板側が識別する署名
                dc.w    1               ; +12: SRAM 版の版数
                endc

; ---------------------------------------------------------------- デバイスヘッダ
devhead:
                dc.l    -1              ; 次のデバイス(なし)
                dc.w    $8000           ; 文字デバイス
                dc.l    strategy        ; +6  (X 形式で再配置)
                dc.l    interrupt       ; +10 (X 形式で再配置)
                dc.b    'VMPU68  '      ; デバイス名(8 文字)

reqptr:         dc.l    0               ; 要求ヘッダ(strategy で受け取る)
rdpos:          dc.l    0               ; READ の読み出し位置(OPEN で 0 に戻す)
txtlen:         dc.l    0               ; linebuf の文字数

strategy:                               ; レジスタは全て保存する(DOS は a0 が残る前提で 1 文字ずつ呼ぶ)
                move.l  a0,-(sp)
                lea     reqptr(pc),a0
                move.l  a5,(a0)
                movea.l (sp)+,a0
                rts

; 要求ヘッダ: +2 コマンド、+3/+4 エラー(下位/上位)、+13 属性、+14 アドレス、+18 長さ
; 文字デバイスの作法(condrv.s / Human68k 逆アセンブルより): 入力(4)は 18(a5) の長さぶん必ず埋め、
; 18(a5) は転送数として読まれる。入力状態(6)は d0 = 0 入力可 / 1 不可。先読み(5)は 13(a5) に 1 文字。
interrupt:
                movem.l d0-d7/a0-a6,-(sp)
                movea.l reqptr(pc),a5
                moveq   #0,d0
                move.b  2(a5),d1
                beq     .init           ; 0: 初期化
                cmp.b   #4,d1
                beq     .read           ; 4: 読み出し
                cmp.b   #13,d1
                beq     .open           ; 13: オープン
                cmp.b   #14,d1
                beq     .ok             ; 14: クローズ
                cmp.b   #5,d1
                beq     .peek           ; 5: 先読み
                cmp.b   #6,d1
                beq     .instat         ; 6: 入力状態
                cmp.b   #7,d1
                beq     .ok             ; 7: 入力バッファ破棄
                cmp.b   #10,d1
                beq     .ok             ; 10: 出力状態
                cmp.b   #11,d1
                beq     .ok             ; 11: 出力バッファ破棄
                move.w  #$5003,d0       ; それ以外は「無効なコマンド」
                bra     .done
.init:
                bsr     build_text
                lea     msg_crlf(pc),a1
                moveq   #IOCS_B_PRINT,d0
                trap    #15
                LINEBUF a1
                moveq   #IOCS_B_PRINT,d0
                trap    #15
                bsr     shear_x68000
                bsr     squash_sharp
                moveq   #0,d0
                lea     devend(pc),a0
                move.l  a0,14(a5)       ; 常駐終了アドレス
                bra     .done
.open:
                bsr     build_text      ; 開くたびに計測し直す
                lea     rdpos(pc),a0
                clr.l   (a0)
                moveq   #0,d0
                bra     .done
.instat:                                ; 6: 入力状態: d0 = 0 入力可 / 1 不可
                move.l  txtlen(pc),d3
                addq.l  #1,d3           ; 本文 + ^Z
                cmp.l   rdpos(pc),d3
                bhi     .ok
                moveq   #1,d0
                bra     .done
.peek:                                  ; 5: 先読み: 次の 1 文字を 13(a5) に(終端以降は ^Z)
                bsr     nextbyte
                move.b  d0,13(a5)
                moveq   #0,d0
                bra     .done
.read:                                  ; 4: 入力: 要求長ぶん必ず埋める(長さ欄は触らない)
                movea.l 14(a5),a1       ; 転送先
                move.l  18(a5),d2       ; 要求長
                beq     .ok
.rdl:           bsr     nextbyte
                move.b  d0,(a1)+
                lea     rdpos(pc),a3
                addq.l  #1,(a3)
                subq.l  #1,d2
                bne     .rdl
                bra     .ok
.ok:
                moveq   #0,d0
.done:
                move.b  d0,3(a5)        ; エラー下位
                lsr.w   #8,d0
                move.b  d0,4(a5)        ; エラー上位
                movem.l (sp)+,d0-d7/a0-a6
                rts

; nextbyte: d0.b = rdpos 位置の文字(本文を読み切ったら ^Z)。rdpos は進めない。
nextbyte:
                move.l  rdpos(pc),d0
                cmp.l   txtlen(pc),d0
                bcc     .eof
                LINEBUF a0
                move.b  (a0,d0.l),d0
                rts
.eof:           moveq   #$1A,d0
                rts

; ---------------------------------------------------------------- 表示文字列の生成
; linebuf に NUL 終端の文字列を作り txtlen に長さを入れる。スーパーバイザモードで呼ぶこと
; (バスエラーベクタを一時的に差し替える)。a6 = 情報ポート。
build_text:
                LEAINFO a6
                LINEBUF a2
                ; 基板がない(バスエラー)場合に備えて、ベクタ 2 を一時的に差し替えて magic を読む
                ; (バスエラーは berr → probe_fail へ)
                move.l  $8.w,d7
                lea     berr(pc),a0
                move.l  a0,$8.w
                move.l  (a6),d0
                cmp.l   #'VMPU',d0
                beq     bt_ok
probe_fail:                             ; (グローバルラベル: berr から来る。以下のラベルも同じ理由でグローバル)
                ifd     XMODE
                cmp.l   #INFO_OLD,a6    ; 旧番地も試した後なら無し
                beq     bt_nf
                lea     INFO_OLD,a6     ; 1.0.0 のカーネル: 旧番地を試す(berr がベクタを戻すので再設定)
                lea     berr(pc),a0
                move.l  a0,$8.w
                move.l  (a6),d0
                cmp.l   #'VMPU',d0
                bne     bt_nf
                lea     infobase(pc),a0
                move.l  a6,(a0)         ; 以後は旧番地を使う
                bra     bt_ok
                endc
bt_nf:          move.l  d7,$8.w
                bra     notfound
bt_ok:          move.l  d7,$8.w

                ; ---- 実効速度の計測(既知サイクル数のループを us カウンタで挟む)
                bsr     read_us
                move.l  d0,d6
                move.w  #LOOPS_OUTER-1,d1
.outer:         move.w  #65535,d2
.inner:         dbra    d2,.inner
                dbra    d1,.outer
                bsr     read_us
                sub.l   d6,d0           ; 経過 us
                beq     .nodiv
                move.l  #CYC_X10,d1
                bsr     udiv32          ; d2 = MHz x10
                bra     .measured
.nodiv:         moveq   #0,d2
.measured:
                move.l  d2,d5           ; d5 = 計測した MHz x10

                ; ---- 1 行目: SHARP(黄) PERSONAL WORKSTATION(小) X68000(水色) SERIES(小)
                lea     msg_sharp(pc),a0
                bsr     puts
                lea     msg_bold(pc),a0
                bsr     puts
                lea     msg_personal(pc),a0
                bsr     puts_small      ; 小さな大文字は 030_omake と同じく太字属性で
                lea     msg_x68000(pc),a0
                bsr     puts
                lea     msg_bold(pc),a0
                bsr     puts
                lea     msg_series(pc),a0
                bsr     puts_small
                lea     msg_normal(pc),a0
                bsr     puts
                lea     msg_crlf(pc),a0
                bsr     puts

                ; ---- 2 行目: BIOS ROM version v.v 'yy.mm.dd
                moveq   #IOCS_ROMVER-256,d0
                trap    #15             ; d0 = $VVYYMMDD (BCD)
                move.l  d0,d6
                lea     msg_bios(pc),a0
                bsr     puts
                move.l  d6,d0
                swap    d0
                lsr.w   #8,d0
                move.b  d0,d1
                lsr.b   #4,d0
                bsr     putdigit
                move.b  #'.',(a2)+
                move.b  d1,d0
                bsr     putdigit
                move.b  #' ',(a2)+
                move.b  #$27,(a2)+      ; '
                move.l  d6,d0
                swap    d0
                bsr     putbcd          ; yy
                move.b  #'.',(a2)+
                move.l  d6,d0
                lsr.w   #8,d0
                bsr     putbcd          ; mm
                move.b  #'.',(a2)+
                move.l  d6,d0
                bsr     putbcd          ; dd
                lea     msg_crlf(pc),a0
                bsr     puts

                ; ---- 3 行目: MPU: VMPU68 Vx.x.x   RAM: xMB   Bus: xx.xMHz
                lea     msg_mpu(pc),a0
                bsr     puts
                lea     16(a6),a0       ; 版数
                bsr     puts_port
                lea     msg_ram(pc),a0
                bsr     puts
                moveq   #0,d0
                move.w  12(a6),d0
                bsr     putdec
                lea     msg_mb(pc),a0
                bsr     puts
                lea     msg_bus(pc),a0
                bsr     puts
                moveq   #0,d0
                move.w  10(a6),d0
                bsr     putdec1
                lea     msg_mhz(pc),a0
                bsr     puts
                lea     msg_crlf(pc),a0
                bsr     puts

                ; ---- 4 行目: CLOCK: xxx.xMHz equiv. (xx.xx of MC68000 10MHz)
                lea     msg_clock(pc),a0
                bsr     puts
                move.l  d5,d0
                addq.l  #5,d0
                divu    #10,d0
                and.l   #$FFFF,d0       ; 四捨五入した整数 MHz
                bsr     putdec
                lea     msg_equiv(pc),a0
                bsr     puts
                move.l  d5,d0
                addq.l  #5,d0
                divu    #10,d0
                and.l   #$FFFF,d0       ; (MHz x10) / 10 = 倍率 x10
                bsr     putdec1
                lea     msg_ratio(pc),a0
                bsr     puts
                bra     fin

notfound:
                ifd     XMODE
                lea     infobase(pc),a0
                clr.l   (a0)            ; 以後 LEAINFO は 0 を返す(再読みしない)
                endc
                lea     msg_none(pc),a0
                bsr     puts
fin:
                clr.b   (a2)
                ifnd    SRAMMODE        ; (SRAM 版: txtlen は SRAM 上で書けないし使わない)
                LINEBUF a0
                suba.l  a0,a2
                move.l  a2,d0
                lea     txtlen(pc),a0
                move.l  d0,(a0)
                endc
                rts

; バスエラー(68000 の 14 バイトフレーム): 例外フレームの拡張部を捨て、probe_fail へ戻す(旧番地を試すか notfound)
berr:
                addq.l  #8,sp
                lea     probe_fail(pc),a0
                move.l  a0,2(sp)
                move.l  d7,$8.w         ; 元のベクタへ戻す
                rte

; shear_x68000: 印字した 1 行目の「X68000」(X68_COL 桁から 6 セル)を斜体風にする: テキスト VRAM 面 0 の 16 ラスタを
;               上ほど右へずらす(4 ラスタごとに 1 ドット、最大 3 ドット。右隣の空白 1 セルにはみ出す)。
;               4 行(+改行)を印字した直後に呼ぶこと: 1 行目 = 現在のカーソル行 - 4。スーパーバイザモード。
shear_x68000:
                movem.l d0-d7/a0-a1,-(sp)
                moveq   #-1,d1
                moveq   #IOCS_B_LOCATE,d0
                trap    #15             ; d0 = x<<16 | y
                and.l   #$FFFF,d0
                subq.l  #4,d0
                bcs     .end
                lsl.l   #8,d0           ; 行 x 2048(16 ラスタ x 128 バイト)
                lsl.l   #3,d0
                lea     TVRAM+X68_COL,a1
                adda.l  d0,a1
                moveq   #0,d3           ; ラスタ 0..15
.row:           moveq   #15,d1
                sub.w   d3,d1
                lsr.w   #2,d1           ; d1 = ずらす量 (15-y)/4 = 3..0
                moveq   #6,d4           ; バイト 6(空白セル)から 0 へ、その場で右シフト(下位側の元の値を先に使う)
.byte:          move.b  (a1,d4.w),d7
                lsr.b   d1,d7           ; old[i] >> sh
                tst.w   d4
                beq     .store
                moveq   #0,d5
                move.b  -1(a1,d4.w),d5
                lsl.w   #8,d5
                lsr.w   d1,d5           ; 下位バイト = old[i-1] << (8-sh)
                or.b    d5,d7
.store:         move.b  d7,(a1,d4.w)
                subq.w  #1,d4
                bpl     .byte
                lea     128(a1),a1
                addq.w  #1,d3
                cmp.w   #16,d3
                blt     .row
.end:           movem.l (sp)+,d0-d7/a0-a1
                rts

; squash_sharp: 1 行目の「SHARP」(桁 0 から 5 セル、黄 = 面 1)を縦 2/3(16 → 11 ラスタ)に潰して下寄せ(ラスタ 5〜15)に置く。
;               1 列(バイト)ずつ 16 ラスタをスタックに写してから、sq_tab の (読み元開始, 本数) で OR して書く。
;               shear_x68000 と同じく 4 行印字直後に呼ぶ。面 0/1 両方を処理。
squash_sharp:
                movem.l d0-d5/a0-a3,-(sp)
                moveq   #-1,d1
                moveq   #IOCS_B_LOCATE,d0
                trap    #15
                and.l   #$FFFF,d0
                subq.l  #4,d0
                bcs     .end
                lsl.l   #8,d0
                lsl.l   #3,d0           ; 行 x 2048
                lea     TVRAM,a0
                adda.l  d0,a0           ; 面 0 の行先頭
                lea     -16(sp),sp
                movea.l sp,a2           ; 16 ラスタの写し
                lea     sq_tab(pc),a3
                moveq   #1,d4           ; 面 0、面 1
.plane:         moveq   #4,d3           ; 5 バイト(セル)
                movea.l a0,a1
.col:           moveq   #0,d0           ; 読み元をスタックへ写して消す
                moveq   #0,d1
.rd:            move.b  (a1,d0.w),(a2,d1.w)
                clr.b   (a1,d0.w)
                add.w   #128,d0
                addq.w  #1,d1
                cmp.w   #16,d1
                blt     .rd
                moveq   #0,d1           ; 出力 j = 0..10
.out:           move.w  d1,d0
                add.w   d0,d0
                move.b  (a3,d0.w),d5    ; 読み元開始
                move.b  1(a3,d0.w),d2   ; 本数
                ext.w   d5
                moveq   #0,d0
.acc:           or.b    (a2,d5.w),d0
                addq.w  #1,d5
                subq.b  #1,d2
                bne     .acc
                move.w  d1,d5
                addq.w  #5,d5           ; ラスタ 5+j へ
                lsl.w   #7,d5
                move.b  d0,(a1,d5.w)
                addq.w  #1,d1
                cmp.w   #11,d1
                blt     .out
                addq.l  #1,a1
                dbra    d3,.col
                adda.l  #$20000,a0      ; 次の面
                dbra    d4,.plane
                lea     16(sp),sp
.end:           movem.l (sp)+,d0-d5/a0-a3
                rts
sq_tab:         dc.b    0,1, 1,1, 2,2, 4,1, 5,2, 7,1, 8,2, 10,1, 11,2, 13,1, 14,2   ; 16 ラスタ → 11 ラスタ
                even

; build_settings: 設定行 "SPEED: ...   RAM: ...   JIT: ..." を a2 へ(NUL は書かない)。a6 = 情報ポート。
;                 設定は情報ポートの +$F0(速度)/+$F2(ライトバック)/+$F4(JIT)。
SET_SPEED       equ     $F0
SET_WB          equ     $F2
SET_JIT         equ     $F4
SET_SB          equ     $F6             ; 起動画面 0/1
SET_RAM         equ     $F8             ; メイン RAM の固定値(MB、0 = 自動)
SET_NAME        equ     $FA             ; 1 を書くと $C0 の文字列をホスト名として保存
SET_SMI         equ     $FC             ; SMI 転送 0/1(読みで 2 = この基板では使えない = V1.0)
NAMEBUF         equ     $C0             ; ホスト名(32 バイト、NUL 終端。基板が現在の名前を置き、-n はここへ書く)
build_settings:
                lea     msg_speed(pc),a0
                bsr     puts
                move.w  SET_SPEED(a6),d0
                tst.w   d0
                beq     .unl
                bsr     putdec
                lea     msg_mhzeq(pc),a0
                bsr     puts
                bra     .ram
.unl:           lea     msg_unlim(pc),a0
                bsr     puts
.ram:           lea     msg_ramset(pc),a0
                bsr     puts
                lea     msg_wt(pc),a0
                tst.w   SET_WB(a6)
                beq     .wtxt
                lea     msg_wb(pc),a0
.wtxt:          bsr     puts
                lea     msg_jitset(pc),a0
                bsr     puts
                lea     msg_off(pc),a0
                tst.w   SET_JIT(a6)
                beq     .jtxt
                lea     msg_on(pc),a0
.jtxt:          bra     puts
                ifd     XMODE
; build_settings2: 2 行目 "HOST: ...   BOOT SCREEN: ...   RAM SIZE: ..." を a2 へ。a6 = 情報ポート。
build_settings2:
                lea     msg_host(pc),a0
                bsr     puts
                lea     NAMEBUF(a6),a0
                moveq   #31,d3
.hc:            move.b  (a0)+,d0
                beq     .hdone
                move.b  d0,(a2)+
                dbra    d3,.hc
.hdone:         lea     msg_bootscr(pc),a0
                bsr     puts
                lea     msg_off(pc),a0
                tst.w   SET_SB(a6)
                beq     .btxt
                lea     msg_on(pc),a0
.btxt:          bsr     puts
                lea     msg_ramsz(pc),a0
                bsr     puts
                move.w  SET_RAM(a6),d0
                tst.w   d0
                beq     .auto
                bsr     putdec
                lea     msg_mbsp(pc),a0
                bsr     puts
                bra     .smi
.auto:          lea     msg_auto(pc),a0
                bsr     puts
.smi:           lea     msg_smi(pc),a0
                bsr     puts
                move.w  SET_SMI(a6),d0  ; 0/1、2 = V1.0(SMI 無し)
                lea     msg_off(pc),a0
                beq     .stxt
                lea     msg_on(pc),a0
                cmp.w   #1,d0
                beq     .stxt
                lea     msg_na(pc),a0
.stxt:          bra     puts
msg_host:       dc.b    'HOST: ',0
msg_smi:        dc.b    '   SMI: ',0
msg_na:         dc.b    '-',0
msg_bootscr:    dc.b    '   BOOT SCREEN: ',0
msg_ramsz:      dc.b    '   RAM SIZE: ',0
msg_mbsp:       dc.b    ' MB',0
msg_auto:       dc.b    'auto',0
                even
                endc
msg_speed:      dc.b    'SPEED: ',0
msg_mhzeq:      dc.b    'MHz equivalent',0
msg_unlim:      dc.b    'Max',0
msg_ramset:     dc.b    '   RAM: ',0
msg_wb:         dc.b    'write-back',0
msg_wt:         dc.b    'write-through',0
msg_jitset:     dc.b    '   JIT: ',0
msg_on:         dc.b    'on',0
msg_off:        dc.b    'off',0
                even

; ---------------------------------------------------------------- 小道具
; read_us: d0 = 基板の us カウンタ(32 ビット)。+4 を読むとラッチされる。
read_us:
                move.w  4(a6),d0
                swap    d0
                move.w  6(a6),d0
                rts

; udiv32: d1 / d0 -> d2(商)、d4 に余り。d3 を壊す。
udiv32:
                moveq   #0,d4
                moveq   #0,d2
                moveq   #31,d3
.l:             lsl.l   #1,d1
                roxl.l  #1,d4
                lsl.l   #1,d2
                cmp.l   d0,d4
                bcs     .s
                sub.l   d0,d4
                addq.l  #1,d2
.s:             dbra    d3,.l
                rts

; puts: a0 の NUL 終端文字列を a2 へコピー(NUL は書かない)
puts:
.l:             move.b  (a0)+,(a2)+
                bne     .l
                subq.l  #1,a2
                rts

; puts_small: a0 の英大文字列を外字(小さな大文字 $F2xx)で a2 へ。空白はそのまま。
puts_small:
.l:             move.b  (a0)+,d0
                beq     .end
                cmp.b   #' ',d0
                beq     .sp
                move.b  #$F2,(a2)+
.sp:            move.b  d0,(a2)+
                bra     .l
.end:           rts

; puts_port: 情報ポート内の文字列(NUL 終端、最大 191 文字)を a2 へ。
;            ポートはワード幅なのでワード単位で読む。
puts_port:
                move.w  #191,d3
.l:             move.w  (a0)+,d0
                move.b  d0,-(sp)        ; 下位バイト
                lsr.w   #8,d0
                tst.b   d0
                beq     .end
                move.b  d0,(a2)+
                move.b  (sp)+,d0
                beq     .rts
                move.b  d0,(a2)+
                subq.w  #2,d3
                bcc     .l
                rts
.end:           addq.l  #2,sp
.rts:           rts

; putdigit: d0 の下位 4 ビットを 1 桁の数字で a2 へ
putdigit:
                and.w   #15,d0
                add.b   #'0',d0
                move.b  d0,(a2)+
                rts

; putbcd: d0.b(BCD 2 桁)を a2 へ
putbcd:
                move.b  d0,d1
                lsr.b   #4,d0
                bsr     putdigit
                move.b  d1,d0
                bra     putdigit

; putdec: d0(0〜65535)を 10 進で a2 へ
putdec:
                and.l   #$FFFF,d0
                moveq   #0,d3           ; 桁数
.split:         divu    #10,d0
                swap    d0
                move.w  d0,-(sp)
                addq.w  #1,d3
                clr.w   d0
                swap    d0
                tst.w   d0
                bne     .split
.out:           move.w  (sp)+,d0
                add.b   #'0',d0
                move.b  d0,(a2)+
                subq.w  #1,d3
                bne     .out
                rts

; putdec1: d0 = 値 x10 を "整数部.小数第 1 位" で a2 へ
putdec1:
                move.l  d0,-(sp)
                divu    #10,d0
                and.l   #$FFFF,d0
                bsr     putdec
                move.b  #'.',(a2)+
                move.l  (sp)+,d0
                divu    #10,d0
                swap    d0
                add.b   #'0',d0
                move.b  d0,(a2)+
                rts

; ---------------------------------------------------------------- 文字列
; コンソールの色: ESC[3Nm の N は 4 で割った余りが色番号(0 黒、1 水色、2 黄、3 白)、N が 4 以上なら太字。
; ESC[m は戻らないので ESC[33m(白・通常)へ戻す。
msg_sharp:      dc.b    27,'[36mSHARP',27,'[33m  ',0
msg_bold:       dc.b    27,'[37m',0
msg_normal:     dc.b    27,'[33m',0
msg_personal:   dc.b    'PERSONAL WORKSTATION',0
msg_x68000:     dc.b    '  ',27,'[35mX68000',27,'[33m  ',0    ; 印字後に shear_x68000 で少し斜めにする(右の空白 1 セルにはみ出す)
X68_COL         equ     29              ; 'SHARP'(5) + 2 + 'PERSONAL WORKSTATION'(20) + 2
IOCS_B_LOCATE   equ     $23
TVRAM           equ     $E00000
msg_series:     dc.b    'SERIES',0
msg_bios:       dc.b    'BIOS ROM version ',0
msg_mpu:        dc.b    'MPU: VMPU68 V',0
msg_clock:      dc.b    'CLOCK: ',0
msg_equiv:      dc.b    'MHz (',0
msg_ratio:      dc.b    'x MC68000 @ 10MHz)',13,10,0
msg_bus:        dc.b    '   Bus: ',0
msg_mhz:        dc.b    'MHz',0
msg_ram:        dc.b    '   RAM: ',0
msg_mb:         dc.b    'MB',0
msg_crlf:       dc.b    13,10,0
msg_none:       dc.b    13,10,'  VMPU68: VMPU68 information port not found ($ECD000)',13,10,0
                even
linebuf:        ds.b    512
devend:

; ---------------------------------------------------------------- SRAM 起動版
                ifd     SRAMMODE
sstart:
                movem.l d0-d7/a0-a6,-(sp)
                bsr     build_text      ; SRAM_BUF に 4 行を作る(基板が無ければ未検出の 1 行)
                moveq   #16,d1          ; 通常起動では IPL は表示を出さないので、ここで 768x512 のテキスト画面にする
                moveq   #IOCS_CRTMOD,d0 ; (Human68k が起動時に設定するのと同じモード。画面も消える)
                trap    #15
                lea     msg_home(pc),a1
                moveq   #IOCS_B_PRINT,d0
                trap    #15
                LINEBUF a1
                moveq   #IOCS_B_PRINT,d0
                trap    #15
                LINEBUF a0
                cmp.b   #27,(a0)        ; ESC で始まる = 基板の情報が出た → 斜体にしてしばらく見せる
                bne     .sdone
                bsr     shear_x68000
                bsr     squash_sharp
                LEAINFO a6
                LINEBUF a2              ; 5 行目: 設定(速度 / ライトバック / JIT)
                bsr     build_settings
                lea     msg_crlf(pc),a0
                bsr     puts
                clr.b   (a2)
                LINEBUF a1
                moveq   #IOCS_B_PRINT,d0
                trap    #15
                bsr     read_us
                move.l  d0,d6
.shold:         bsr     read_us
                sub.l   d6,d0
                cmp.l   #SRAM_HOLD_US,d0
                bcs     .shold
.sdone:         movem.l (sp)+,d0-d7/a0-a6
                rts
msg_home:       dc.b    27,'[1;1H',0    ; 左上へ
                even
                endc

; ---------------------------------------------------------------- .R 版(動作確認用)
                ifd     RMODE
rstart:
                clr.l   -(sp)
                dc.w    $FF20           ; DOS _SUPER: スーパーバイザへ
                addq.l  #4,sp
                move.l  d0,-(sp)
                bsr     build_text
                lea     msg_crlf(pc),a1
                moveq   #IOCS_B_PRINT,d0
                trap    #15
                LINEBUF a1
                moveq   #IOCS_B_PRINT,d0
                trap    #15
                dc.w    $FF20           ; DOS _SUPER: 元のモードへ
                addq.l  #4,sp
                dc.w    $FF00           ; DOS _EXIT
                endc

; ---------------------------------------------------------------- .X 版(VMPU68.X: 情報表示と設定)
;   VMPU68            情報と現在の設定を表示
;   VMPU68 -b         X68030 風の 4 行(SHARP 〜 / BIOS ROM / MPU / CLOCK)だけを表示(AUTOEXEC.BAT 用。
;                     旧 VMPU68.SYS の起動時表示の代わり)
;   VMPU68 -s<MHz>    速度: -s10 / -s16 / -s24 / -s0(全速力)。基板が適用して SD に保存する
;   VMPU68 -w<0|1>    メイン RAM: -w1 ライトバック / -w0 ライトスルー
;   VMPU68 -j<0|1>    JIT(68000 命令の実行時翻訳): -j1 有効 / -j0 無効
; 設定は情報ポートの +$F0(速度)/+$F2(ライトバック)/+$F4(JIT)へ書くと基板側が受け取る。
                ifd     XMODE
xstart:
                suba.l  a4,a4           ; a4 = 1 なら -b(4 行だけ表示)。d7 は build_text が使うので使えない
                movea.l a2,a5           ; コマンドライン(先頭 1 バイトは長さ)
                clr.l   -(sp)
                dc.w    $FF20           ; DOS _SUPER: スーパーバイザへ
                addq.l  #4,sp
                move.l  d0,-(sp)
                ; 基板の有無を確認(build_text が magic を見る)
                bsr     build_text
                LEAINFO a6
                move.l  a6,d0
                beq     .show           ; 基板がなければ表示だけ(未検出メッセージ。再読みするとバスエラー)
                move.l  (a6),d0
                cmp.l   #'VMPU',d0
                bne     .show
                moveq   #0,d6           ; 設定を書いたら 1
                moveq   #0,d1
                move.b  (a5)+,d1        ; 引数の長さ
.parse:         tst.w   d1
                beq     .parsed
                subq.w  #1,d1
                move.b  (a5)+,d0
                cmp.b   #'-',d0
                bne     .parse
                tst.w   d1
                beq     .parsed
                subq.w  #1,d1
                move.b  (a5)+,d0
                or.b    #$20,d0         ; 小文字化
                cmp.b   #'b',d0
                bne     .not_b
                lea     1.w,a4
                bra     .parse
.not_b:         cmp.b   #'s',d0
                beq     .opt_s
                cmp.b   #'j',d0
                beq     .opt_j
                cmp.b   #'i',d0
                beq     .opt_i
                cmp.b   #'m',d0
                beq     .opt_m
                cmp.b   #'n',d0
                beq     .opt_n
                cmp.b   #'t',d0
                beq     .opt_t
                cmp.b   #'w',d0
                bne     .parse
                bsr     getnum
                and.w   #1,d2
                move.w  d2,SET_WB(a6)
                moveq   #1,d6
                bra     .parse
.opt_s:         bsr     getnum
                cmp.w   #1000,d2
                bhi     .parse
                move.w  d2,SET_SPEED(a6)
                moveq   #1,d6
                bra     .parse
.opt_j:         bsr     getnum
                cmp.w   #1,d2
                bhi     .parse
                move.w  d2,SET_JIT(a6)
                moveq   #1,d6
                bra     .parse
.opt_i:         bsr     getnum          ; -i1|-i0: 起動画面(IPL の起動時表示)
                cmp.w   #1,d2
                bhi     .parse
                move.w  d2,SET_SB(a6)
                moveq   #1,d6
                bra     .parse
.opt_m:         bsr     getnum          ; -m<MB>: メイン RAM の固定値、-m0 で自動検出(次の起動から)
                cmp.w   #12,d2
                bhi     .parse
                move.w  d2,SET_RAM(a6)
                moveq   #1,d6
                bra     .parse
.opt_t:         bsr     getnum          ; -t1|-t0: SMI 転送(V2.0 以降の基板。V1.0 では無視される)
                cmp.w   #1,d2
                bhi     .parse
                move.w  d2,SET_SMI(a6)
                moveq   #1,d6
                bra     .parse
.opt_n:         ; -n<name>: 次の " -" までをホスト名にする(先頭の空白は飛ばす、31 バイトまで)。
                ; ワード単位で $C0 へ書く(情報ポートはワード書込みで受ける)
                lea     namebuf(pc),a0
                moveq   #0,d3
.nskip:         tst.w   d1
                beq     .ncopied
                cmp.b   #' ',(a5)
                bne     .ncopy
                addq.l  #1,a5
                subq.w  #1,d1
                bra     .nskip
.ncopy:         tst.w   d1
                beq     .ncopied
                cmp.w   #31,d3
                bcc     .ncopied
                cmp.b   #' ',(a5)       ; " -" が来たら次のオプション(名前に " -" は使えない)
                bne     .nch
                cmp.w   #2,d1
                bcs     .nch
                cmp.b   #'-',1(a5)
                beq     .ncopied
.nch:           move.b  (a5)+,(a0)+
                subq.w  #1,d1
                addq.w  #1,d3
                bra     .ncopy
.ncopied:       cmp.w   #32,d3          ; 残りを 0 で埋める
                bcc     .nfilled
                clr.b   (a0)+
                addq.w  #1,d3
                bra     .ncopied
.nfilled:       lea     namebuf(pc),a0
                lea     NAMEBUF(a6),a1
                moveq   #15,d3
.nw:            move.w  (a0)+,(a1)+
                dbra    d3,.nw
                move.w  #1,SET_NAME(a6)
                moveq   #1,d6
                bra     .parse
.parsed:        tst.w   d6
                beq     .show
                ; 基板が適用して情報ポートを更新するまで約 800 ms 待つ(µs カウンタ: +4 を読むとラッチ、+6 が下位)
                move.l  4(a6),d4
.wait:          move.l  4(a6),d5
                sub.l   d4,d5
                cmp.l   #800000,d5
                bcs     .wait
                bsr     build_text      ; 表示を作り直す
.show:          LINEBUF a1              ; 空行を挟まずに 1 行目から
                moveq   #IOCS_B_PRINT,d0
                trap    #15
                LINEBUF a0
                cmp.b   #27,(a0)
                bne     .nologo
                bsr     shear_x68000
                bsr     squash_sharp
.nologo:        move.l  a4,d0           ; -b: ここまで
                bne     .exit
                ; 設定行
                LEAINFO a6
                move.l  a6,d0
                beq     .exit           ; 基板なし(再読みしない)
                move.l  (a6),d0
                cmp.l   #'VMPU',d0
                bne     .exit
                LINEBUF a2
                bsr     build_settings
                lea     msg_crlf(pc),a0
                bsr     puts
                bsr     build_settings2
                lea     msg_usage(pc),a0
                bsr     puts
                clr.b   (a2)
                LINEBUF a1
                moveq   #IOCS_B_PRINT,d0
                trap    #15
.exit:          dc.w    $FF20           ; DOS _SUPER: 元のモードへ
                addq.l  #4,sp
                dc.w    $FF00           ; DOS _EXIT

; getnum: a5 から 10 進数を読んで d2 へ(d1 = 残り長さを更新)
getnum:         moveq   #0,d2
.g:             tst.w   d1
                beq     .gend
                move.b  (a5),d0
                sub.b   #'0',d0
                bcs     .gend
                cmp.b   #9,d0
                bhi     .gend
                addq.l  #1,a5
                subq.w  #1,d1
                mulu    #10,d2
                and.w   #15,d0
                add.w   d0,d2
                bra     .g
.gend:          rts

msg_usage:      dc.b    13,10,13,10,'usage: VMPU68 [-b] [-s10|-s16|-s24|-s0] [-w1|-w0] [-j1|-j0] [-i1|-i0] [-m<MB>|-m0]',13,10
                dc.b    '              [-n<name>] [-t1|-t0]',13,10
                dc.b    '  -b: banner only   -s: speed   -w: write-back   -j: JIT   -i: boot screen',13,10
                dc.b    '  -m: RAM size (0=auto, next boot)   -n: host name (up to the next " -")   -t: SMI (V2.0+)',13,10,13,10,0
                even
namebuf:        ds.b    32              ; ワード転送するので偶数番地に
infobase:       dc.l    INFO            ; 見つけた情報ポートの番地(build_text が旧番地に切り替えることがある)
                endc
