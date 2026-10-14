/*
 * Virtual CPU entry and FPU/cache helpers.
 */
	.set	C_SR,	64		/* struct vctx */
	.set	C_PC,	66

	.text

/*
 * void vcpu_enter(const struct vctx *c, u32 ssp)   -- does not return
 * Load the guest context and RTE into it (user mode).
 */
	.globl	vcpu_enter
vcpu_enter:
	move.w	#0x2700,%sr
	move.l	4(%sp),%a0
	move.l	8(%sp),%sp
	clr.w	-(%sp)			/* format 0, vector 0 */
	move.l	C_PC(%a0),-(%sp)
	move.w	C_SR(%a0),-(%sp)
	move.l	60(%a0),%a1		/* a[7]: guest A7 */
	move.l	%a1,%usp
	movem.l	(%a0),%d0-%d7/%a0-%a6	/* a0 last: loaded from memory */
	rte

/* u32 fpu_save(void *buf): FSAVE into buf, return the frame's size */
	.globl	fpu_save
fpu_save:
	move.l	4(%sp),%a0
	.short	0xf310			/* fsave (%a0) */
	moveq	#0,%d0
	move.b	1(%a0),%d0		/* frame body size; 0 = null frame */
	addq.l	#4,%d0
	rts

/* void fpu_restore(const void *buf) */
	.globl	fpu_restore
fpu_restore:
	move.l	4(%sp),%a0
	.short	0xf350			/* frestore (%a0) */
	rts

/* void cache_push_all(u32 model): write back and invalidate all caches */
	.globl	cache_push_all
cache_push_all:
	cmp.l	#40,4(%sp)
	bhs.s	1f
	movec	%cacr,%d0
	or.w	#0x0808,%d0		/* 030: clear I and D */
	movec	%d0,%cacr
	rts
1:	.short	0xf4f8			/* cpusha %bc */
	rts
