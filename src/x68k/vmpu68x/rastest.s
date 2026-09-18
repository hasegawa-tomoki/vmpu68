; rastest.s: CRTC ラスタコピーの試験(eld で $1FF000 に置いて走らせる)
;   転送元ラスタ SRC..SRC+N-1(面 0: 行番号の下位バイト、面 1: その反転)を転送先 DST.. へ IOCS と同じ手順でコピーする。
;   -DDELAY=n: HSYNC 検出後の dbra 待ち回数、-DEDGE=1: HSYNC の立ち下がり→立ち上がりを待ってから書く
SRC     equ     200
DST     equ     160
N       equ     32
        ifnd DELAY
DELAY   equ     0
        endc
        ifnd EDGE
EDGE    equ     0
        endc
start:
                move.w  #$2700,sr
                lea     $E00000+SRC*4*128,a0
                lea     $E20000+SRC*4*128,a1
                move.w  #SRC*4,d1
                move.w  #N*4-1,d2       ; 行数
.fr:            move.b  d1,d4
                lsl.w   #8,d4
                move.b  d1,d4           ; ワード = 行番号 x2
                move.w  d4,d5
                not.w   d5
                move.w  #63,d3
.fw:            move.w  d4,(a0)+
                move.w  d5,(a1)+
                dbra    d3,.fw
                addq.w  #1,d1
                dbra    d2,.fr
                lea     $E00000+DST*4*128,a0
                lea     $E20000+DST*4*128,a1
                move.w  #N*4*64-1,d3
.cl:            clr.w   (a0)+
                clr.w   (a1)+
                dbra    d3,.cl
                move.w  #$0003,$E8002A  ; R21: 面 0,1 をコピー
                move.w  #(SRC<<8)|DST,d1
                move.w  #N-1,d0
.lp:
        if EDGE
.wl:            btst.b  #7,$E88001
                bne     .wl             ; HSYNC が下がるまで
        endc
.wh:            btst.b  #7,$E88001
                beq     .wh             ; HSYNC が上がるまで
        if DELAY
                move.w  #DELAY,d6
.dl:            dbra    d6,.dl
        endc
                move.w  d1,$E8002C      ; R22 = 転送元<<8 | 転送先
                move.w  #8,$E80480      ; ラスタコピー開始
                add.w   #$0101,d1
                dbra    d0,.lp
.w2:            btst.b  #7,$E88001
                beq     .w2
                clr.w   $E80480
                move.w  #$1234,$1FE000  ; 完了印
.end:           bra     .end
