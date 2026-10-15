/*
 * MMU self-test, low level (see cpu/mmutest.c for the tables).
 *
 * With interrupts masked: load the root pointer and the transparent
 * translation registers, enable translation, read and write the test page
 * through its mapping, then read the invalid page, which must cause an
 * access fault. The fault comes back through probe_abort() to label 9
 * with the stack as saved in probe_sp (other registers are not preserved
 * on that path). Translation is switched off again on every path.
 *
 * Returns -1 if the CPU has no MMU (the first MMU instruction trapped),
 * 0 if the invalid page did not fault, 1 if it faulted (expected).
 * out[0] gets the value read through the mapping.
 *
 * MMU instructions are emitted as .short (the file assembles for -m68020-60).
 */
	.set	PATTERN_W,	0x5a5aa5a5

	.text

/*
 * int mmu_test030(const u32 regs[6], u32 va_ok, u32 va_bad, u32 *out)
 * regs: CRP (2 longs), TC, TT0, TT1, 0 (for switching off)
 */
	.globl	mmu_test030
mmu_test030:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)		/* 44 bytes; args at 48 */
	move.l	48(%sp),%a2
	moveq	#-1,%d7
	lea	8f(%pc),%a0
	move.l	%a0,probe_resume
	.short	0xf000,0x2400			/* pflusha: F-line without MMU */
	clr.l	probe_resume
	lea	9f(%pc),%a0
	move.l	%a0,probe_bus_resume
	move.l	%sp,probe_sp
	lea	12(%a2),%a0
	.short	0xf010,0x0800			/* pmove (%a0),%tt0 */
	lea	16(%a2),%a0
	.short	0xf010,0x0c00			/* pmove (%a0),%tt1 */
	move.l	%a2,%a0
	.short	0xf010,0x4c00			/* pmove (%a0),%crp */
	lea	8(%a2),%a0
	.short	0xf010,0x4000			/* pmove (%a0),%tc: on */
	move.l	52(%sp),%a0
	move.l	(%a0),%d0
	move.l	60(%sp),%a1
	move.l	%d0,(%a1)
	move.l	#PATTERN_W,(%a0)
	moveq	#0,%d7
	move.l	56(%sp),%a0
	move.l	(%a0),%d0			/* must fault */
	bra.s	7f
9:	moveq	#1,%d7
7:	move.l	48(%sp),%a2			/* registers are not kept on a fault */
	clr.l	probe_bus_resume
	lea	20(%a2),%a0
	.short	0xf010,0x4000			/* pmove (%a0),%tc: off */
	.short	0xf010,0x0800			/* pmove (%a0),%tt0 */
	.short	0xf010,0x0c00			/* pmove (%a0),%tt1 */
	.short	0xf000,0x2400			/* pflusha */
8:	clr.l	probe_resume
	move.l	%d7,%d0
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

/*
 * int mmu_test040(const u32 regs[4], u32 va_ok, u32 va_bad, u32 *out)
 * regs: root table (URP/SRP), TC, TT0 (ITT0/DTT0), TT1 (ITT1/DTT1).
 * Same for the 68060.
 */
	.globl	mmu_test040
mmu_test040:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a2
	moveq	#-1,%d7
	lea	8f(%pc),%a0
	move.l	%a0,probe_resume
	move.l	(%a2),%d0
	.short	0x4e7b,0x0807			/* movec %d0,%srp */
	clr.l	probe_resume
	.short	0x4e7b,0x0806			/* movec %d0,%urp */
	move.l	8(%a2),%d0
	.short	0x4e7b,0x0004			/* movec %d0,%itt0 */
	.short	0x4e7b,0x0006			/* movec %d0,%dtt0 */
	move.l	12(%a2),%d0
	.short	0x4e7b,0x0005			/* movec %d0,%itt1 */
	.short	0x4e7b,0x0007			/* movec %d0,%dtt1 */
	.short	0xf518				/* pflusha */
	lea	9f(%pc),%a0
	move.l	%a0,probe_bus_resume
	move.l	%sp,probe_sp
	move.l	4(%a2),%d0
	.short	0x4e7b,0x0003			/* movec %d0,%tc: on */
	move.l	52(%sp),%a0
	move.l	(%a0),%d0
	move.l	60(%sp),%a1
	move.l	%d0,(%a1)
	move.l	#PATTERN_W,(%a0)
	moveq	#0,%d7
	move.l	56(%sp),%a0
	move.l	(%a0),%d0			/* must fault */
	bra.s	7f
9:	moveq	#1,%d7
7:	move.l	48(%sp),%a2			/* registers are not kept on a fault */
	clr.l	probe_bus_resume
	moveq	#0,%d0
	.short	0x4e7b,0x0003			/* movec %d0,%tc: off */
	.short	0x4e7b,0x0004			/* movec %d0,%itt0 */
	.short	0x4e7b,0x0005			/* movec %d0,%itt1 */
	.short	0x4e7b,0x0006			/* movec %d0,%dtt0 */
	.short	0x4e7b,0x0007			/* movec %d0,%dtt1 */
	.short	0xf518				/* pflusha */
	.short	0xf4f8				/* cpusha %bc */
8:	clr.l	probe_resume
	move.l	%d7,%d0
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

/*
 * void probe_abort(void) -- from exception() on a bus/access error while
 * probe_bus_resume is set: drop the exception frame and continue there.
 */
	.globl	probe_abort
probe_abort:
	move.l	probe_sp,%sp
	move.l	probe_bus_resume,%a0
	jmp	(%a0)

	.bss
	.balign	4
	.globl	probe_bus_resume, probe_sp
probe_bus_resume:
	.space	4
probe_sp:
	.space	4
