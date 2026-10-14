/*
 * One-shot filter for exec Alert(): swallows AN_BootError (0x30000001),
 * which strap raises when the floppy's boot block declines to boot (see
 * decline_boot() in stage0.c), and then puts the original Alert() back.
 * Every other alert goes straight to the original function.
 *
 * stage0 copies ap_start..ap_end into memory that stays allocated and
 * installs ap_start with SetFunction(); it stores the old vector in ap_old.
 * Position independent.
 */
	.set	AN_BOOTERROR,	0x30000001
	.set	LVO_ALERT,	-108
	.set	LVO_SETFUNCTION, -420

	.text
	.globl	ap_start, ap_old, ap_end
ap_start:
	cmp.l	#AN_BOOTERROR,%d7
	beq.s	1f
	move.l	ap_old(%pc),-(%sp)	/* any other alert: original Alert() */
	rts
1:	movem.l	%d0-%d1/%a0-%a1/%a6,-(%sp)
	move.l	4,%a6
	move.l	%a6,%a1			/* library: exec */
	move.w	#LVO_ALERT,%a0		/* function offset (sign-extended) */
	move.l	ap_old(%pc),%d0		/* restore the original */
	jsr	LVO_SETFUNCTION(%a6)
	movem.l	(%sp)+,%d0-%d1/%a0-%a1/%a6
	rts
	.balign	4
ap_old:
	.long	0
ap_end:
