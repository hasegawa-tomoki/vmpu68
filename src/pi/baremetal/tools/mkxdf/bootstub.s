| vfd68 boot stub v4 — white window, 45x4 layout.
| Bar geometry per the stock Human68k boot sector (own disassembly of
| HUMAN302.XDF sector 1): column 25, 45 columns, text attribute 15
| (white + emphasis + reverse).  Four rows (12-15): blank, message,
| message, blank.  Boot drive from _BOOTINF, ejected via _B_EJECT, then
| restart through the ROM reset vector so the boot scan continues with
| the next device (SCSI).
| IOCS: $20 _B_PUTC  $21 _B_PRINT  $22 _B_COLOR  $23 _B_LOCATE
|       $4F _B_EJECT  $8E _BOOTINF
| Build: m68k-xelf-as -m68000 -o bootstub.o bootstub.s
|        m68k-xelf-objcopy -O binary bootstub.o bootstub.bin
    .text
    .global _start
_start:
    moveq   #-114,%d0           | $8E _BOOTINF: boot drive info
    trap    #15
    move.w  #0x70,%d7           | 2HD media byte, PDA in the high byte
    lsl.w   #8,%d0
    or.w    %d0,%d7             | d7 = eject parameter for _B_EJECT
    moveq   #15,%d1             | white + emphasis + reverse
    moveq   #0x22,%d0           | _B_COLOR
    trap    #15
    moveq   #12,%d2             | four bar rows, 12..15
3:  bsr     bar
    addq.w  #1,%d2
    cmpi.w  #16,%d2
    bne     3b
    move.w  #31,%d1
    moveq   #13,%d2
    bsr     say
    lea     msg2(%pc),%a1
    move.w  #34,%d1
    moveq   #14,%d2
    bsr     locate
    moveq   #0x21,%d0           | _B_PRINT
    trap    #15
    moveq   #3,%d1              | restore normal text
    moveq   #0x22,%d0
    trap    #15
    move.w  %d7,%d1
    moveq   #0x4f,%d0           | _B_EJECT the boot drive
    trap    #15
    move.w  #119,%d0            | ~2 s pause so the window can be read: 120 frames of
1:  btst    #4,0xe88001         | VDISP (MFP GPIP bit 4) - real time on any CPU, unlike a
    beq     1b                  | delay loop (an accelerator or vmpu68 runs that in ~0.1 s
2:  btst    #4,0xe88001         | and the restart then races the drive's eject)
    bne     2b
    dbf     %d0,1b
    movea.l 0xff0000,%sp        | reset vectors from ROM: machine-independent
    movea.l 0xff0004,%a0
    jmp     (%a0)

say:                            | locate (d1,d2) and print msg1
    bsr     locate
    lea     msg1(%pc),%a1
    moveq   #0x21,%d0           | _B_PRINT
    trap    #15
    rts
locate:
    moveq   #0x23,%d0           | _B_LOCATE
    trap    #15
    rts
bar:                            | 45 reverse-video spaces at column 25
    move.w  %d2,-(%sp)
    moveq   #25,%d1
    bsr     locate
    moveq   #44,%d3
2:  moveq   #32,%d1             | ' '
    moveq   #0x20,%d0           | _B_PUTC
    trap    #15
    dbf     %d3,2b
    move.w  (%sp)+,%d2
    rts
msg1:
    .byte   0x82,0xb1,0x82,0xcc,0x83,0x66,0x83,0x42,0x83,0x58,0x83,0x4e,0x82,0xa9,0x82,0xe7,0x82,0xcd,0x8b,0x4e,0x93,0xae,0x82,0xc5,0x82,0xab,0x82,0xdc,0x82,0xb9,0x82,0xf1,0x00
msg2:
    .byte   0x83,0x43,0x83,0x57,0x83,0x46,0x83,0x4e,0x83,0x67,0x82,0xb5,0x82,0xc4,0x8d,0xc4,0x8b,0x4e,0x93,0xae,0x82,0xb5,0x82,0xdc,0x82,0xb7,0x00
