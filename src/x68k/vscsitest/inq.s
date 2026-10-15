; INQ.X: IOCS _SCSIDRV S_INQUIRY を ID 2 へ d3=1 と d3=36 で呼び、d0 と受信データの先頭を表示する(フックの確認用)
                section text
start:          lea     buf(pc),a1
                move.l  #2,d4
                moveq   #$F5,d0
                moveq   #$20,d1
                moveq   #1,d3
                move.b  #$EE,(a1)       ; 書き換わったか分かるよう前もって埋める
                trap    #15
                bsr     show
                lea     buf(pc),a1
                move.l  #2,d4
                moveq   #$F5,d0
                moveq   #$20,d1
                moveq   #36,d3
                move.b  #$EE,(a1)
                trap    #15
                bsr     show
                dc.w    $FF00           ; _EXIT
; d0 = 戻り値、(a1) = データ → "d0=xxxxxxxx b0=xx b8=.."
show:           movem.l d0-d7/a0-a6,-(sp)
                move.l  d0,d5
                lea     line(pc),a0
                lea     3(a0),a2
                moveq   #7,d6
.h1:            rol.l   #4,d5
                move.b  d5,d7
                and.b   #15,d7
                move.b  hex(pc,d7.w),(a2)+
                dbra    d6,.h1
                move.b  (a1),d5
                lea     16(a0),a2
                move.b  d5,d7
                lsr.b   #4,d7
                move.b  hex(pc,d7.w),(a2)+
                and.b   #15,d5
                move.b  hex(pc,d5.w),(a2)+
                lea     22(a0),a2
                moveq   #7,d6
.s:             move.b  8(a1),d7
                move.b  d7,(a2)+
                addq.l  #1,a1
                dbra    d6,.s
                move.l  a0,-(sp)
                dc.w    $FF09           ; _PRINT
                addq.l  #4,sp
                movem.l (sp)+,d0-d7/a0-a6
                rts
hex:            dc.b    '0123456789ABCDEF'
line:           dc.b    'd0=........ b0=.. s=........',13,10,0
                even
buf:            ds.b    64
