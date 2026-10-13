; SPDX-License-Identifier: MIT
; VSCSITST.X - vmpu68 の仮想 SCSI コマンドポート($ECD100、docs/design/vscsi-plan.md 段階 1)の試験と速度計測
;   ポートを探し、INQUIRY / READ CAPACITY / 無い ID の応答を確かめ、1 ブロック読みの往復時間、
;   64 KB 読み・書きの速度(µs カウンタは情報ポート $ECD004)と読み戻しの照合を表示する。
;   ビルド: build.sh(vasm -Fbin → mkx.py で X 形式)
;   実行: Human68k から VSCSITST(基板側の RAM 試験ディスク ID 0 を相手にする)

VSPORT          equ     $ECD100
INFO            equ     $ECD000
BLK             equ     512
XFER_BLKS       equ     128             ; 64 KB
XFER_BYTES      equ     XFER_BLKS*BLK
ROUNDS          equ     64              ; 64 x 64 KB = 4 MB
LAT_N           equ     100
WR_LBA          equ     4096
TIMEOUT_US      equ     3000000

                bra.w   start
                dc.b    'VSCSITST 1.0',0
                even
start:
                clr.l   -(sp)
                dc.w    $FF20           ; DOS _SUPER
                addq.l  #4,sp
                move.l  d0,-(sp)        ; 元の SSP(戻すとき)
                ; ポート探し: バスエラーなら notfound へ
                move.l  $8.w,d7
                lea     berr(pc),a0
                move.l  a0,$8.w
                lea     VSPORT,a6
                move.l  $20(a6),d0
                move.l  d7,$8.w
                cmp.l   #'VSCS',d0
                bne     notfound
                ; バッファ(64 KB、プログラム内。X 形式のプログラムは空きメモリを全部持っているので _MALLOC は使えない)
                lea     buffer(pc),a5
                ; 見出し
                lea     linebuf(pc),a0
                lea     msg_hdr(pc),a1
                bsr     puts
                moveq   #0,d0
                move.w  $24(a6),d0
                bsr     putdec
                lea     msg_hdr2(pc),a1
                bsr     puts
                moveq   #0,d0
                move.w  $26(a6),d0
                bsr     putdec
                lea     msg_kb(pc),a1
                bsr     puts
                bsr     flush
                ; INQUIRY(36 バイト)
                lea     cdb_inq(pc),a0
                moveq   #0,d1           ; ID 0
                moveq   #1,d2           ; データイン
                move.l  a5,a1
                moveq   #36,d3
                bsr     docmd
                lea     linebuf(pc),a0
                lea     msg_inq(pc),a1
                bsr     puts
                tst.l   d0
                bne     .inq_ng
                lea     8(a5),a1
                moveq   #28-1,d1
.inq_c:         move.b  (a1)+,(a0)+
                dbra    d1,.inq_c
                bra     .inq_done
.inq_ng:        bsr     puterr
.inq_done:      bsr     flush
                ; READ CAPACITY
                lea     cdb_cap(pc),a0
                moveq   #0,d1
                moveq   #1,d2
                move.l  a5,a1
                moveq   #8,d3
                bsr     docmd
                lea     linebuf(pc),a0
                lea     msg_cap(pc),a1
                bsr     puts
                tst.l   d0
                bne     .cap_ng
                move.l  (a5),d0
                addq.l  #1,d0
                bsr     putdec
                lea     msg_blocks(pc),a1
                bsr     puts
                move.l  4(a5),d0
                bsr     putdec
                bra     .cap_done
.cap_ng:        bsr     puterr
.cap_done:      bsr     flush
                ; 無い ID(1): 応答なし(-1)のはず
                lea     cdb_tur(pc),a0
                moveq   #1,d1
                moveq   #0,d2
                move.l  a5,a1
                moveq   #0,d3
                bsr     docmd
                lea     linebuf(pc),a0
                lea     msg_id1(pc),a1
                bsr     puts
                move.w  $1A(a6),d0
                ext.l   d0
                bsr     putsdec
                lea     msg_id1b(pc),a1
                bsr     puts
                bsr     flush
                ; DMA で読み込まれた直後のページは 3 秒間ライトスルー扱い(実バスへ 1 ワードずつ)なので、
                ; 計測前に 3.5 秒待つ(docs/design/writeback-plan.md、WB_WT_HOLD_MS)
                lea     linebuf(pc),a0
                lea     msg_wait(pc),a1
                bsr     puts
                bsr     flush
                bsr     us_now
                move.l  d0,d6
.settle:        bsr     us_now
                sub.l   d6,d0
                cmp.l   #3500000,d0
                bcs     .settle
                ; 1 ブロック読み x LAT_N: 往復時間
                bsr     us_now
                move.l  d0,d6
                move.w  #LAT_N-1,d5
.lat:           lea     cdb_rd1(pc),a0
                moveq   #0,d1
                moveq   #1,d2
                move.l  a5,a1
                move.l  #BLK,d3
                bsr     docmd
                tst.l   d0
                bne     .lat_ng
                dbra    d5,.lat
                bsr     us_now
                sub.l   d6,d0
                move.l  #LAT_N,d1
                bsr     udiv32
                lea     linebuf(pc),a0
                lea     msg_lat(pc),a1
                bsr     puts
                bsr     putdec
                lea     msg_us(pc),a1
                bsr     puts
                bsr     flush
                bra     .lat_done
.lat_ng:        lea     linebuf(pc),a0
                lea     msg_lat(pc),a1
                bsr     puts
                bsr     puterr
                bsr     flush
.lat_done:
                ; 64 KB 読み x ROUNDS、全ロング照合
                lea     xfer_us(pc),a0
                clr.l   (a0)
                moveq   #0,d4           ; 照合エラー数
                bsr     us_now
                move.l  d0,d6
                move.w  #ROUNDS-1,d5
                moveq   #0,d7           ; LBA
.rd:            lea     cdb_rd(pc),a0
                move.l  d7,2(a0)        ; LBA(ビッグエンディアン、+2..+5)
                moveq   #0,d1
                moveq   #1,d2
                move.l  a5,a1
                move.l  #XFER_BYTES,d3
                bsr     docmd
                tst.l   d0
                bne     .rd_ng
                ; 照合: ブロック n のロング j = (n<<8)|j
                move.l  a5,a1
                move.l  d7,d0
                lsl.l   #8,d0           ; (LBA<<8)
                move.w  #XFER_BLKS-1,d1
.vb:            moveq   #0,d2
.vl:            move.l  d0,d3
                or.l    d2,d3
                cmp.l   (a1)+,d3
                beq     .vok
                addq.l  #1,d4
.vok:           addq.l  #1,d2
                cmp.l   #BLK/4,d2
                bne     .vl
                add.l   #$100,d0
                dbra    d1,.vb
                add.l   #XFER_BLKS,d7
                cmp.l   #8192,d7
                bcs     .rd_next
                moveq   #0,d7
.rd_next:       dbra    d5,.rd
                bsr     us_now
                sub.l   d6,d0
                move.l  d0,d6           ; d6 = µs
                lea     linebuf(pc),a0
                lea     msg_rd(pc),a1
                bsr     puts
                move.l  d6,d0
                bsr     putrate
                lea     msg_xfer(pc),a1
                bsr     puts
                move.l  xfer_us(pc),d0
                bsr     putrate
                lea     msg_verify(pc),a1
                bsr     puts
                move.l  d4,d0
                bsr     putdec
                lea     msg_errs(pc),a1
                bsr     puts
                bsr     flush
                bra     .rd_done
.rd_ng:         lea     linebuf(pc),a0
                lea     msg_rd(pc),a1
                bsr     puts
                bsr     puterr
                bsr     flush
.rd_done:
                ; 64 KB 書き x ROUNDS(LBA WR_LBA から、模様 ~((n<<8)|j))
                lea     xfer_us(pc),a0
                clr.l   (a0)
                bsr     us_now
                move.l  d0,d6
                move.w  #ROUNDS-1,d5
                move.l  #WR_LBA,d7
.wr:            ; バッファに模様
                move.l  a5,a1
                move.l  d7,d0
                lsl.l   #8,d0
                move.w  #XFER_BLKS-1,d1
.fb:            moveq   #0,d2
.fl:            move.l  d0,d3
                or.l    d2,d3
                not.l   d3
                move.l  d3,(a1)+
                addq.l  #1,d2
                cmp.l   #BLK/4,d2
                bne     .fl
                add.l   #$100,d0
                dbra    d1,.fb
                lea     cdb_wr(pc),a0
                move.l  d7,2(a0)
                moveq   #0,d1
                moveq   #2,d2           ; データアウト
                move.l  a5,a1
                move.l  #XFER_BYTES,d3
                bsr     docmd
                tst.l   d0
                bne     .wr_ng
                add.l   #XFER_BLKS,d7
                cmp.l   #8192,d7
                bcs     .wr_next
                move.l  #WR_LBA,d7
.wr_next:       dbra    d5,.wr
                bsr     us_now
                sub.l   d6,d0
                move.l  d0,d6
                lea     linebuf(pc),a0
                lea     msg_wr(pc),a1
                bsr     puts
                move.l  d6,d0
                bsr     putrate
                lea     msg_xfer(pc),a1
                bsr     puts
                move.l  xfer_us(pc),d0
                bsr     putrate
                bsr     flush
                ; 書いた範囲を読み戻して照合(ROUNDS x 64 KB は 4 MB = ディスク全体なので WR_LBA 以降 + 先頭に折り返した分)
                moveq   #0,d4
                move.w  #ROUNDS-1,d5
                move.l  #WR_LBA,d7
.rb:            lea     cdb_rd(pc),a0
                move.l  d7,2(a0)
                moveq   #0,d1
                moveq   #1,d2
                move.l  a5,a1
                move.l  #XFER_BYTES,d3
                bsr     docmd
                tst.l   d0
                bne     .rb_ng
                move.l  a5,a1
                move.l  d7,d0
                lsl.l   #8,d0
                move.w  #XFER_BLKS-1,d1
.rvb:           moveq   #0,d2
.rvl:           move.l  d0,d3
                or.l    d2,d3
                not.l   d3
                cmp.l   (a1)+,d3
                beq     .rvok
                addq.l  #1,d4
.rvok:          addq.l  #1,d2
                cmp.l   #BLK/4,d2
                bne     .rvl
                add.l   #$100,d0
                dbra    d1,.rvb
                add.l   #XFER_BLKS,d7
                cmp.l   #8192,d7
                bcs     .rb_next
                move.l  #WR_LBA,d7
.rb_next:       dbra    d5,.rb
                lea     linebuf(pc),a0
                lea     msg_rb(pc),a1
                bsr     puts
                move.l  d4,d0
                bsr     putdec
                lea     msg_errs(pc),a1
                bsr     puts
                bsr     flush
                ; 書いた範囲を元の模様 (n<<8)|j に戻す(Pi 側の RAM ディスクは X68000 のリセットでは消えないので、
                ; 次回の読み試験の照合が通るように)
                move.w  #ROUNDS-1,d5
                move.l  #WR_LBA,d7
.rs:            move.l  a5,a1
                move.l  d7,d0
                lsl.l   #8,d0
                move.w  #XFER_BLKS-1,d1
.rsb:           moveq   #0,d2
.rsl:           move.l  d0,d3
                or.l    d2,d3
                move.l  d3,(a1)+
                addq.l  #1,d2
                cmp.l   #BLK/4,d2
                bne     .rsl
                add.l   #$100,d0
                dbra    d1,.rsb
                lea     cdb_wr(pc),a0
                move.l  d7,2(a0)
                moveq   #0,d1
                moveq   #2,d2
                move.l  a5,a1
                move.l  #XFER_BYTES,d3
                bsr     docmd
                add.l   #XFER_BLKS,d7
                cmp.l   #8192,d7
                bcs     .rs_next
                move.l  #WR_LBA,d7
.rs_next:       dbra    d5,.rs
                lea     linebuf(pc),a0
                lea     msg_restored(pc),a1
                bsr     puts
                bsr     flush
                bra     exit
.wr_ng:         lea     linebuf(pc),a0
                lea     msg_wr(pc),a1
                bsr     puts
                bsr     puterr
                bsr     flush
                bra     exit
.rb_ng:         lea     linebuf(pc),a0
                lea     msg_rb(pc),a1
                bsr     puts
                bsr     puterr
                bsr     flush
exit:
                dc.w    $FF20           ; DOS _SUPER: 元のモードへ(SSP は積んである)
                addq.l  #4,sp
                dc.w    $FF00           ; DOS _EXIT
notfound:       lea     msg_none(pc),a1
exit_msg:       lea     linebuf(pc),a0
                bsr     puts
                bsr     flush
                bra     exit

; バスエラー(68000 の 14 バイトフレーム): 拡張部を捨てて notfound へ
berr:           addq.l  #8,sp
                lea     notfound(pc),a0
                move.l  a0,2(sp)
                move.l  d7,$8.w
                rte

; ---------------------------------------------------------------- コマンド発行
; docmd: a0 = CDB(12 バイト)、d1 = ID、d2 = 方向(0/1 イン/2 アウト)、a1 = バッファ、d3 = 長さ
;        戻り d0 = 0 正常、それ以外 = +$18 の状態語(エラー)または -1(時間切れ)。a6 = ポート
docmd:          movem.l d1-d6/a0-a1,-(sp)
                move.w  (a0)+,0(a6)
                move.w  (a0)+,2(a6)
                move.w  (a0)+,4(a6)
                move.w  (a0)+,6(a6)
                move.w  (a0)+,8(a6)
                move.w  (a0)+,10(a6)
                move.b  d1,$C(a6)
                move.b  d2,$D(a6)
                move.l  a1,$10(a6)
                move.l  d3,$14(a6)
                bsr     us_now
                move.l  d0,d5
                move.w  #1,$18(a6)      ; 開始
.poll:          move.w  $18(a6),d4
                btst    #1,d4
                bne     .done
                bsr     us_now
                sub.l   d5,d0
                cmp.l   #TIMEOUT_US,d0
                bcs     .poll
                moveq   #-1,d0
                bra     .ret
.done:          bsr     us_now          ; 転送に掛かった時間(開始の書込みから完了まで)を積算
                sub.l   d5,d0
                lea     xfer_us(pc),a0
                add.l   d0,(a0)
                moveq   #0,d0
                move.w  d4,d0
                and.w   #$FF04,d0       ; 状態バイトかエラービットが立っていれば異常
                beq     .ret
                move.w  d4,d0
.ret:           movem.l (sp)+,d1-d6/a0-a1
                rts

; us_now: d0 = 情報ポートの µs カウンタ(+4 を読むとラッチ、+6 が下位)
us_now:         move.w  INFO+4,d0
                swap    d0
                move.w  INFO+6,d0
                rts

; ---------------------------------------------------------------- 表示
; puts: a1 の NUL 終端文字列を a0 へ
puts:           move.b  (a1)+,(a0)+
                bne     puts
                subq.l  #1,a0
                rts
; puterr: d0(docmd の戻り)を " NG (status=xxxx result=n)" の形で a0 へ
puterr:         movem.l d0-d1/a1,-(sp)
                lea     msg_ng(pc),a1
                bsr     puts
                move.l  d0,d1
                bsr     puthex4
                lea     msg_ng2(pc),a1
                bsr     puts
                move.w  $1A(a6),d0
                ext.l   d0
                bsr     putsdec
                move.b  #')',(a0)+
                movem.l (sp)+,d0-d1/a1
                rts
; puthex4: d1 の下位 16 ビットを 4 桁 16 進で a0 へ
puthex4:        movem.l d0-d2,-(sp)
                moveq   #3,d2
                rol.w   #4,d1
.h:             move.w  d1,d0
                and.w   #15,d0
                add.b   #'0',d0
                cmp.b   #'9',d0
                bls     .d
                addq.b  #7,d0
.d:             move.b  d0,(a0)+
                rol.w   #4,d1
                dbra    d2,.h
                movem.l (sp)+,d0-d2
                rts
; putsdec: 符号付き d0 を 10 進で a0 へ
putsdec:        tst.l   d0
                bpl     putdec
                move.b  #'-',(a0)+
                neg.l   d0
; putdec: 符号無し d0 を 10 進で a0 へ
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
; putrate: d0 = µs で ROUNDS x XFER_BYTES(4 MB)を転送した結果を "NNN ms, N.NN MB/s" の形で a0 へ
;          MB/s x100 = (4194304 x 100) / µs = 419430400 / µs(32 ビットに収まる。MB は 10^6)
putrate:        movem.l d0-d3/a1,-(sp)
                move.l  d0,d3           ; µs
                move.l  #1000,d1
                bsr     udiv32
                bsr     putdec          ; ms
                lea     msg_ms(pc),a1
                bsr     puts
                move.l  #ROUNDS*XFER_BYTES/16*100,d0
                lsl.l   #4,d0           ; 4194304 x 100(即値を 16 で割って lsl: 定数式の桁あふれ回避)
                move.l  d3,d1
                bsr     udiv32          ; d0 = MB/s x100
                move.l  #100,d1
                bsr     udiv32          ; d0 = 整数部、d1 = 小数 2 桁
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
; udiv32: d0 = d0 / d1、d1 = 余り(符号無し 32/32)
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
; flush: linebuf を改行付きで表示(a0 = 終端位置)
flush:          move.b  #13,(a0)+
                move.b  #10,(a0)+
                clr.b   (a0)
                pea     linebuf(pc)
                dc.w    $FF09           ; DOS _PRINT
                addq.l  #4,sp
                rts

; ---------------------------------------------------------------- データ
cdb_inq:        dc.b    $12,0,0,0,36,0, 0,0,0,0,0,0
cdb_cap:        dc.b    $25,0,0,0,0,0,0,0,0,0, 0,0
cdb_tur:        dc.b    0,0,0,0,0,0, 0,0,0,0,0,0
cdb_rd1:        dc.b    $28,0,0,0,0,0,0,0,1,0, 0,0
cdb_rd:         dc.b    $28,0,0,0,0,0,0,0,XFER_BLKS,0, 0,0
cdb_wr:         dc.b    $2A,0,0,0,0,0,0,0,XFER_BLKS,0, 0,0
                even
msg_hdr:        dc.b    'VSCSITST: virtual SCSI port v',0
msg_hdr2:       dc.b    ', max ',0
msg_kb:         dc.b    ' KB per command',0
msg_inq:        dc.b    'INQUIRY: ',0
msg_cap:        dc.b    'READ CAPACITY: ',0
msg_blocks:     dc.b    ' blocks x ',0
msg_id1:        dc.b    'ID 1 (absent): result ',0
msg_id1b:       dc.b    ' (expect -1)',0
msg_wait:       dc.b    '(waiting 3.5 s for the write-through hold to expire)',0
msg_lat:        dc.b    '1-block read x 100: ',0
msg_us:         dc.b    ' us per command',0
msg_rd:         dc.b    '64 KB read x 64 (4 MB): ',0
msg_wr:         dc.b    '64 KB write x 64 (4 MB): ',0
msg_rb:         dc.b    'read back after write: ',0
msg_restored:   dc.b    '(test pattern restored)',0
msg_verify:     dc.b    ', verify ',0
msg_xfer:       dc.b    '; transfer only: ',0
msg_errs:       dc.b    ' errors',0
msg_ms:         dc.b    ' ms, ',0
msg_mbs:        dc.b    ' MB/s',0
msg_ng:         dc.b    'NG (status=',0
msg_ng2:        dc.b    ' result=',0
msg_none:       dc.b    'VSCSITST: virtual SCSI port not found ($ECD100)',0
                even
xfer_us:        dc.l    0               ; docmd の中で過ごした µs の積算
numbuf:         dcb.b   12,0            ; (-Fbin は末尾の ds.b を出力しないので初期化済みにする)
linebuf:        dcb.b   256,0
                even
buffer:         dcb.b   XFER_BYTES,0
