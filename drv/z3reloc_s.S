/*
 * z3reloc_s.S -- the Z3660 FPGA bus reset from the 060's side (z3reloc.c).
 *
 * u32 z3reloc_spin(u32 n): n turns of the wait loop (calibration); returns 0.
 *
 * void z3reloc_pulse(volatile u32 *reg, u32 val, u32 n): with interrupts
 * off, writes val to *reg (the "BRST" register), then turns the wait loop n
 * times without a single bus cycle: core 0 pulses REG0 bit 31 for 10 ms
 * after the write's ack, and every 060 cycle in flight then (CPU RAM, chip
 * RAM, CIA, ROM all go through the FPGA) would be lost.  So the routine runs
 * twice: a first pass without the write and with a short wait brings its
 * lines into the instruction cache (pvmk runs with the MMU off and the
 * instruction cache on, CACR 0x8000), the second pass writes and waits.
 * Only registers inside the window; the stack is touched before and after.
 */
	.text
	.even
	.globl	z3reloc_spin
z3reloc_spin:
	move.l	4(%sp),%d0
1:	subq.l	#1,%d0
	bne.s	1b
	rts

	.balign	16
	.globl	z3reloc_pulse
z3reloc_pulse:
	movem.l	%d2-%d4,-(%sp)
	move.l	16(%sp),%a0		/* reg */
	move.l	20(%sp),%d0		/* val */
	move.l	24(%sp),%d1		/* n */
	move.w	%sr,%d3
	ori.w	#0x0700,%sr		/* interrupts off */
	moveq	#1,%d2			/* pass 1: warm the cache, pass 0: the real one */
.Lpass:
	tst.l	%d2
	bne.s	.Lnowrite
	move.l	%d0,(%a0)		/* "BRST": acked by core 0 before the pulse */
	nop				/* the write is done, the store buffer empty */
.Lnowrite:
	move.l	%d1,%d4
	tst.l	%d2
	beq.s	.Lwait
	moveq	#16,%d4
.Lwait:
	subq.l	#1,%d4
	bne.s	.Lwait
	subq.l	#1,%d2
	bpl.s	.Lpass
	move.w	%d3,%sr
	movem.l	(%sp)+,%d2-%d4
	rts

/*
 * int safe_write16(u32 addr, u32 val), int safe_read32(u32 addr, u32 *val):
 * one word write / one long read that returns -1 instead of faulting on a
 * bus error (an unconfigured board at 0xFF000000 does not answer), as
 * safe_copy (core/vcpu_s.S), whose byte accesses autoconfig cannot use.
 */
	.globl	safe_write16
safe_write16:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)		/* 44 bytes; args at 48 */
	lea	9f(%pc),%a0
	move.l	%a0,probe_bus_resume
	move.l	%sp,probe_sp
	move.l	48(%sp),%a1
	move.l	52(%sp),%d0
	move.w	%d0,(%a1)
	nop
	moveq	#0,%d7
	bra.s	8f
9:	moveq	#-1,%d7
8:	clr.l	probe_bus_resume
	move.l	%d7,%d0
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.globl	safe_read32
safe_read32:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	lea	9f(%pc),%a0
	move.l	%a0,probe_bus_resume
	move.l	%sp,probe_sp
	move.l	48(%sp),%a1
	move.l	52(%sp),%a2
	move.l	(%a1),%d0
	nop
	move.l	%d0,(%a2)
	moveq	#0,%d7
	bra.s	8f
9:	moveq	#-1,%d7
8:	clr.l	probe_bus_resume
	move.l	%d7,%d0
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts
