/*
 * pvmk image entry. Must be linked first (offset 0).
 *
 * Called by the boot block with:
 *	A0 = image base   A1 = IOStdReq   A6 = SysBase
 *	D0 = boot medium  D1 = serial baud  D2 = first image block on the device
 *
 * Applies the image's own relocation table (the image is linked at 0),
 * moves the table to just past bss (takeover reuses it to relocate the
 * image again), clears bss, flushes caches, switches to the pvmk boot stack and calls
 * stage0_main(sysbase, ioreq, medium, baud, base, start). Returns its result in
 * D0 to the boot block on the caller's stack.
 */
#include "pvmk/amiga.h"
#include "pvmk/image.h"

	.section .text.entry,"ax"
	.globl	_start
_start:
	bra.w	entry
	.long	PVMK_MAGIC
	.word	PVMK_HDR_VERSION
	.word	PVMK_HDR_SIZE
hdr_file_size:	.long	0		/* filled in by mkimage */
hdr_bss_end:	.long	0
hdr_reloc_off:	.long	0
hdr_reloc_cnt:	.long	0
hdr_alloc:	.long	0
	.long	0, 0

entry:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	lea	_start(%pc),%a2		/* image base */
	move.l	%a1,%a5
	move.l	%d0,%d6
	move.l	%d1,%d7

	/* relocate: for each offset, *(base + offset) += base */
	move.l	%a2,%d4
	move.l	hdr_reloc_off(%pc),%d2
	lea	(%a2,%d2.l),%a3
	move.l	hdr_reloc_cnt(%pc),%d3
	bra.s	2f
1:	move.l	(%a3)+,%d2
	add.l	%d4,(%a2,%d2.l)
2:	subq.l	#1,%d3
	bpl.s	1b

	/*
	 * Keep the table: move it from reloc_off to bss_end. The destination
	 * is never below the source, so copy from the end.
	 */
	move.l	hdr_reloc_cnt(%pc),%d3
	move.l	%d3,%d2
	lsl.l	#2,%d2
	move.l	hdr_reloc_off(%pc),%d5
	add.l	%d2,%d5
	lea	(%a2,%d5.l),%a3		/* source end */
	move.l	hdr_bss_end(%pc),%d5
	add.l	%d2,%d5
	lea	(%a2,%d5.l),%a4		/* destination end */
	bra.s	7f
6:	move.l	-(%a3),-(%a4)
7:	subq.l	#1,%d3
	bpl.s	6b

	/* clear bss */
	move.l	hdr_file_size(%pc),%d2
	lea	(%a2,%d2.l),%a3
	move.l	hdr_bss_end(%pc),%d3
	sub.l	%d2,%d3
	lsr.l	#2,%d3
	bra.s	4f
3:	clr.l	(%a3)+
4:	subq.l	#1,%d3
	bpl.s	3b

	/* relocation patched code: push data cache, invalidate icache */
	cmp.w	#37,EB_VERSION(%a6)
	blo.s	5f
	jsr	LVO_CacheClearU(%a6)
5:
	move.l	%sp,%d5
	move.l	#_boot_stack_top,%sp	/* absolute; relocated above */
	move.l	%d5,-(%sp)

	move.l	%d5,%a0
	move.l	(%a0),-(%sp)		/* start: D2 as saved on entry */
	move.l	%a2,-(%sp)		/* base */
	move.l	%d7,-(%sp)		/* baud */
	move.l	%d6,-(%sp)		/* medium */
	move.l	%a5,-(%sp)		/* ioreq */
	move.l	%a6,-(%sp)		/* sysbase */
	jsr	stage0_main
	lea	24(%sp),%sp

	move.l	(%sp)+,%sp
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

/*
 * long exec_doio(void *sysbase, void *ioreq)
 * Returns io_Error (sign-extended).
 */
	.text
	.globl	exec_doio
exec_doio:
	move.l	%a6,-(%sp)
	move.l	8(%sp),%a6
	move.l	12(%sp),%a1
	jsr	LVO_DoIO(%a6)
	ext.w	%d0
	ext.l	%d0
	move.l	(%sp)+,%a6
	rts

	.section .bss
	.balign	4
	.space	8192
	.globl	_boot_stack_top
_boot_stack_top:
