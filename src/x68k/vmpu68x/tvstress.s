; tvstress.s: テキスト VRAM への書込みで CRTC の意図しない書込み(docs §30/§32)を誘発する試験
;   eld で $1FF000 に置き、epc → erun 1 で走らせる。fktest.s TEST=8 と同じ書込み(R21 の bset/bclr を
;   挟んだバイト書込み)を、画面モードを変えて行う。
;   -DMODE=n  IOCS _CRTMOD の画面モード(既定 -1 = 変えない。3 = 256x256 15 kHz、ドラキュラ相当)
;   -DLINE=n  書き込む先頭行(16 行。既定 100 = 可視行。480 = fktest と同じ行 30)
;   -DFILL=1  最初にテキスト VRAM 4 面全体(行 0〜1023)を疑似乱数で埋める(表示データが 0 だと化けが見えない)
;   -DN=n     繰返し回数(既定 400000 ≈ 6 秒)
;   -DKIND=n  0: fktest TEST=8 と同じ(面 0 の 16 行に R21 bset/bclr 付きバイト書込み)
;             1: ドラキュラの転送ルーチンと同じ形: 面 3 の非表示領域(行 512〜519、列ごとに 32 バイト)から
;                読んで、行 104〜111 の 4 面へ move.b (a1)+,d16(a2..a5)。R21 は $0033 のまま。
;             3: 読み戻し試験: $E70200 に $A5C3 を置き、GVRAM $C9C574 へワード書込み(値は d7)→ 直後に $E70200 を
;                ワード読み、$A5C3 でなければ数える。$1FE004 = 不一致回数、$1FE008 = 総回数、$1FE00C = 最後の不一致値
;                (書込み先は -DWADDR=、読み先は -DRADDR= で変更可)
;             5: ドラキュラの GVRAM 内ワードコピーと同じ形: $C9C776 → $C9C576 を 128 ワード × 64 行(行 1024 バイト)、
;                最初に転送元を疑似乱数で埋める。Pi 側の pw(テキスト VRAM 読み戻し)と組み合わせて使う。
;   終了時に $1FE000 に $1234 を書く。
        ifnd MODE
MODE    equ     -1
        endc
        ifnd LINE
LINE    equ     100
        endc
        ifnd FILL
FILL    equ     1
        endc
        ifnd N
N       equ     400000
        endc
        ifnd KIND
KIND    equ     0
        endc
        ifnd WADDR
WADDR   equ     $C9C574
        endc
        ifnd RADDR
RADDR   equ     $E70200
        endc
TARGET  equ     $E00000+LINE*128
start:
                move.w  #$2700,sr
                move.w  #$0033,$E8002A          ; R21: 同時アクセス OFF、コピー面 0-3、マスク OFF
        if MODE>=0
                moveq   #MODE,d1
                moveq   #$10,d0                 ; IOCS _CRTMOD
                trap    #15
                move.w  #$2700,sr
        endc
        ifd R20
                move.w  #R20,$E80028            ; CRTC レジスタ 20(メモリモード)を直接設定(例: ドラキュラ $0110 = 256 色)
        endc
        ifd VC0
                move.w  #VC0,$E82400            ; ビデオコントローラ R0(色モード: 0=16 色 1=256 色 3=65536 色)
        endc
        ifd VC1
                move.w  #VC1,$E82500            ; ビデオコントローラ R1(優先順位)
        endc
        ifd VC2
                move.w  #VC2,$E82600            ; ビデオコントローラ R2(表示 ON/OFF。ドラキュラ $006F = スプライト+テキスト+グラフィック)
        endc
        ifd IRQON
                move.w  #$2000,sr               ; 割込みを許可したまま走らせる(IOCS の割込み処理 = IACK サイクルが混ざる)
        endc
        ifd ADPCM
                bsr     adpcm_kick              ; ADPCM を鳴らして DMA(ch3)を走らせる(IRQON 前提)
        endc
        if FILL=1
                move.w  #$0033,$E8002A
                lea     $E00000,a0
                move.l  #$9E3779B9,d0           ; 疑似乱数(黄金比の乗算合同)
                move.l  #(512*1024)/4-1,d1
.fill:          move.l  d0,(a0)+
                add.l   #$9E3779B9,d0
                rol.l   #5,d0
                subq.l  #1,d1
                bne     .fill
        endc
                move.l  #N,d7
        if KIND=0
.lp:            bset.b  #0,$E8002A              ; R21 bit8: 同時アクセス ON(IOCS の文字描画と同じ形)
                lea     TARGET,a2
                move.w  #15,d4
.g:             move.b  d7,(a2)
                lea     128(a2),a2
                dbra    d4,.g
                bclr.b  #0,$E8002A
                subq.l  #1,d7
                bne     .lp
        endc
        if KIND=1
.lp:            moveq   #0,d1                   ; 列 0〜31
.col:           lea     $E70000,a1
                move.l  d1,d0
                lsl.l   #5,d0
                adda.l  d0,a1                   ; a1 = $E70000 + 列×32 = 面 3 非表示領域(行 512〜)の帯(32 バイト)
                lea     $E00000+104*128,a2
                adda.l  d1,a2
                lea     $20000(a2),a3
                lea     $20000(a3),a4
                lea     $20000(a4),a5
ROW             set     0
                rept    8
                move.b  (a1)+,ROW(a2)
                move.b  (a1)+,ROW(a3)
                move.b  (a1)+,ROW(a4)
                move.b  (a1)+,ROW(a5)
ROW             set     ROW+128
                endr
                addq.w  #1,d1
                cmp.w   #32,d1
                bne     .col
        ifd ADPCM
                bsr     adpcm_kick              ; 止まっていたら鳴らし直す
        endc
                subq.l  #1,d7
                bne     .lp
        endc
        if KIND=3
                move.w  #$A5C3,RADDR
                moveq   #0,d6
                move.l  #N,d5
.lp3:           move.w  d7,WADDR
                move.w  RADDR,d0
                cmp.w   #$A5C3,d0
                beq     .ok3
                addq.l  #1,d6
                move.w  d0,$1FE00C
.ok3:           addq.w  #1,d7
                subq.l  #1,d5
                bne     .lp3
                move.l  d6,$1FE004
                move.l  #N,$1FE008
        endc
        if KIND=5
                lea     $C9C776,a0              ; 転送元を疑似乱数で埋める(64 行)
                move.l  #$9E3779B9,d0
                move.w  #63,d4
.f5r:           move.w  #127,d3
.f5:            move.w  d0,(a0)+
                add.l   #$9E3779B9,d0
                rol.l   #5,d0
                dbra    d3,.f5
                adda.l  #$400-256,a0
                dbra    d4,.f5r
.lp5:           lea     $C9C776,a1
                lea     $C9C576,a2
                move.w  #63,d4
.r5:            move.w  #127,d3
.w5:            move.w  (a1)+,(a2)+
                dbra    d3,.w5
                adda.l  #$400-256,a1
                adda.l  #$400-256,a2
                dbra    d4,.r5
        ifd ADPCM
                bsr     adpcm_kick              ; 止まっていたら鳴らし直す
        endc
                subq.l  #1,d7
                bne     .lp5
        endc
                move.w  #$0033,$E8002A
                move.w  #$1234,$1FE000          ; 完了印
.end:           bra     .end
        ifd ADPCM
adpcm_kick:     movem.l d0-d2/a1,-(sp)
                moveq   #$68,d0                 ; IOCS _ADPCMSNS: 0 = 停止中
                trap    #15
                tst.l   d0
                bne     .ak_done
                move.w  #$0403,d1               ; 15.6 kHz、左右
                move.l  #32768,d2               ; 32 KB ≈ 4.2 秒
                lea     $180000,a1              ; メイン RAM の空き領域(2 MB 機)
                moveq   #$60,d0                 ; IOCS _ADPCMOUT
                trap    #15
.ak_done:       movem.l (sp)+,d0-d2/a1
                rts
        endc
