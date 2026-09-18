; scrtest.s: コンソールに文字を大量に流してスクロールさせ、最後に空行で画面を流す(TVRAM ゴミの試験)
                clr.l   -(sp)
                dc.w    $FF20           ; _SUPER
                addq.l  #4,sp
                move.l  d0,-(sp)
                move.w  #399,d7
.l1:            lea     line(pc),a1
                moveq   #$21,d0
                trap    #15
                dbra    d7,.l1
                move.w  #39,d7
.l2:            lea     crlf(pc),a1
                moveq   #$21,d0
                trap    #15
                dbra    d7,.l2
                dc.w    $FF20
                addq.l  #4,sp
                dc.w    $FF00
line:           dc.b    'WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW 0123456789',13,10,0
crlf:           dc.b    13,10,0
