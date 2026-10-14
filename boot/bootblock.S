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
 *	D0 = boot medium  D1 = serial baud
 * If the image returns D0 = 0, AmigaOS boot continues (dos.library init).
 *
 * Serial progress characters: B (entry) L (loaded) C (CRC ok) J (jump);
 * errors print "E" followed by a code letter and turn the screen red:
 *	Em bad PVBB magic   Ea AllocMem failed   Er read error
 *	Ec CRC mismatch     Ed dos.library not found
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

	/* one CMD_READ of nblocks from start */
	moveq	#'r',%d7
	move.l	%a5,%a1
	move.w	#CMD_READ,IO_COMMAND(%a1)
	move.l	%a3,IO_DATA(%a1)
	moveq	#9,%d1
	move.l	PVBB_O_NBLOCKS(%a4),%d0
	lsl.l	%d1,%d0
	move.l	%d0,IO_LENGTH(%a1)
	move.l	PVBB_O_START(%a4),%d0
	lsl.l	%d1,%d0
	move.l	%d0,IO_OFFSET(%a1)
	jsr	LVO_DoIO(%a6)
	tst.b	%d0
	bne	fail
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
	move.l	%a3,%a2
	add.l	PVBB_O_ENTRY(%a4),%a2
	jsr	(%a2)
	tst.l	%d0
	bne.s	out			/* image reported failure */

	/* Image returned 0: continue booting AmigaOS from this disk. */
	moveq	#'d',%d7
	lea	dosname(%pc),%a1
	jsr	LVO_FindResident(%a6)
	tst.l	%d0
	beq.s	fail
	move.l	%d0,%a0
	move.l	RT_INIT(%a0),%a0
	moveq	#0,%d0
	bra.s	out

fail:
	move.w	#0xf00,COLOR00		/* red */
	moveq	#'E',%d0
	bsr	ser_putc
	move.l	%d7,%d0
	bsr	ser_putc
	moveq	#'\r',%d0
	bsr	ser_putc
	moveq	#'\n',%d0
	bsr	ser_putc
	move.l	%a3,%d0
	beq.s	1f
	move.l	%a3,%a1
	move.l	PVBB_O_ALLOC(%a4),%d0
	jsr	LVO_FreeMem(%a6)
1:	moveq	#-1,%d0
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
