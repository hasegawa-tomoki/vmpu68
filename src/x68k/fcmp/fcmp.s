; SPDX-License-Identifier: MIT
; FCMP.X - 2 つのファイルをバイト単位で比較する(SCSI/FD の往復照合用。Human68k には比較コマンドが無い)
;   FCMP A B  →  "FCMP: $<size> bytes, bad=$<n> first=$<offset>" (size/first は 32 ビット 16 進、bad も 16 進)
;   32 KB ずつ _READ で読み、長さの違いも化けとして数える。バッファはメモリブロック末尾の 128 KB を借りる。
;   ビルド: build.sh(vasm -Fbin → mkx.py で X 形式)
CHUNK           equ     $8000

V_H1            equ     0
V_H2            equ     2
V_BUF1          equ     4
V_BUF2          equ     8
V_BAD           equ     12
V_FIRST         equ     16
V_TOTAL         equ     20
V_N1            equ     24
V_N2            equ     28
V_NAME1         equ     32
V_NAME2         equ     36
V_A0            equ     40
V_END           equ     44

                bra.w   start           ; mkx.py の X ヘッダは実行開始番地 0 なので先頭は命令でなければならない(文字列を実行すると CHK 例外)
                dc.b    'FCMP 1.0',0
                even
start:
                lea     vars(pc),a5
                move.l  a0,V_A0(a5)
                move.l  8(a0),V_END(a5)
                clr.l   V_BAD(a5)
                move.l  #-1,V_FIRST(a5)
                clr.l   V_TOTAL(a5)
                move.l  8(a0),d0        ; メモリブロック末尾
                sub.l   #CHUNK*2,d0
                and.l   #$FFFFFFF0,d0
                move.l  d0,V_BUF2(a5)
                sub.l   #CHUNK*2,d0
                move.l  d0,V_BUF1(a5)
                ; コマンド行: 長さバイト + 文字列。空白で 2 つの名前に分ける(末尾を 0 で終端)
                moveq   #0,d0
                move.b  (a2)+,d0
                beq     usage
                bsr     skipsp
                move.l  a2,V_NAME1(a5)
                bsr     tokend
                tst.b   d0
                beq     usage
                clr.b   (a2)+           ; 名前 1 の終端
                subq.b  #1,d0
                bsr     skipsp
                tst.b   d0
                beq     usage
                move.l  a2,V_NAME2(a5)
                bsr     tokend
                clr.b   (a2)
                ; open
                move.w  #0,-(sp)
                move.l  V_NAME1(a5),-(sp)
                dc.w    $FF3D           ; _OPEN
                addq.l  #6,sp
                tst.l   d0
                bmi     openerr
                move.w  d0,V_H1(a5)
                move.w  #0,-(sp)
                move.l  V_NAME2(a5),-(sp)
                dc.w    $FF3D
                addq.l  #6,sp
                tst.l   d0
                bmi     openerr
                move.w  d0,V_H2(a5)
                lea     msg_dbg(pc),a0  ; 診断: a0(MCB)・ブロック末尾・バッファ
                bsr     print
                move.l  V_A0(a5),d0
                bsr     puthex
                lea     msg_sp(pc),a0
                bsr     print
                move.l  V_END(a5),d0
                bsr     puthex
                lea     msg_sp(pc),a0
                bsr     print
                move.l  V_BUF1(a5),d0
                bsr     puthex
                lea     msg_sp(pc),a0
                bsr     print
                move.l  V_BUF2(a5),d0
                bsr     puthex
                lea     msg_crlf(pc),a0
                bsr     print
.loop:
                move.l  #CHUNK,-(sp)
                move.l  V_BUF1(a5),-(sp)
                move.w  V_H1(a5),-(sp)
                dc.w    $FF3F           ; _READ
                lea     10(sp),sp
                move.l  d0,V_N1(a5)
                move.l  #CHUNK,-(sp)
                move.l  V_BUF2(a5),-(sp)
                move.w  V_H2(a5),-(sp)
                dc.w    $FF3F
                lea     10(sp),sp
                move.l  d0,V_N2(a5)
                move.l  V_N1(a5),d1
                move.l  V_N2(a5),d2
                bmi     readerr
                tst.l   d1
                bmi     readerr
                cmp.l   d1,d2
                bne     lenerr
                tst.l   d1
                beq     done            ; EOF
                move.l  V_BUF1(a5),a0
                move.l  V_BUF2(a5),a1
                move.l  V_TOTAL(a5),d3  ; 現在のオフセット
                move.l  d1,d4
                subq.l  #1,d4
.cmp:           move.b  (a0)+,d5        ; (cmpm.b は vmpu68 のエミュレータで不当命令になった: 要調査)
                cmp.b   (a1)+,d5
                beq.s   .same
                addq.l  #1,V_BAD(a5)
                tst.l   V_FIRST(a5)
                bpl.s   .same
                move.l  d3,V_FIRST(a5)
.same:          addq.l  #1,d3
                dbra    d4,.cmp
                move.l  d3,V_TOTAL(a5)
                bra     .loop
lenerr:                                 ; 長さが違う: 残りを化けに数えて終了
                move.l  d1,d3
                sub.l   d2,d3
                bpl.s   .p
                neg.l   d3
.p:             add.l   d3,V_BAD(a5)
                tst.l   V_FIRST(a5)
                bpl.s   done
                move.l  V_TOTAL(a5),d3
                cmp.l   d1,d2
                bcc.s   .u
                add.l   d2,d3
                bra.s   .s
.u:             add.l   d1,d3
.s:             move.l  d3,V_FIRST(a5)
done:
                move.w  V_H1(a5),-(sp)
                dc.w    $FF3E           ; _CLOSE
                addq.l  #2,sp
                move.w  V_H2(a5),-(sp)
                dc.w    $FF3E
                addq.l  #2,sp
                lea     msg_res(pc),a0
                bsr     print
                move.l  V_TOTAL(a5),d0
                bsr     puthex
                lea     msg_bad(pc),a0
                bsr     print
                move.l  V_BAD(a5),d0
                bsr     puthex
                lea     msg_first(pc),a0
                bsr     print
                move.l  V_FIRST(a5),d0
                bsr     puthex
                lea     msg_crlf(pc),a0
                bsr     print
                tst.l   V_BAD(a5)
                beq.s   .ok
                lea     msg_ng(pc),a0
                bsr     print
                bra.s   exit
.ok:            lea     msg_ok(pc),a0
                bsr     print
exit:           dc.w    $FF00           ; _EXIT
usage:          lea     msg_usage(pc),a0
                bsr     print
                bra.s   exit
openerr:        lea     msg_open(pc),a0
                bsr     print
                bra.s   exit
readerr:        lea     msg_read(pc),a0
                bsr     print
                bra.s   done

skipsp:                                 ; a2 を空白の先へ(d0 = 残り長)
.l:             tst.b   d0
                beq.s   .e
                cmp.b   #' ',(a2)
                bne.s   .e
                addq.l  #1,a2
                subq.b  #1,d0
                bra.s   .l
.e:             rts
tokend:                                 ; a2 を空白か末尾まで進める(d0 = 残り長)
.l:             tst.b   d0
                beq.s   .e
                cmp.b   #' ',(a2)
                beq.s   .e
                addq.l  #1,a2
                subq.b  #1,d0
                bra.s   .l
.e:             rts

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
msg_usage:      dc.b    'usage: FCMP file1 file2',13,10,0
msg_open:       dc.b    'FCMP: cannot open',13,10,0
msg_read:       dc.b    'FCMP: read error',13,10,0
msg_res:        dc.b    'FCMP: $',0
msg_bad:        dc.b    ' bytes, bad=$',0
msg_first:      dc.b    ' first=$',0
msg_crlf:       dc.b    13,10,0
msg_dbg:        dc.b    'FCMP: mcb/end/buf1/buf2 $',0
msg_sp:         dc.b    ' $',0
msg_ok:         dc.b    'FCMP: identical',13,10,0
msg_ng:         dc.b    'FCMP: DIFFERENT',13,10,0
                even
linebuf:        ds.b    16
vars:           ds.b    64
                end
