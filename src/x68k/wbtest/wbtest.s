; SPDX-License-Identifier: MIT
; WBTEST.X - メイン RAM のライトバック(シャドウ RAM のダーティなデータ)と、PSRAM 割当範囲(2 MB 以上)への
;   DMA 読み書き(SCSI のファイル書込み・読込み)の整合試験。
;   自分の確保ブロックの末尾近く(2 MB 以上になる)に 128 KB のバッファを 2 つ取り、CPU でパターンを書き、
;   B:\VMPUTEST\WBn.DAT へ書き出し(DMAC がメモリを読む)、もう一方のバッファへ読み戻し(DMAC がメモリを書く)、
;   期待パターンと照合する。4 回(即・即・4 秒待ち・即)、送り元と先を交互に入れ替える。
;   3 回目の 4 秒待ちは、前の回で 3 秒間ライトスルーになったページの期限を過ぎさせ、DMAC 起動時の書き戻しだけで
;   整合するかを見るため。ファイルは Mac 側で中身を照合するため残す(試験後に DEL で消す)。
;   ビルド: build.sh(vasm -Fbin → mkx.py で X 形式)

BUFSZ           equ     $20000          ; 128 KB
WORDS           equ     BUFSZ/2
ROUNDS          equ     4
WAIT_CS         equ     400             ; 4 秒(1/100 秒単位)

; 変数(a5 からのオフセット)
V_TOP           equ     0
V_BUF1          equ     4
V_BUF2          equ     8
V_SRC           equ     12
V_DST           equ     16
V_FAILS         equ     20
V_MISS          equ     24
V_FIRST         equ     28
V_WR            equ     32
V_RD            equ     36
V_SIZE          equ     40

                bra.w   start
                dc.b    'WBTEST 1.0',0
                even
start:
                lea     vars(pc),a5
                move.l  a1,d0           ; 確保ブロックの末尾(Human68k が渡す)
                sub.l   #$10000,d0      ; スタック用に 64 KB 空ける
                and.l   #$FFFF0000,d0
                move.l  d0,V_TOP(a5)
                sub.l   #BUFSZ,d0
                move.l  d0,V_BUF2(a5)
                sub.l   #BUFSZ,d0
                move.l  d0,V_BUF1(a5)
                clr.l   V_FAILS(a5)

                lea     msg_hdr(pc),a0
                bsr     print
                move.l  V_BUF1(a5),d0
                bsr     puthex
                lea     msg_hdr2(pc),a0
                bsr     print
                move.l  V_BUF2(a5),d0
                bsr     puthex
                lea     msg_crlf(pc),a0
                bsr     print
                cmp.l   #$200000,V_BUF1(a5)
                bcc.s   .high
                lea     msg_low(pc),a0
                bsr     print
.high:
                pea     dirname(pc)
                dc.w    $FF39           ; _MKDIR(既にあればエラー、無視)
                addq.l  #4,sp

                moveq   #1,d7           ; 回
round_loop:
                ; 送り元と先(奇数回: buf1 → buf2、偶数回: buf2 → buf1)
                move.l  V_BUF1(a5),d0
                move.l  V_BUF2(a5),d1
                btst    #0,d7
                bne.s   .odd
                exg     d0,d1
.odd:           move.l  d0,V_SRC(a5)
                move.l  d1,V_DST(a5)
                ; 3 回目はパターンを書く前に 4 秒待つ(ライトスルー期限を過ぎさせる)
                cmp.w   #3,d7
                bne.s   .nowait
                bsr     wait4s
.nowait:
                ; 送り元に期待パターン、先に別パターン(読み戻しが行われなかったことも検出できる)
                move.l  V_SRC(a5),a0
                move.w  d7,d6
                bsr     fill
                move.l  V_DST(a5),a0
                move.w  d7,d6
                add.w   #$80,d6
                bsr     fill
                ; ファイル名の数字
                lea     fname(pc),a0
                move.b  d7,d0
                add.b   #'0',d0
                move.b  d0,fname_digit-fname(a0)
                ; 書き出し
                clr.w   -(sp)           ; 属性 0
                pea     fname(pc)
                dc.w    $FF3C           ; _CREATE
                addq.l  #6,sp
                tst.l   d0
                bmi     err_create
                move.w  d0,d5           ; ハンドル
                move.l  #BUFSZ,-(sp)
                move.l  V_SRC(a5),-(sp)
                move.w  d5,-(sp)
                dc.w    $FF40           ; _WRITE
                lea     10(sp),sp
                move.l  d0,V_WR(a5)
                move.w  d5,-(sp)
                dc.w    $FF3E           ; _CLOSE
                addq.l  #2,sp
                ; 読み戻し
                clr.w   -(sp)           ; 読み出しモード
                pea     fname(pc)
                dc.w    $FF3D           ; _OPEN
                addq.l  #6,sp
                tst.l   d0
                bmi     err_open
                move.w  d0,d5
                move.l  #BUFSZ,-(sp)
                move.l  V_DST(a5),-(sp)
                move.w  d5,-(sp)
                dc.w    $FF3F           ; _READ
                lea     10(sp),sp
                move.l  d0,V_RD(a5)
                move.w  d5,-(sp)
                dc.w    $FF3E           ; _CLOSE
                addq.l  #2,sp
                ; 照合(先のバッファを期待パターンと比べる)
                move.l  V_DST(a5),a0
                move.w  d7,d6
                bsr     compare
                ; 結果表示
                bsr     report
                tst.l   V_MISS(a5)
                bne.s   .bad
                cmp.l   #BUFSZ,V_WR(a5)
                bne.s   .bad
                cmp.l   #BUFSZ,V_RD(a5)
                beq.s   .next
.bad:           addq.l  #1,V_FAILS(a5)
.next:
                addq.w  #1,d7
                cmp.w   #ROUNDS,d7
                bls     round_loop

                tst.l   V_FAILS(a5)
                bne.s   .ng
                lea     msg_ok(pc),a0
                bra.s   .fin
.ng:            lea     msg_ng(pc),a0
.fin:           bsr     print
                dc.w    $FF00           ; _EXIT

err_create:     lea     msg_ecreate(pc),a0
                bra.s   err_fin
err_open:       lea     msg_eopen(pc),a0
err_fin:        bsr     print
                dc.w    $FF00

; ---- パターン: ワード i = i*$9E37 + 種*$1111 + $2468(下位 16 ビット)
; a0 = バッファ、d6 = 種
fill:
                movem.l d0-d2/a0,-(sp)
                move.w  d6,d2
                mulu    #$1111,d2
                add.w   #$2468,d2
                moveq   #0,d1
                move.l  #WORDS-1,d0
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

; a0 = バッファ、d6 = 種 → V_MISS(不一致ワード数)、V_FIRST(最初の不一致のオフセット、無ければ -1)
compare:
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
                move.l  d1,d3
                add.l   d3,d3
                move.l  d3,V_FIRST(a5)
.ok:            addq.w  #1,d1
                cmp.l   #WORDS,d1
                bcs.s   .l
                movem.l (sp)+,d0-d3/a0
                rts

; "round n: src=XXXXXXXX dst=XXXXXXXX write=N read=N mismatch=N first=+XXXXXX"
report:
                lea     msg_round(pc),a0
                bsr     print
                moveq   #0,d0
                move.w  d7,d0
                bsr     putdec
                lea     msg_src(pc),a0
                bsr     print
                move.l  V_SRC(a5),d0
                bsr     puthex
                lea     msg_dst(pc),a0
                bsr     print
                move.l  V_DST(a5),d0
                bsr     puthex
                lea     msg_wr(pc),a0
                bsr     print
                move.l  V_WR(a5),d0
                bsr     putdec
                lea     msg_rd(pc),a0
                bsr     print
                move.l  V_RD(a5),d0
                bsr     putdec
                lea     msg_miss(pc),a0
                bsr     print
                move.l  V_MISS(a5),d0
                bsr     putdec
                tst.l   V_FIRST(a5)
                bmi.s   .nofirst
                lea     msg_first(pc),a0
                bsr     print
                move.l  V_FIRST(a5),d0
                bsr     puthex
.nofirst:       lea     msg_crlf(pc),a0
                bsr     print
                rts

; 4 秒待つ(IOCS _ONTIME、1/100 秒)
wait4s:
                movem.l d0-d2,-(sp)
                moveq   #$7F,d0
                trap    #15
                move.l  d0,d2
.l:             moveq   #$7F,d0
                trap    #15
                sub.l   d2,d0
                cmp.l   #WAIT_CS,d0
                bcs.s   .l
                movem.l (sp)+,d0-d2
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
msg_hdr:        dc.b    'WBTEST: buf1=$',0
msg_hdr2:       dc.b    ' buf2=$',0
msg_low:        dc.b    '(buf1 < $200000: PSRAM range not used)',13,10,0
msg_round:      dc.b    'round ',0
msg_src:        dc.b    ': src=$',0
msg_dst:        dc.b    ' dst=$',0
msg_wr:         dc.b    ' write=',0
msg_rd:         dc.b    ' read=',0
msg_miss:       dc.b    ' mismatch=',0
msg_first:      dc.b    ' first=+$',0
msg_crlf:       dc.b    13,10,0
msg_ok:         dc.b    'WBTEST: OK (files left in B:\VMPUTEST for host-side check)',13,10,0
msg_ng:         dc.b    'WBTEST: NG',13,10,0
msg_ecreate:    dc.b    'WBTEST: _CREATE failed',13,10,0
msg_eopen:      dc.b    'WBTEST: _OPEN failed',13,10,0
dirname:        dc.b    'B:\VMPUTEST',0
fname:          dc.b    'B:\VMPUTEST\WB'
fname_digit:    dc.b    '0.DAT',0
                even
linebuf:        ds.b    32
vars:           ds.b    V_SIZE
