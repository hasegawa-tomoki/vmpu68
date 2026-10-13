; SPDX-License-Identifier: MIT
; SPEED.X [KB] - 現在のドライブに SPEED.TMP(既定 4096 KB)を 64 KB ずつ書き、読み戻して、速度を表示する
;   Human68k の DOS 呼び出し(_CREATE/_WRITE/_OPEN/_READ)を通した速度 = ドライバと IOCS と装置を全部含む
;   計時は vmpu68 の情報ポート($ECD004 の µs カウンタ、スーパーバイザで読む)
INFO            equ     $ECD000
CHUNK           equ     65536
                bra.w   start
                dc.b    'SPEED 1.0',0
                even
start:          movea.l a2,a5           ; コマンドライン(先頭 1 バイトは長さ)
                move.l  #8192,d6        ; KB(既定)
                moveq   #0,d1
                move.b  (a5)+,d1
                beq     .noarg
.sp:            cmp.b   #' ',(a5)
                bne     .num
                addq.l  #1,a5
                subq.w  #1,d1
                bne     .sp
                bra     .noarg
.num:           moveq   #0,d0
.dig:           move.b  (a5)+,d2
                sub.b   #'0',d2
                bcs     .numend
                cmp.b   #9,d2
                bhi     .numend
                ext.w   d2
                ext.l   d2
                move.l  d0,d3
                lsl.l   #3,d0           ; x8
                add.l   d3,d0           ; x9
                add.l   d3,d0           ; x10
                add.l   d2,d0
                bra     .dig
.numend:        tst.l   d0
                beq     .noarg
                move.l  d0,d6
.noarg:         ; バッファに模様
                lea     buffer(pc),a0
                move.l  #CHUNK/4-1,d0
                moveq   #0,d1
.fill:          move.l  d1,(a0)+
                add.l   #$01010101,d1
                dbra    d0,.fill
                ; 書く
                clr.w   -(sp)
                pea     fname(pc)
                dc.w    $FF3C           ; _CREATE
                addq.l  #6,sp
                tst.l   d0
                bmi     err_create
                move.w  d0,d7           ; ハンドル
                move.l  d6,d5           ; 残り KB
                bsr     us_now
                move.l  d0,d4
.wloop:         move.l  d5,d0
                cmp.l   #CHUNK/1024,d0
                bls     .wlast
                move.l  #CHUNK/1024,d0
.wlast:         move.l  d0,d3
                lsl.l   #8,d3
                lsl.l   #2,d3           ; バイト数 = KB x 1024
                move.l  d3,-(sp)
                pea     buffer(pc)
                move.w  d7,-(sp)
                dc.w    $FF40           ; _WRITE
                lea     10(sp),sp
                cmp.l   d3,d0
                bne     err_write
                sub.l   #CHUNK/1024,d5
                bhi     .wloop
                bsr     us_now
                sub.l   d4,d0
                lea     wr_us(pc),a0
                move.l  d0,(a0)
                move.w  d7,-(sp)
                dc.w    $FF3E           ; _CLOSE
                addq.l  #2,sp
                ; 読む
                clr.w   -(sp)
                pea     fname(pc)
                dc.w    $FF3D           ; _OPEN(読み)
                addq.l  #6,sp
                tst.l   d0
                bmi     err_open
                move.w  d0,d7
                move.l  d6,d5
                bsr     us_now
                move.l  d0,d4
.rloop:         move.l  d5,d0
                cmp.l   #CHUNK/1024,d0
                bls     .rlast
                move.l  #CHUNK/1024,d0
.rlast:         move.l  d0,d3
                lsl.l   #8,d3
                lsl.l   #2,d3
                move.l  d3,-(sp)
                pea     buffer(pc)
                move.w  d7,-(sp)
                dc.w    $FF3F           ; _READ
                lea     10(sp),sp
                cmp.l   d3,d0
                bne     err_read
                sub.l   #CHUNK/1024,d5
                bhi     .rloop
                bsr     us_now
                sub.l   d4,d0
                lea     rd_us(pc),a0
                move.l  d0,(a0)
                move.w  d7,-(sp)
                dc.w    $FF3E           ; _CLOSE
                addq.l  #2,sp
                ; 照合(最後のチャンクの先頭 256 バイト)
                lea     buffer(pc),a0
                moveq   #0,d1
                moveq   #64-1,d0
.chk:           cmp.l   (a0)+,d1
                bne     err_verify
                add.l   #$01010101,d1
                dbra    d0,.chk
                pea     fname(pc)
                dc.w    $FF41           ; _DELETE
                addq.l  #4,sp
                ; 表示
                lea     linebuf(pc),a0
                lea     msg_wr(pc),a1
                bsr     puts
                move.l  d6,d0
                bsr     putdec
                lea     msg_kb(pc),a1
                bsr     puts
                move.l  wr_us(pc),d0
                move.l  d6,d1
                bsr     putrate
                bsr     flush
                lea     linebuf(pc),a0
                lea     msg_rd(pc),a1
                bsr     puts
                move.l  d6,d0
                bsr     putdec
                lea     msg_kb(pc),a1
                bsr     puts
                move.l  rd_us(pc),d0
                move.l  d6,d1
                bsr     putrate
                bsr     flush
exit:           dc.w    $FF00           ; _EXIT
err_create:     lea     msg_ecreate(pc),a1
                bra     err_msg
err_write:      lea     msg_ewrite(pc),a1
                bra     err_msg
err_open:       lea     msg_eopen(pc),a1
                bra     err_msg
err_read:       lea     msg_eread(pc),a1
                bra     err_msg
err_verify:     lea     msg_everify(pc),a1
err_msg:        lea     linebuf(pc),a0
                bsr     puts
                bsr     flush
                bra     exit

; us_now: d0 = IOCS _ONTIME の 1/100 秒(モード切替なしで読める。µs カウンタはスーパーバイザ専用)
us_now:         move.l  d1,-(sp)
                moveq   #$7F,d0
                trap    #15
                move.l  (sp)+,d1
                rts

; putrate: d0 = 1/100 秒、d1 = KB → " in NNN ms (N.NN MB/s)" を a0 へ。MB/s x100 = KB x 10000 / (tick x 1024)
putrate:        movem.l d0-d3/a1,-(sp)
                move.l  d0,d3
                beq     .zero
                lea     msg_in(pc),a1
                bsr     puts
                move.l  d3,d0
                moveq   #10,d1
                bsr     umul32
                bsr     putdec          ; ms
                lea     msg_ms(pc),a1
                bsr     puts
                move.l  4(sp),d0        ; KB
                move.l  #10000,d1
                bsr     umul32          ; KB x 10000(KB <= 400000 まで)
                move.l  d3,d1
                lsl.l   #8,d1
                lsl.l   #2,d1           ; tick x 1024
                bsr     udiv32          ; MB/s x100
                move.l  #100,d1
                bsr     udiv32
                bsr     putdec
                move.b  #'.',(a0)+
                move.l  d1,d0
                cmp.l   #10,d0
                bcc     .two
                move.b  #'0',(a0)+
.two:           bsr     putdec
                lea     msg_mbs(pc),a1
                bsr     puts
                movem.l (sp)+,d0-d3/a1
                rts
.zero:          lea     msg_fast(pc),a1
                bsr     puts
                movem.l (sp)+,d0-d3/a1
                rts
; umul32: d0 = d0 x d1(下位 32 ビット)
umul32:         movem.l d2-d4,-(sp)
                move.l  d0,d2           ; a
                move.l  d1,d3           ; b
                move.l  d2,d4
                mulu    d3,d4           ; a.lo x b.lo
                move.l  d2,d0
                swap    d0
                mulu    d3,d0           ; a.hi x b.lo
                swap    d0
                clr.w   d0
                add.l   d0,d4
                move.l  d3,d0
                swap    d0
                mulu    d2,d0           ; b.hi x a.lo
                swap    d0
                clr.w   d0
                add.l   d0,d4
                move.l  d4,d0
                movem.l (sp)+,d2-d4
                rts
udiv32:         movem.l d2-d3,-(sp)
                moveq   #0,d2
                moveq   #31,d3
.l:             add.l   d0,d0
                addx.l  d2,d2
                cmp.l   d1,d2
                bcs     .n
                sub.l   d1,d2
                addq.l  #1,d0
.n:             dbra    d3,.l
                move.l  d2,d1
                movem.l (sp)+,d2-d3
                rts
puts:           move.b  (a1)+,(a0)+
                bne     puts
                subq.l  #1,a0
                rts
putdec:         movem.l d0-d2/a1,-(sp)
                lea     numbuf+11(pc),a1
                clr.b   -(a1)
.n:             moveq   #10,d1
                bsr     udiv32
                add.b   #'0',d1
                move.b  d1,-(a1)
                tst.l   d0
                bne     .n
.c:             move.b  (a1)+,(a0)+
                bne     .c
                subq.l  #1,a0
                movem.l (sp)+,d0-d2/a1
                rts
flush:          move.b  #13,(a0)+
                move.b  #10,(a0)+
                clr.b   (a0)
                pea     linebuf(pc)
                dc.w    $FF09           ; _PRINT
                addq.l  #4,sp
                rts
fname:          dc.b    'SPEED.TMP',0
msg_wr:         dc.b    'write ',0
msg_rd:         dc.b    'read  ',0
msg_kb:         dc.b    ' KB',0
msg_in:         dc.b    ' in ',0
msg_ms:         dc.b    ' ms (',0
msg_mbs:        dc.b    ' MB/s)',0
msg_fast:       dc.b    ' in < 10 ms',0
msg_ecreate:    dc.b    'SPEED: cannot create SPEED.TMP',0
msg_ewrite:     dc.b    'SPEED: write failed',0
msg_eopen:      dc.b    'SPEED: cannot open SPEED.TMP',0
msg_eread:      dc.b    'SPEED: read failed',0
msg_everify:    dc.b    'SPEED: verify failed',0
                even
wr_us:          dc.l    0
rd_us:          dc.l    0
numbuf:         dcb.b   12,0
linebuf:        dcb.b   128,0
                even
buffer:         dcb.b   CHUNK,0
