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
