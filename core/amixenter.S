/*
 * void amix_enter(const struct amix_handoff *h)   -- does not return
 *
 * Final step of a native AMIX boot, like AMIX's copyit.s: with interrupts,
 * DMA and the MMU off (SORIX-LDOM keeps them that way after takeover),
 * copy .text, .rodata and .data from the loaded image to their final
 * addresses, make the instruction cache forget the old memory contents,
 * and jump to the kernel with D0 = 3 (EXEC1) and D1 = &bootinfo. The
 * kernel installs its own VBR and stack and clears its .bss.
 *
 * The image buffer lies above the kernel's final location, so a forward
 * copy is safe. This code and its stack are outside both.
 */
	.set	H_SP,		0
	.set	H_ENTRY,	4
	.set	H_D0,		8
	.set	H_D1,		12
	.set	H_TBUF,		16
	.set	H_TVADDR,	20
	.set	H_TSIZE,	24
	.set	H_RBUF,		28
	.set	H_RVADDR,	32
	.set	H_RSIZE,	36
	.set	H_DBUF,		40
	.set	H_DVADDR,	44
	.set	H_DSIZE,	48
	.set	H_CPU,		52

	.set	DMACON,		0xdff096
	.set	INTENA,		0xdff09a
	.set	INTREQ,		0xdff09c

	.text
	.globl	amix_enter
amix_enter:
	move.l	4(%sp),%a2
	move.w	#0x2700,%sr
	move.w	#0x7dff,DMACON		/* our display goes away; */
	move.w	#0x8200,DMACON		/* DMAEN stays, as Kickstart has it */
	move.w	#0x7fff,INTENA
	move.w	#0x7fff,INTREQ

	move.l	H_TBUF(%a2),%a0
	move.l	H_TVADDR(%a2),%a1
	move.l	H_TSIZE(%a2),%d0
	bsr.s	copy
	move.l	H_RBUF(%a2),%a0
	move.l	H_RVADDR(%a2),%a1
	move.l	H_RSIZE(%a2),%d0
	bsr.s	copy
	move.l	H_DBUF(%a2),%a0
	move.l	H_DVADDR(%a2),%a1
	move.l	H_DSIZE(%a2),%d0
	bsr.s	copy

	cmp.l	#40,H_CPU(%a2)
	bhs.s	1f
	movec	%cacr,%d0
	or.w	#0x0808,%d0		/* 030: clear instruction and data cache */
	movec	%d0,%cacr
	bra.s	2f
1:	.short	0xf4f8			/* 040/060: cpusha %bc */
2:
	move.l	H_SP(%a2),%sp
	move.l	H_ENTRY(%a2),%a0
	move.l	H_D0(%a2),%d0
	move.l	H_D1(%a2),%d1
	jmp	(%a0)

/* a0 = source, a1 = destination, d0 = bytes (destination below source) */
copy:
	tst.l	%d0
	beq.s	9f
	move.l	%a0,%d1
	move.l	%a1,%d2
	or.l	%d2,%d1
	or.l	%d0,%d1
	and.w	#3,%d1
	bne.s	5f
	lsr.l	#2,%d0			/* all long-aligned: longwords */
3:	move.l	(%a0)+,(%a1)+
	subq.l	#1,%d0
	bne.s	3b
	rts
5:	move.b	(%a0)+,(%a1)+
	subq.l	#1,%d0
	bne.s	5b
9:	rts
