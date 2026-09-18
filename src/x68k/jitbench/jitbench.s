; jitbench.s: JIT の効果測定用ベンチマーク(Human68k、X 形式、PC 相対のみ)
;   4 つのカーネルを走らせ、情報ポート($ECFF04/06)の µs カウンタで計時して表示する。
;   結果は $ECFF80〜 にも書く(コンソール info の書込みログで読める)。
                clr.l   -(sp)
                dc.w    $FF20           ; _SUPER (I/O 領域のため)
                addq.l  #4,sp
                move.l  d0,-(sp)

                lea     title(pc),a1
                bsr     print

; ---- K1: ALU ループ(add/sub/eor/lsl/addq/cmp/bcc) ----
                bsr     tick
                move.l  d0,d6
                moveq   #0,d0
                move.l  #$12345678,d1
                move.l  #$0F0F0F0F,d2
                move.l  #$5A5A5A5A,d3
                moveq   #0,d4
                move.l  #2000000,d5
.k1:            add.l   d1,d0
                sub.l   d2,d0
                eor.l   d3,d0
                lsl.l   #1,d0
                addq.l  #1,d4
                cmp.l   d5,d4
                blt.s   .k1
                move.l  d0,-(sp)
                bsr     tick
                sub.l   d6,d0
                lea     r_alu(pc),a0
                move.l  d0,(a0)
                addq.l  #4,sp
                lea     s_alu(pc),a1
                move.l  d0,d1
                bsr     print_us

; ---- K2: メモリ転送(64 KB を 50 回、move.l (a0)+,(a1)+ ×4 / dbra) ----
                bsr     tick
                move.l  d0,d6
                move.w  #49,d7
.k2o:           lea     buf(pc),a0
                lea     buf(pc),a1
                adda.l  #65536,a1
                move.w  #4095,d5
.k2i:           move.l  (a0)+,(a1)+
                move.l  (a0)+,(a1)+
                move.l  (a0)+,(a1)+
                move.l  (a0)+,(a1)+
                dbra    d5,.k2i
                dbra    d7,.k2o
                bsr     tick
                sub.l   d6,d0
                lea     r_mem(pc),a0
                move.l  d0,(a0)
                lea     s_mem(pc),a1
                move.l  d0,d1
                bsr     print_us

; ---- K3: サブルーチン呼出し(bsr/rts + 引数の受け渡し) ----
                bsr     tick
                move.l  d0,d6
                move.l  #1000000,d7
                moveq   #0,d0
.k3:            move.l  d7,d1
                bsr     sub3
                add.l   d1,d0
                subq.l  #1,d7
                bne.s   .k3
                bsr     tick
                sub.l   d6,d0
                lea     r_call(pc),a0
                move.l  d0,(a0)
                lea     s_call(pc),a1
                move.l  d0,d1
                bsr     print_us

; ---- K4: 混合(lea/movem/moveq/ext/swap/btst/and/or、スタック使用) ----
                bsr     tick
                move.l  d0,d6
                move.l  #400000,d7
                lea     buf(pc),a2
.k4:            movem.l d0-d3/a0-a1,-(sp)
                move.l  d7,d3
                andi.l  #$FFFF,d3
                lea     8(a2,d3.l),a0
                moveq   #-1,d0
                ext.l   d1
                swap    d0
                btst    #3,d7
                beq.s   .k4a
                and.l   d0,d1
                bra.s   .k4b
.k4a:           or.l    d7,d1
.k4b:           move.w  d1,(a0)
                move.w  (a0),d2
                movem.l (sp)+,d0-d3/a0-a1
                subq.l  #1,d7
                bne.s   .k4
                bsr     tick
                sub.l   d6,d0
                lea     r_mix(pc),a0
                move.l  d0,(a0)
                lea     s_mix(pc),a1
                move.l  d0,d1
                bsr     print_us

; ---- 結果を情報ポートへ(ms 単位、16 ビット) ----
                lea     $ECFF80,a0
                move.l  r_alu(pc),d0
                bsr     to_ms
                move.w  d0,(a0)+
                move.l  r_mem(pc),d0
                bsr     to_ms
                move.w  d0,(a0)+
                move.l  r_call(pc),d0
                bsr     to_ms
                move.w  d0,(a0)+
                move.l  r_mix(pc),d0
                bsr     to_ms
                move.w  d0,(a0)+

                dc.w    $FF20           ; _SUPER 戻し
                addq.l  #4,sp
                dc.w    $FF00           ; _EXIT

sub3:           add.l   d1,d1
                addq.l  #3,d1
                rts

; µs カウンタ読み: d0 = 32 ビット µs
tick:           move.w  $ECFF04,d0
                swap    d0
                move.w  $ECFF06,d0
                rts

; d0 = µs → d0 = ms(切り捨て、16 ビット)
to_ms:          divu    #1000,d0
                andi.l  #$FFFF,d0
                rts

; print: a1 = 文字列(DOS _PRINT)
print:          movem.l d0-d1/a0-a1,-(sp)
                moveq   #$21,d0
                trap    #15
                movem.l (sp)+,d0-d1/a0-a1
                rts

; print_us: a1 = 見出し文字列、d1 = µs 値。"<見出し> <数字> us\r\n"
print_us:       movem.l d0-d3/a0-a2,-(sp)
                bsr     print
                lea     numbuf(pc),a0
                move.l  d1,d0
                lea     pow10(pc),a2
                moveq   #0,d3           ; 先頭ゼロ抑止フラグ
.pu0:           move.l  (a2)+,d2
                beq.s   .pu3
                moveq   #'0',d1
.pu1:           cmp.l   d2,d0
                blo.s   .pu2
                sub.l   d2,d0
                addq.b  #1,d1
                bra.s   .pu1
.pu2:           cmp.b   #'0',d1
                bne.s   .pu2a
                tst.l   d3
                beq.s   .pu0            ; 先頭ゼロは出さない
.pu2a:          move.b  d1,(a0)+
                moveq   #1,d3
                bra.s   .pu0
.pu3:           tst.l   d3
                bne.s   .pu4
                move.b  #'0',(a0)+
.pu4:           clr.b   (a0)
                lea     numbuf(pc),a1
                bsr     print
                lea     s_us(pc),a1
                bsr     print
                movem.l (sp)+,d0-d3/a0-a2
                rts

title:          dc.b    'VMPU68 JIT bench',13,10,0
s_alu:          dc.b    'ALU  (2M loops): ',0
s_mem:          dc.b    'MEM  (64KBx50) : ',0
s_call:         dc.b    'CALL (1M bsr)  : ',0
s_mix:          dc.b    'MIX  (400K)    : ',0
s_us:           dc.b    ' us',13,10,0
                even
pow10:          dc.l    1000000000,100000000,10000000,1000000,100000,10000,1000,100,10,1,0
                even
r_alu:          dc.l    0
r_mem:          dc.l    0
r_call:         dc.l    0
r_mix:          dc.l    0
numbuf:         ds.b    12
                even
buf:            ds.b    131072
