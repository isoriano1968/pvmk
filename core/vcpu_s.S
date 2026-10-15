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

/*
 * u32 idle_stop(u32 level): STOP the CPU with interrupt mask = level (0-6),
 * supervisor mode. The interrupt that ends it is taken nested; exception()
 * records its vector in idle_vec and returns with interrupts masked, so it
 * stays pending for the guest. Returns the vector.
 */
	.globl	idle_stop, idle_active, idle_vec
idle_stop:
	move.l	4(%sp),%d0
	clr.l	idle_vec
	move.l	#1,idle_active
	lsl.w	#3,%d0
	jmp	1f(%pc,%d0.w)
1:	stop	#0x2000
	bra.w	2f
	stop	#0x2100
	bra.w	2f
	stop	#0x2200
	bra.w	2f
	stop	#0x2300
	bra.w	2f
	stop	#0x2400
	bra.w	2f
	stop	#0x2500
	bra.w	2f
	stop	#0x2600
	bra.w	2f
2:	move.w	#0x2700,%sr
	clr.l	idle_active
	move.l	idle_vec,%d0
	rts

	.bss
	.balign	4
idle_active:	.space	4
idle_vec:	.space	4

/*
 * int safe_copy(void *dst, const void *src, u32 n): byte copy that returns
 * -1 instead of crashing on a bus/access error (GDB memory access).
 */
	.text
	.globl	safe_copy
safe_copy:
	movem.l	%d2-%d7/%a2-%a6,-(%sp)		/* 44 bytes; args at 48 */
	lea	9f(%pc),%a0
	move.l	%a0,probe_bus_resume
	move.l	%sp,probe_sp
	move.l	48(%sp),%a1
	move.l	52(%sp),%a0
	move.l	56(%sp),%d0
	bra.s	2f
1:	move.b	(%a0)+,(%a1)+
2:	subq.l	#1,%d0
	bpl.s	1b
	moveq	#0,%d7
	bra.s	8f
9:	moveq	#-1,%d7
8:	clr.l	probe_bus_resume
	move.l	%d7,%d0
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts
