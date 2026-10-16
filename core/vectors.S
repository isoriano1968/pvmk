/*
 * Exception entry for every vector. The vector number comes from the
 * format/vector word of the exception frame (68020+ frames all have it).
 *
 * Masks interrupts (an exception from a guest leaves the CPU at the guest's
 * interrupt level), saves D0-D7/A0-A6 and the user stack pointer (in the A7
 * slot: for exceptions from a guest that is the guest's A7) below the frame
 * and calls exception(struct excregs *). If it returns, registers and USP
 * are restored (possibly modified) and RTE resumes.
 */
	.text
	.globl	exc_entry
exc_entry:
	ori.w	#0x0700,%sr
	subq.l	#4,%sp			/* A7 slot: USP */
	movem.l	%d0-%d7/%a0-%a6,-(%sp)
	move.l	%usp,%a0
	move.l	%a0,60(%sp)
	move.l	%sp,-(%sp)
	jsr	exception
	addq.l	#4,%sp
	move.l	60(%sp),%a0
	move.l	%a0,%usp
	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	addq.l	#4,%sp
	rte

/*
 * void restart(u32 sp, void (*fn)(void))   -- does not return
 * Reset the supervisor stack, mask interrupts and call fn.
 */
	.globl	restart
restart:
	move.l	4(%sp),%d0
	move.l	8(%sp),%a0
	move.w	#0x2700,%sr
	move.l	%d0,%sp
	jsr	(%a0)
1:	bra.s	1b

/* void set_vbr(void *vbr) */
	.globl	set_vbr
set_vbr:
	move.l	4(%sp),%a0
	movec	%a0,%vbr
	rts

/* u32 get_vbr(void), u16 get_sr(void), u32 get_cacr(void) */
	.globl	get_vbr, get_sr, get_cacr
get_vbr:
	movec	%vbr,%d0
	rts
get_sr:
	moveq	#0,%d0
	move.w	%sr,%d0
	rts
get_cacr:
	movec	%cacr,%d0
	rts

/*
 * void icache_inval(u32 model): invalidate the instruction cache after
 * code was written through the (disabled) data cache path.
 */
	.globl	icache_inval
icache_inval:
	cmp.l	#40,4(%sp)
	bhs.s	1f
	movec	%cacr,%d0
	or.w	#0x0008,%d0		/* 030: CI */
	movec	%d0,%cacr
	rts
1:	.short	0xf4f8			/* 040/060: cpusha %bc */
	rts

/*
 * long call_addr(u32 addr)  -- "go": call code at addr like a C function,
 * preserving callee-saved registers, and return its D0.
 */
	.globl	call_addr
call_addr:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a0
	jsr	(%a0)
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

/*
 * Cold reboot into Kickstart (Commodore's documented sequence): RESET,
 * then the prefetched JMP lands on the ROM's second reset instruction.
 * Must sit in one aligned longword. MMU is already off.
 */
	.globl	cold_reboot
	.balign	4
cold_reboot:
	move.w	#0x2700,%sr
	lea	0x01000000,%a0		/* end of ROM */
	sub.l	-0x14(%a0),%a0		/* minus ROM size = ROM start */
	move.l	4(%a0),%a0		/* initial PC */
	subq.l	#2,%a0			/* points to the second RESET */
	.p2alignw 2,0x4e71		/* NOP padding: RESET+JMP in one longword */
	reset
	jmp	(%a0)

/*
 * Line A (vector 10): fast path for the guest kernel's patched spl sites
 * (A10n, A11n, A120, A13n; see aline() in vcpu.c, which also handles
 * everything this leaves to exc_entry). Kernel text is identity mapped,
 * so the opcode is read through SORIX-ILOM-LDOM's own (physical) view. An
 * interrupt arriving here is left pending by exception() (it raises the
 * interrupted SR to IPL 7) and taken when the guest runs again.
 */
	.globl	aline_entry
aline_entry:
	tst.l	aline_fast
	beq	exc_entry
	btst	#5,(%sp)		/* from SORIX-ILOM-LDOM itself */
	bne	exc_entry
	tst.b	(%sp)			/* T1: tracing (GDB, watchpoint) */
	bmi	exc_entry
	movem.l	%d0-%d7/%a0-%a1,-(%sp)	/* 40 bytes: frame at 40(%sp) */
	move.l	42(%sp),%a0		/* PC */
	cmp.l	spl_lo,%a0
	blo	9f
	cmp.l	spl_hi,%a0
	bhs	9f
	move.w	(%a0),%d0		/* opcode */
	move.w	%d0,%d1
	and.w	#0xffc0,%d1
	cmp.w	#0xa100,%d1
	bne	9f
	move.l	vcpu_vsr,%a1
	move.w	(%a1),%d2		/* virtual system byte */
	btst	#13,%d2			/* virtual S */
	beq	9f
	move.w	40(%sp),%d4		/* full virtual SR: system byte + CCR */
	and.w	#0x001f,%d4
	move.w	%d2,%d3
	and.w	#0xff00,%d3
	or.w	%d3,%d4
	moveq	#7,%d1
	and.w	%d0,%d1
	lsl.w	#2,%d1			/* Dn slot */
	move.w	%d0,%d3
	and.w	#0x0030,%d3
	beq.s	1f			/* A10n: move sr,dn */
	cmp.w	#0x0010,%d3
	beq.s	2f			/* A11n: move dn,sr */
	cmp.w	#0x0020,%d3
	beq.s	3f			/* A120: move #imm,sr */
	move.w	4(%a0),%d5		/* A13n: move sr,dn; move #imm,sr */
	moveq	#6,%d6
	bra.s	5f
1:	move.w	%d4,2(%sp,%d1.w)
	addq.l	#2,42(%sp)
	bra.s	8f
2:	move.w	2(%sp,%d1.w),%d5
	moveq	#2,%d6
	bra.s	4f
3:	move.w	%d0,%d3
	and.w	#0x000f,%d3
	bne.s	9f
	move.w	2(%a0),%d5
	moveq	#4,%d6
	bra.s	4f
5:	bsr.s	6f			/* check before Dn changes */
	bne.s	9f
	move.w	%d4,2(%sp,%d1.w)
	bra.s	7f
4:	bsr.s	6f
	bne.s	9f
7:	move.w	%d5,%d3			/* new SR: virtual system byte, */
	and.w	#0xff00,%d3
	move.w	%d3,(%a1)
	and.w	#0x071f,%d5		/* real SR: user mode, IPL, CCR */
	move.w	%d5,40(%sp)
	add.l	%d6,42(%sp)
8:	addq.l	#1,aline_count
	movem.l	(%sp)+,%d0-%d7/%a0-%a1
	rte
/* Z set if the new SR (d5) keeps S and M and has no trace bits */
6:	move.w	%d5,%d3
	eor.w	%d2,%d3
	and.w	#0x3000,%d3
	bne.s	61f
	move.w	%d5,%d3
	and.w	#0xc000,%d3
61:	rts
9:	movem.l	(%sp)+,%d0-%d7/%a0-%a1
	bra	exc_entry
