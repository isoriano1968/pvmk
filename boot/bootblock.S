/*
 * pvmk boot block — shared by the floppy (ADF) and RDB partition boot.
 *
 * Kickstart loads these 1024 bytes, verifies the checksum, and calls
 * offset 12 with A6 = SysBase and A1 = IOStdReq of the boot device
 * (trackdisk.device for DF0:). The code is position independent.
 *
 * Steps: init serial, allocate memory, read the pvmk image described by
 * the PVBB area with one CMD_READ, verify its CRC-32, flush caches, and
 * jump to it with:
 *	A0 = image base   A1 = IOStdReq   A6 = SysBase
 *	D0 = boot medium  D1 = serial baud  D2 = first image block on the device
 * If the image returns D0 = 0, AmigaOS boot continues (dos.library init).
 * BOOTBLOCK_DECLINE: free the image and return an error quietly, so strap
 * goes on with the next boot device.
 *
 * Serial progress characters: B (entry) L (loaded) C (CRC ok) J (jump);
 * errors print "E" followed by a code letter and hold a colour for about
 * three seconds (to be seen without a serial terminal):
 *	Em bad PVBB magic       magenta   Ea AllocMem failed   yellow
 *	Er read error           orange    Ec CRC mismatch      red
 *	Ed dos.library missing  white
 * and then raise the recoverable alert 5056 LLEE: LL the code letter
 * (6d 61 72 63 64), EE trackdisk's io_Error (Er), before strap's own
 * AN_BootError (3000 0001).
 */
#include "pvmk/amiga.h"
#include "pvmk/image.h"

	.text
	.globl	_bb_start
_bb_start:
	.ascii	"DOS"
	.byte	1			/* FFS */
	.long	0			/* checksum, set by mkadf */
	.long	880			/* root block */

bb_entry:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	%a1,%a5			/* IOStdReq */
	lea	pvbb(%pc),%a4
	sub.l	%a3,%a3			/* no image allocated yet */

	move.w	#0x00f,COLOR00		/* blue: boot block running */
	move.l	PVBB_O_BAUD(%a4),%d0
	bsr	ser_init
	moveq	#'B',%d0
	bsr	ser_putc

	moveq	#'m',%d7
	cmp.l	#PVBB_MAGIC,PVBB_O_MAGIC(%a4)
	bne	fail

	/* image = AllocMem(alloc, MEMF_PUBLIC) */
	moveq	#'a',%d7
	move.l	PVBB_O_ALLOC(%a4),%d0
	moveq	#MEMF_PUBLIC,%d1
	jsr	LVO_AllocMem(%a6)
	tst.l	%d0
	beq	fail
	move.l	%d0,%a3

	/*
	 * Read nblocks from start, one track (11 blocks) per CMD_READ, up to
	 * three tries each: one long request fails as a whole on one bad try.
	 */
	moveq	#'r',%d7
	move.l	PVBB_O_START(%a4),%d3	/* block */
	move.l	PVBB_O_NBLOCKS(%a4),%d4	/* blocks left */
	move.l	%a3,%d5			/* destination */
rd_track:
	move.l	%d3,%d0			/* to the end of this track */
	divu.w	#11,%d0
	swap	%d0
	moveq	#11,%d2
	sub.w	%d0,%d2
	cmp.l	%d4,%d2
	bls.s	1f
	move.l	%d4,%d2
1:	moveq	#3,%d6			/* tries */
rd_try:
	move.l	%a5,%a1
	move.w	#CMD_READ,IO_COMMAND(%a1)
	move.l	%d5,IO_DATA(%a1)
	moveq	#9,%d1
	move.l	%d2,%d0
	lsl.l	%d1,%d0
	move.l	%d0,IO_LENGTH(%a1)
	move.l	%d3,%d0
	lsl.l	%d1,%d0
	move.l	%d0,IO_OFFSET(%a1)
	jsr	LVO_DoIO(%a6)
	tst.b	%d0
	beq.s	2f
	subq.w	#1,%d6
	bne.s	rd_try
	bra	fail
2:	add.l	%d2,%d3
	moveq	#9,%d1
	move.l	%d2,%d0
	lsl.l	%d1,%d0
	add.l	%d0,%d5
	sub.l	%d2,%d4
	bne.s	rd_track
	move.w	#0x0ff,COLOR00		/* cyan: image loaded */
	moveq	#'L',%d0
	bsr	ser_putc

	moveq	#'c',%d7
	move.l	%a3,%a0
	move.l	PVBB_O_SIZE(%a4),%d0
	bsr	crc32
	cmp.l	PVBB_O_CRC(%a4),%d0
	bne	fail
	moveq	#'C',%d0
	bsr	ser_putc

	/* 040/060 copyback caches: push data, invalidate instructions */
	cmp.w	#37,EB_VERSION(%a6)
	blo.s	1f
	jsr	LVO_CacheClearU(%a6)
1:
	moveq	#'J',%d0
	bsr	ser_putc
	moveq	#'\r',%d0
	bsr	ser_putc
	moveq	#'\n',%d0
	bsr	ser_putc

	move.l	%a3,%a0
	move.l	%a5,%a1
	moveq	#0,%d0
	move.w	PVBB_O_MEDIUM(%a4),%d0
	move.l	PVBB_O_BAUD(%a4),%d1
	move.l	PVBB_O_START(%a4),%d2	/* image's first block on the device */
	move.l	%a3,%a2
	add.l	PVBB_O_ENTRY(%a4),%a2
	jsr	(%a2)
	cmp.l	#BOOTBLOCK_DECLINE,%d0
	beq.s	decline
	tst.l	%d0
	bne.w	out			/* image reported failure */

	/* Image returned 0: continue booting AmigaOS from this disk. */
	moveq	#'d',%d7
	lea	dosname(%pc),%a1
	jsr	LVO_FindResident(%a6)
	tst.l	%d0
	beq.w	fail
	move.l	%d0,%a0
	move.l	RT_INIT(%a0),%a0
	moveq	#0,%d0
	bra.w	out

decline:
	move.l	%a3,%a1
	move.l	PVBB_O_ALLOC(%a4),%d0
	jsr	LVO_FreeMem(%a6)
	moveq	#-1,%d0
	bra.w	out

fail:
	moveq	#'E',%d0
	bsr	ser_putc
	move.l	%d7,%d0
	bsr	ser_putc
	moveq	#' ',%d0		/* io_Error, buffer, io_Actual */
	bsr	ser_putc
	move.b	IO_ERROR(%a5),%d3
	moveq	#1,%d2
	bsr	ser_hex
	move.l	%a3,%d3
	moveq	#7,%d2
	bsr	ser_hex
	move.l	IO_ACTUAL(%a5),%d3
	moveq	#7,%d2
	bsr	ser_hex
	moveq	#'\r',%d0
	bsr	ser_putc
	moveq	#'\n',%d0
	bsr	ser_putc
	lea	colours(%pc),%a0
1:	move.w	(%a0)+,%d0		/* code letter, colour */
	move.w	(%a0)+,%d1
	cmp.b	%d0,%d7
	bne.s	1b
	move.w	#300,%d2		/* line 16 and 272: 2 per frame */
2:	move.w	%d1,COLOR00		/* again and again: the Copper */
	cmp.b	#0x10,VHPOSR		/* may set it every frame */
	bne.s	2b
3:	move.w	%d1,COLOR00
	cmp.b	#0x10,VHPOSR
	beq.s	3b
	dbf	%d2,2b
	move.l	%a3,%d0
	beq.s	1f
	move.l	%a3,%a1
	move.l	PVBB_O_ALLOC(%a4),%d0
	jsr	LVO_FreeMem(%a6)
1:	lsl.w	#8,%d7			/* our own recoverable alert, to */
	move.b	IO_ERROR(%a5),%d7	/* be read off the screen: 5056 */
	and.l	#0xffff,%d7		/* code letter, io_Error */
	or.l	#0x50560000,%d7
	jsr	LVO_Alert(%a6)
	moveq	#-1,%d0
out:
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

/*
 * ser_init: d0 = baud. SERPER = colour clock / baud - 1, where the colour
 * clock is 5 * E clock (PAL 3546895 Hz, NTSC 3579545 Hz).
 */
ser_init:
	move.l	%d0,%d1
	move.l	EB_ECLOCKFREQ(%a6),%d0
	mulu.l	#5,%d0
	move.l	%d1,%d2
	lsr.l	#1,%d2
	add.l	%d2,%d0			/* round to nearest */
	divu.l	%d1,%d0
	subq.l	#1,%d0
	move.w	%d0,SERPER
	rts

/* ser_hex: d3 = value, d2 = digits - 1; a space before; clobbers d0-d3 */
ser_hex:
	moveq	#' ',%d0
	bsr.s	ser_putc
	move.l	%d2,%d1
	addq.l	#1,%d1
	lsl.l	#2,%d1
	ror.l	%d1,%d3			/* top digit first */
1:	rol.l	#4,%d3
	moveq	#15,%d0
	and.l	%d3,%d0
	add.b	#'0',%d0
	cmp.b	#'9',%d0
	bls.s	2f
	addq.b	#7,%d0
2:	move.l	%d2,-(%sp)
	bsr.s	ser_putc
	move.l	(%sp)+,%d2
	dbf	%d2,1b
	rts

/* ser_putc: d0 = character; polled, clobbers d0/d1 */
ser_putc:
1:	move.w	SERDATR,%d1
	btst	#SERDATR_TBE,%d1
	beq.s	1b
	and.w	#0xff,%d0
	or.w	#0x100,%d0		/* one stop bit */
	move.w	%d0,SERDAT
	rts

/* crc32: a0 = buffer, d0 = length -> d0 = CRC-32 (zlib); clobbers d1-d4 */
crc32:
	move.l	%d0,%d2
	moveq	#-1,%d0
	move.l	#0xedb88320,%d3
	bra.s	3f
1:	moveq	#0,%d1
	move.b	(%a0)+,%d1
	eor.l	%d1,%d0
	moveq	#7,%d4
2:	lsr.l	#1,%d0
	bcc.s	4f
	eor.l	%d3,%d0
4:	dbf	%d4,2b
3:	subq.l	#1,%d2
	bpl.s	1b
	not.l	%d0
	rts

colours:
	.word	'm', 0xf0f, 'a', 0xff0, 'r', 0xf80, 'c', 0xf00, 'd', 0xfff

dosname:
	.asciz	"dos.library"

	.globl	_bb_code_end
_bb_code_end:

	.org	PVBB_OFFSET
pvbb:
	.long	PVBB_MAGIC
	.word	PVBB_VERSION
	.word	BOOT_FLOPPY
	.long	0, 0, 0, 0, 0, 0	/* start nblocks size alloc crc entry */
	.long	115200			/* baud */

	.org	1024
