; SPDX-License-Identifier: MIT
; WRTEST.X - 実 RAM への書込みが化けていないかを X68000 側から数える検査(Pi コンソールの vfy に相当)
;   エミュレータの CPU 書込みはシャドウ RAM 止まり(ライトバック)なので、実 RAM を通す経路として DMAC(ch2)の
;   メモリ間転送を使う。手順(1 回あたり 32768 ワード = 64 KB):
;     1. 送り元バッファに期待パターンを書く(シャドウ)。先のバッファは CPU で触らない(ダーティにすると、
;        バースト DMA のスヌープ取りこぼしを実 RAM から補修する処理が飛んで偽の化けを数えてしまう)。
;     2. DMAC ch2 を起動 → 基板が送り元のダーティなワードを実 RAM に書き戻す(= 検査対象の Pi からの書込み)
;        → DMAC が実 RAM から読んで先へ書く → 基板がスヌープで先のシャドウを更新。
;     3. 先のバッファを期待パターンと照合。不一致 = 書込み(または DMA/スヌープ)で化けたワード。
;   8 回繰り返し、回ごとの化け数と最初の不一致番地、合計を表示する。
;   ビルド: build.sh(vasm -Fbin → mkx.py で X 形式)

BUFSZ           equ     $10000          ; 64 KB
WORDS           equ     BUFSZ/2
ROUNDS          equ     8
DMAC2           equ     $E84080         ; HD63450 channel 2
POLL_MAX        equ     2000000

V_SRC           equ     0
V_DST           equ     4
V_MISS          equ     8
V_FIRST         equ     12
V_TOTAL         equ     16
V_SSP           equ     20
V_CSR           equ     24
V_CER           equ     26
V_GOT           equ     28
V_WANT          equ     30
V_CS            equ     32
V_A0            equ     36
V_A1            equ     40
V_SP            equ     44
V_MBLK          equ     48
V_SIZE          equ     52

                bra.w   start
                dc.b    'WRTEST 1.0',0
                even
start:
                lea     vars(pc),a5
                move.l  a0,V_A0(a5)
                move.l  a1,V_A1(a5)
                move.l  sp,V_SP(a5)
                clr.l   V_CS(a5)
                moveq   #0,d0
                move.b  (a2)+,d0        ; コマンドライン長
.arg:           tst.b   d0
                beq.s   .noarg
                move.b  (a2)+,d1
                subq.b  #1,d0
                cmp.b   #'c',d1
                beq.s   .cs
                cmp.b   #'C',d1
                bne.s   .arg
.cs:            move.l  #1,V_CS(a5)     ; 'c': サイクルスチール転送
.noarg:
                ; バッファは _MALLOC で確保する(Human68k はこのプログラムに自分のサイズ分しか割り当てない。
                ; ブロック末尾から下へ取ると COMMAND.X など他の領域を DMA で壊す)
                ; X 形式のプログラムは空きメモリを全部持つ(_MALLOC は失敗する)。a1 はプログラム本体の末尾で、
                ; ブロックの末尾はメモリ管理ブロック(a0 の +8)にある。その 64 KB 下から 128 KB を使う。
                move.l  8(a0),d0
                move.l  d0,V_MBLK(a5)   ; (表示用) ブロック末尾
                sub.l   #$10000,d0
                and.l   #$FFFFFFF0,d0
                sub.l   #BUFSZ,d0
                move.l  d0,V_DST(a5)
                sub.l   #BUFSZ,d0
                move.l  d0,V_SRC(a5)
                cmp.l   a1,d0           ; プログラム本体より下なら使えない
                bls     err_malloc
                clr.l   V_TOTAL(a5)
                clr.l   -(sp)
                dc.w    $FF20           ; _SUPER(DMAC のレジスタはスーパーバイザで触る)
                addq.l  #4,sp
                move.l  d0,V_SSP(a5)

                lea     msg_hdr(pc),a0
                bsr     print
                move.l  a0,d0           ; (表示用) メモリ管理ポインタ a0 は上で壊れているので保存値を使う
                move.l  V_A0(a5),d0
                bsr     puthex
                lea     msg_end(pc),a0
                bsr     print
                move.l  V_A1(a5),d0
                bsr     puthex
                lea     msg_sp(pc),a0
                bsr     print
                move.l  V_SP(a5),d0
                bsr     puthex
                lea     msg_top(pc),a0
                bsr     print
                move.l  V_MBLK(a5),d0
                bsr     puthex
                lea     msg_hdr1(pc),a0
                bsr     print
                move.l  V_SRC(a5),d0
                bsr     puthex
                lea     msg_hdr2(pc),a0
                bsr     print
                move.l  V_DST(a5),d0
                bsr     puthex
                lea     msg_hdr3(pc),a0
                bsr     print

                moveq   #1,d7
round_loop:
                move.l  V_SRC(a5),a0
                move.w  d7,d6
                bsr     fill            ; 先(dst)は CPU で触らない: ダーティにするとスヌープ取りこぼしの補修が飛び、偽の化けになる
                bsr     dma_copy
                move.l  V_DST(a5),a0
                move.w  d7,d6
                bsr     compare
                move.l  V_MISS(a5),d0
                add.l   d0,V_TOTAL(a5)
                bsr     report
                addq.w  #1,d7
                cmp.w   #ROUNDS,d7
                bls     round_loop

                lea     DMAC2,a0
                move.b  #$FF,0(a0)      ; CSR を消しておく
                lea     msg_total(pc),a0
                bsr     print
                move.l  V_TOTAL(a5),d0
                bsr     putdec
                lea     msg_total2(pc),a0
                bsr     print
                move.l  V_SSP(a5),-(sp)
                dc.w    $FF20           ; _SUPER 解除
                addq.l  #4,sp
                dc.w    $FF00           ; _EXIT
err_malloc:     lea     msg_emalloc(pc),a0
                bsr     print
                dc.w    $FF00

; ---- DMAC ch2: V_SRC → V_DST、WORDS ワード、バースト、メモリ間(dmat と同じ設定)
dma_copy:
                movem.l d0-d1/a0,-(sp)
                lea     DMAC2,a0
                move.b  #$FF,0(a0)      ; CSR: clear
                move.b  #$08,4(a0)      ; DCR: burst, 68000 dual-address, 16-bit port
                move.b  #$11,5(a0)      ; OCR: memory -> device, word, auto-request max rate
                tst.l   V_CS(a5)
                beq.s   .burst
                move.b  #$88,4(a0)      ; DCR: cycle steal
                move.b  #$10,5(a0)      ; OCR: auto-request limited rate
.burst:
                move.b  #$05,6(a0)      ; SCR: MAR++, DAR++
                move.w  #WORDS,$A(a0)   ; MTC
                move.l  V_SRC(a5),$C(a0)        ; MAR
                move.l  V_DST(a5),$14(a0)       ; DAR
                move.b  #$05,$29(a0)    ; MFC
                move.b  #$05,$31(a0)    ; DFC
                move.b  #$03,$2D(a0)    ; BFC
                move.b  #$80,7(a0)      ; CCR: STR
                move.l  #POLL_MAX,d1
.poll:          move.b  0(a0),d0
                btst    #7,d0           ; COC
                bne.s   .done
                subq.l  #1,d1
                bne.s   .poll
.done:          move.b  0(a0),V_CSR(a5)
                move.b  1(a0),V_CER(a5)
                movem.l (sp)+,d0-d1/a0
                rts

; ---- パターン: ワード i = i*$9E37 + 種*$1111 + $2468(下位 16 ビット)
fill:                                   ; a0 = バッファ、d6 = 種
                movem.l d0-d2/a0,-(sp)
                moveq   #0,d1
.l:             move.w  d1,d2
                mulu    #$9E37,d2
                move.w  d6,d0
                mulu    #$1111,d0
                add.w   #$2468,d0
                add.w   d0,d2
                move.w  d2,(a0)+
                addq.w  #1,d1
                cmp.l   #WORDS,d1
                bcs.s   .l
                movem.l (sp)+,d0-d2/a0
                rts

compare:                                ; a0 = バッファ、d6 = 種 → V_MISS、V_FIRST
                movem.l d0-d3/a0,-(sp)
                clr.l   V_MISS(a5)
                move.l  #-1,V_FIRST(a5)
                moveq   #0,d1
.l:             move.w  d1,d2
                mulu    #$9E37,d2
                move.w  d6,d0
                mulu    #$1111,d0
                add.w   #$2468,d0
                add.w   d0,d2
                cmp.w   (a0)+,d2
                beq.s   .ok
                addq.l  #1,V_MISS(a5)
                tst.l   V_FIRST(a5)
                bpl.s   .ok
                move.l  a0,d3
                subq.l  #2,d3
                move.l  d3,V_FIRST(a5)
                move.w  -2(a0),V_GOT(a5)
                move.w  d2,V_WANT(a5)
.ok:            addq.w  #1,d1
                cmp.l   #WORDS,d1
                bcs.s   .l
                movem.l (sp)+,d0-d3/a0
                rts

report:
                lea     msg_round(pc),a0
                bsr     print
                moveq   #0,d0
                move.w  d7,d0
                bsr     putdec
                lea     msg_bad(pc),a0
                bsr     print
                move.l  V_MISS(a5),d0
                bsr     putdec
                tst.l   V_FIRST(a5)
                bmi.s   .nofirst
                lea     msg_first(pc),a0
                bsr     print
                move.l  V_FIRST(a5),d0
                bsr     puthex
                lea     msg_got(pc),a0
                bsr     print
                moveq   #0,d0
                move.w  V_GOT(a5),d0
                bsr     puthex4
                lea     msg_want(pc),a0
                bsr     print
                moveq   #0,d0
                move.w  V_WANT(a5),d0
                bsr     puthex4
.nofirst:       lea     msg_csr(pc),a0
                bsr     print
                moveq   #0,d0
                move.b  V_CSR(a5),d0
                bsr     puthex2
                lea     msg_cer(pc),a0
                bsr     print
                moveq   #0,d0
                move.b  V_CER(a5),d0
                bsr     puthex2
                lea     msg_crlf(pc),a0
                bsr     print
                rts

; ---- 表示
print:                                  ; a0 = 文字列
                move.l  a0,-(sp)
                dc.w    $FF09           ; _PRINT
                addq.l  #4,sp
                rts

puthex:                                 ; d0 = 32 ビット値を 8 桁の 16 進で
                movem.l d0-d2/a0-a1,-(sp)
                lea     linebuf(pc),a1
                moveq   #7,d2
.l:             rol.l   #4,d0
                move.w  d0,d1
                and.w   #15,d1
                move.b  hexdig(pc,d1.w),(a1)+
                dbra    d2,.l
                clr.b   (a1)
                lea     linebuf(pc),a0
                bsr     print
                movem.l (sp)+,d0-d2/a0-a1
                rts
puthex4:                                ; d0 = 16 ビット値を 4 桁の 16 進で
                movem.l d0-d2/a0-a1,-(sp)
                lea     linebuf(pc),a1
                swap    d0
                moveq   #3,d2
.l:             rol.l   #4,d0
                move.w  d0,d1
                and.w   #15,d1
                move.b  hexdig(pc,d1.w),(a1)+
                dbra    d2,.l
                clr.b   (a1)
                lea     linebuf(pc),a0
                bsr     print
                movem.l (sp)+,d0-d2/a0-a1
                rts
puthex2:                                ; d0 = 8 ビット値を 2 桁の 16 進で
                movem.l d0-d2/a0-a1,-(sp)
                lea     linebuf(pc),a1
                rol.l   #8,d0
                rol.l   #8,d0
                rol.l   #8,d0           ; 下位バイトを上位へ
                moveq   #1,d2
.l:             rol.l   #4,d0
                move.w  d0,d1
                and.w   #15,d1
                move.b  hexdig(pc,d1.w),(a1)+
                dbra    d2,.l
                clr.b   (a1)
                lea     linebuf(pc),a0
                bsr     print
                movem.l (sp)+,d0-d2/a0-a1
                rts
hexdig:         dc.b    '0123456789ABCDEF'
                even

putdec:                                 ; d0 = 符号なし 32 ビット値を 10 進で(32 ビット ÷ 10 を divu 2 段で)
                movem.l d0-d3/a0-a1,-(sp)
                lea     linebuf+15(pc),a1
                clr.b   (a1)
.l:             move.l  d0,d1
                clr.w   d1
                swap    d1              ; d1 = 上位ワード
                divu    #10,d1          ; d1.w = 上位の商、上位 16 ビット = 余り
                move.w  d1,d3           ; d3 = 上位の商
                clr.w   d1
                swap    d1
                swap    d1              ; d1 = 余り << 16
                move.w  d0,d1           ; d1 = 余り:下位ワード
                divu    #10,d1          ; d1.w = 下位の商、上位 16 ビット = 桁
                move.w  d1,d2           ; d2 = 下位の商
                swap    d1
                add.b   #'0',d1
                move.b  d1,-(a1)
                move.w  d3,d0
                swap    d0
                move.w  d2,d0           ; d0 = 商(32 ビット)
                tst.l   d0
                bne.s   .l
                move.l  a1,a0
                bsr     print
                movem.l (sp)+,d0-d3/a0-a1
                rts

; ---- データ
msg_hdr:        dc.b    'WRTEST: Pi -> RAM write check via DMAC ch2 (64 KB x 8)',13,10
                dc.b    '  block=$',0
msg_end:        dc.b    ' end=$',0
msg_sp:         dc.b    ' sp=$',0
msg_top:        dc.b    ' top=$',0
msg_hdr1:       dc.b    13,10,'  src=$',0
msg_hdr2:       dc.b    ' dst=$',0
msg_hdr3:       dc.b    13,10,0
msg_round:      dc.b    'round ',0
msg_bad:        dc.b    ': bad=',0
msg_first:      dc.b    ' first=$',0
msg_got:        dc.b    ' got=',0
msg_want:       dc.b    ' want=',0
msg_csr:        dc.b    ' CSR=',0
msg_cer:        dc.b    ' CER=',0
msg_crlf:       dc.b    13,10,0
msg_total:      dc.b    'total bad=',0
msg_total2:     dc.b    ' / 262144 words (0 = OK)',13,10,0
msg_emalloc:    dc.b    'WRTEST: not enough memory (need 128 KB + 64 KB)',13,10,0
                even
linebuf:        ds.b    32
vars:           ds.b    V_SIZE
