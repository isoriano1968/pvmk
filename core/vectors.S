/*
 * Exception entry for every vector. The vector number comes from the
 * format/vector word of the exception frame (68020+ frames all have it).
 *
 * Saves D0-D7/A0-A7 below the frame and calls exception(struct excregs *).
 * If it returns (probe traps), registers are restored and RTE resumes at
 * the possibly modified PC.
 */
	.text
	.globl	exc_entry
exc_entry:
	movem.l	%d0-%d7/%a0-%a7,-(%sp)
	move.l	%sp,-(%sp)
	jsr	exception
	addq.l	#4,%sp
	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	addq.l	#4,%sp			/* saved A7 */
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
