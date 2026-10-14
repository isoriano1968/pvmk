/*
 * CPU feature probes. Each sets probe_resume before trying an instruction
 * that may not exist; exception() then resumes there instead of reporting
 * a fault. Instructions not known to -m68020-60 are emitted as .short.
 */
	.text

/* int probe_pcr(u32 *pcr): 68060 processor configuration register */
	.globl	probe_pcr
probe_pcr:
	lea	1f(%pc),%a0
	move.l	%a0,probe_resume
	moveq	#0,%d0
	.short	0x4e7a,0x1808		/* movec %pcr,%d1 */
	move.l	4(%sp),%a0
	move.l	%d1,(%a0)
	moveq	#1,%d0
1:	clr.l	probe_resume
	rts

/* int probe_urp(void): 68040/68060 user root pointer */
	.globl	probe_urp
probe_urp:
	lea	1f(%pc),%a0
	move.l	%a0,probe_resume
	moveq	#0,%d0
	.short	0x4e7a,0x1806		/* movec %urp,%d1 */
	moveq	#1,%d0
1:	clr.l	probe_resume
	rts

/*
 * int probe_cacr_fd(void): 68030 has a data cache, so CACR bit 9 (FD,
 * freeze data cache) reads back; on a 68020 it reads 0. Restores CACR.
 */
	.globl	probe_cacr_fd
probe_cacr_fd:
	movec	%cacr,%d1
	move.l	%d1,%d0
	or.w	#0x0200,%d0
	movec	%d0,%cacr
	movec	%cacr,%d0
	movec	%d1,%cacr
	and.l	#0x0200,%d0
	sne	%d0
	and.l	#1,%d0
	rts

/*
 * int probe_fpu(void): -1 if no FPU (F-line trap), else the size byte of
 * the FSAVE idle frame (68881: 0x18, 68882: 0x38; 040/060 differ).
 */
	.globl	probe_fpu
probe_fpu:
	lea	1f(%pc),%a0
	move.l	%a0,probe_resume
	moveq	#-1,%d0
	.short	0xf280,0x0000		/* fnop */
	.short	0xf327			/* fsave -(%sp) */
	moveq	#0,%d0
	move.b	1(%sp),%d0
	.short	0xf35f			/* frestore (%sp)+ */
1:	clr.l	probe_resume
	rts

	.bss
	.balign	4
	.globl	probe_resume
probe_resume:
	.space	4
