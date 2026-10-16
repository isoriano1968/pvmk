/*
 * u32 lib_call(void *base, s32 lvo, u32 d0, u32 d1, u32 a0, u32 a1)
 *
 * Call an AmigaOS library function at base+lvo with the given register
 * arguments (stage0 only, while Kickstart is alive). Returns D0.
 */
	.text
	.globl	lib_call
lib_call:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)		/* 44 bytes */
	move.l	48(%sp),%a6
	move.l	52(%sp),%d2
	move.l	56(%sp),%d0
	move.l	60(%sp),%d1
	move.l	64(%sp),%a0
	move.l	68(%sp),%a1
	jsr	0(%a6,%d2.l)
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts

/*
 * u32 sorix_guest(void *sysbase)
 *
 * 'SORX' if this AmigaOS runs as a guest of SORIX-LDOM, else 0. In
 * supervisor mode (exec Supervisor()) TRAP #15 with D0 = 0 is SORIX-LDOM's
 * HC_VERSION hypercall, which returns 'SORX'; natively the trap goes to a
 * handler installed for this one call that returns 0.
 */
	.globl	sorix_guest
sorix_guest:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)		/* 44 bytes */
	move.l	48(%sp),%a6
	lea	1f(%pc),%a5
	jsr	-30(%a6)			/* Supervisor(): 1f, then back */
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts
1:	movec	%vbr,%a0
	move.l	47*4(%a0),%a1			/* TRAP #15 vector */
	lea	2f(%pc),%a2
	move.l	%a2,47*4(%a0)
	moveq	#0,%d0				/* HC_VERSION */
	trap	#15
	move.l	%a1,47*4(%a0)
	rte
2:	moveq	#0,%d0				/* native: no SORIX-LDOM */
	rte
