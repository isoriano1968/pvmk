/*
 * pvtest — conformance test guest for the SORIX-LDOM virtual CPU.
 *
 * Runs deprivileged (real user mode) as if it were a kernel in supervisor
 * mode, linked at PVTEST_BASE. Every privileged instruction below traps to
 * SORIX-LDOM and is emulated; exceptions come back through pvtest's own
 * vector table. Results go to the console through hypercalls; the guest
 * exits with D1 = number of failures. Entered with D0 = 1 it then keeps
 * printing dots forever (for break-in/resume tests).
 */
	.set	HC_VERSION,	0
	.set	HC_PUTC,	1
	.set	HC_EXIT,	7
	.set	HC_WRITE,	9
	.set	HC_MAGIC,	0x534f5258
	.set	INTENA,		0xdff09a
	.set	INTREQ,		0xdff09c

	.macro	HCALL n
	moveq	#\n,%d0
	trap	#15
	.endm

	/* RESULT name, cc: record pass if condition cc holds */
	.macro	RESULT name, cc
	b\cc	1f
	lea	\name(%pc),%a0
	bsr	fail
	bra	2f
1:	lea	\name(%pc),%a0
	bsr	pass
2:
	.endm

	.text
	.globl	_start
_start:
	lea	kstack_top,%sp
	move.l	%d0,loop_mode
	lea	s_hello(%pc),%a0
	bsr	puts

	/* vector table: everything unexpected, then the handlers we use */
	lea	vectors,%a0
	lea	unexpected(%pc),%a1
	move.w	#255,%d1
1:	move.l	%a1,(%a0)+
	dbf	%d1,1b
	lea	vectors,%a0
	lea	h_illegal(%pc),%a1
	move.l	%a1,4*4(%a0)
	lea	h_zerodiv(%pc),%a1
	move.l	%a1,5*4(%a0)
	lea	h_priv(%pc),%a1
	move.l	%a1,8*4(%a0)
	lea	h_level3(%pc),%a1
	move.l	%a1,27*4(%a0)
	lea	h_trap0(%pc),%a1
	move.l	%a1,32*4(%a0)
	lea	h_trap1(%pc),%a1
	move.l	%a1,33*4(%a0)
	lea	h_trap3(%pc),%a1
	move.l	%a1,35*4(%a0)

	/* --- hypercall --- */
	HCALL	HC_VERSION
	move.l	%d2,cpu
	cmp.l	#HC_MAGIC,%d0
	RESULT	n_hcall, eq

	/* --- SR --- */
	move.w	%sr,%d0
	and.w	#0x2700,%d0
	cmp.w	#0x2700,%d0
	RESULT	n_movefromsr, eq

	move.w	#0x2300,%sr
	move.w	%sr,%d0
	cmp.w	#0x2300,%d0
	RESULT	n_movetosr, eq

	ori.w	#0x0400,%sr
	move.w	%sr,%d0
	cmp.w	#0x2700,%d0
	RESULT	n_orisr, eq

	andi.w	#0xf8ff,%sr
	move.w	%sr,%d0
	cmp.w	#0x2000,%d0
	RESULT	n_andisr, eq

	eori.w	#0x0100,%sr
	move.w	%sr,%d0
	cmp.w	#0x2100,%d0
	RESULT	n_eorisr, eq
	move.w	#0x2700,%sr

	lea	srsave,%a0			/* MOVE SR,(An) */
	move.w	%sr,(%a0)
	cmp.w	#0x2700,srsave
	RESULT	n_srmem, eq

	/* --- USP, MOVEC --- */
	move.l	#0x12345678,%a0
	move.l	%a0,%usp
	move.l	%usp,%a1
	cmp.l	%a0,%a1
	RESULT	n_usp, eq

	lea	vectors,%a0
	movec	%a0,%vbr
	movec	%vbr,%d0
	cmp.l	%a0,%d0
	RESULT	n_vbr, eq

	moveq	#5,%d0
	movec	%d0,%sfc
	movec	%sfc,%d1
	cmp.l	%d0,%d1
	RESULT	n_sfc, eq

	move.l	#0x8a5a5a5a,%d0
	movec	%d0,%usp
	move.l	%usp,%a1
	cmp.l	%d0,%a1
	RESULT	n_movecusp, eq

	/* --- exceptions through our own vectors --- */
	clr.l	flag
	trap	#3
	cmp.l	#0x3333,flag
	RESULT	n_trap, eq
	move.w	%sr,%d0
	cmp.w	#0x2700,%d0			/* RTE restored the SR */
	RESULT	n_rte, eq

	clr.l	flag
	illegal
	cmp.l	#0x4444,flag
	RESULT	n_illegal, eq

	clr.l	flag
	moveq	#0,%d1
	moveq	#7,%d0
	divu.w	%d1,%d0
	cmp.l	#0x5555,flag
	RESULT	n_zerodiv, eq

	/* --- virtual user mode and back --- */
	move.l	#ustack_top,%a0
	move.l	%a0,%usp
	move.l	%sp,ksave
	clr.l	flag
	andi.w	#0xdfff,%sr			/* to virtual user mode */
	cmp.l	#ustack_top,%sp			/* SP is now the user stack */
	seq	ucheck
	move.w	%sr,%d0				/* privileged: our handler */
	trap	#0				/* to our kernel handler */
	trap	#1				/* back to supervisor */
back:	tst.b	ucheck
	RESULT	n_usermode, ne
	cmp.l	#0x8888,flag
	RESULT	n_userpriv, eq
	cmp.l	#0x1010,flag2
	RESULT	n_usertrap, eq
	cmp.l	ksave,%sp
	RESULT	n_kstack, eq

	/* --- MMU registers --- */
	cmp.l	#30,cpu
	bne.s	1f
	lea	tcval,%a0
	lea	tcread,%a1
	.short	0xf010,0x4000			/* pmove (%a0),%tc */
	.short	0xf011,0x4200			/* pmove %tc,(%a1) */
	move.l	tcread,%d0
	cmp.l	tcval,%d0
	bra.s	2f
1:	move.l	#0x0000c000,%d0
	.short	0x4e7b,0x0003			/* movec %d0,%tc */
	.short	0x4e7a,0x1003			/* movec %tc,%d1 */
	cmp.l	%d0,%d1
2:	RESULT	n_mmu, eq

	/* --- interrupts: vertical blank, level 3 --- */
	clr.l	vblanks
	move.w	#0x0020,INTREQ
	move.w	#0xc020,INTENA			/* master + VERTB */
	move.w	#0x2000,%sr			/* unmask */
	move.l	#4000000,%d1
1:	cmp.l	#3,vblanks
	bhs.s	2f
	subq.l	#1,%d1
	bne.s	1b
2:	move.w	#0x2700,%sr			/* mask again */
	cmp.l	#3,vblanks
	RESULT	n_irq, hs

	move.l	vblanks,%d2			/* masked: none get through */
	move.l	#400000,%d1
1:	subq.l	#1,%d1
	bne.s	1b
	move.w	#0x0020,INTENA
	cmp.l	vblanks,%d2
	RESULT	n_irqmask, eq

	stop	#0x2700				/* emulated, continues */
	RESULT	n_stop, ra

	/* --- summary --- */
	lea	s_done(%pc),%a0
	bsr	puts
	move.l	fails,%d0
	add.b	#'0',%d0
	bsr	putc
	lea	s_failed(%pc),%a0
	bsr	puts

	tst.l	loop_mode
	beq.s	9f
	move.w	#0x2000,%sr			/* loop: like a kernel, unmasked */
1:	move.l	#300000,%d1			/* loop: dots until stopped */
2:	subq.l	#1,%d1
	bne.s	2b
	moveq	#'.',%d0
	bsr	putc
	bra.s	1b
9:	move.l	fails,%d1
	HCALL	HC_EXIT

/* --- handlers (run in virtual supervisor mode) --- */
h_trap3:
	move.l	#0x3333,flag
	rte
h_illegal:
	move.l	#0x4444,flag
	addq.l	#2,2(%sp)			/* skip the ILLEGAL */
	rte
h_zerodiv:
	move.l	#0x5555,flag
	rte					/* frame PC is past the DIVU */
h_priv:
	move.w	(%sp),%d0			/* from virtual user mode? */
	and.w	#0x2000,%d0
	bne.s	unexpected
	move.l	#0x8888,flag
	addq.l	#2,2(%sp)			/* skip the MOVE SR,D0 */
	rte
h_trap0:
	move.w	(%sp),%d0
	and.w	#0x2000,%d0
	bne.s	unexpected
	move.l	#0x1010,flag2
	rte
h_trap1:					/* return in supervisor mode */
	move.l	ksave,%sp
	move.w	#0x2700,%sr
	bra	back
h_level3:
	addq.l	#1,vblanks
	move.w	#0x0020,INTREQ
	rte
unexpected:
	lea	s_unexpected(%pc),%a0
	bsr	puts
	moveq	#99,%d1
	HCALL	HC_EXIT

/* --- output --- */
pass:	move.l	%a0,-(%sp)
	lea	s_ok(%pc),%a0
	bsr	puts
	move.l	(%sp)+,%a0
	bsr	puts
	moveq	#10,%d0
	bra.s	putc
fail:	addq.l	#1,fails
	move.l	%a0,-(%sp)
	lea	s_fail(%pc),%a0
	bsr	puts
	move.l	(%sp)+,%a0
	bsr	puts
	moveq	#10,%d0
putc:	movem.l	%d0-%d1,-(%sp)
	move.l	%d0,%d1
	HCALL	HC_PUTC
	movem.l	(%sp)+,%d0-%d1
	rts
puts:	movem.l	%d0-%d1/%a0-%a1,-(%sp)
	move.l	%a0,%a1
	moveq	#-1,%d1
1:	addq.l	#1,%d1
	tst.b	(%a1)+
	bne.s	1b
	HCALL	HC_WRITE
	movem.l	(%sp)+,%d0-%d1/%a0-%a1
	rts

s_hello:	.asciz	"pvtest: deprivileged test guest running\n"
s_ok:		.asciz	"pvtest: ok   "
s_fail:		.asciz	"pvtest: FAIL "
s_done:		.asciz	"pvtest: done, "
s_failed:	.asciz	" failed\n"
s_unexpected:	.asciz	"pvtest: FAIL unexpected exception\n"
n_hcall:	.asciz	"hypercall"
n_movefromsr:	.asciz	"move sr,dn"
n_movetosr:	.asciz	"move #,sr"
n_orisr:	.asciz	"ori #,sr"
n_andisr:	.asciz	"andi #,sr"
n_eorisr:	.asciz	"eori #,sr"
n_srmem:	.asciz	"move sr,(an)"
n_usp:		.asciz	"move an,usp / usp,an"
n_vbr:		.asciz	"movec vbr"
n_sfc:		.asciz	"movec sfc"
n_movecusp:	.asciz	"movec usp"
n_trap:		.asciz	"trap through own vbr"
n_rte:		.asciz	"rte restores sr"
n_illegal:	.asciz	"illegal instruction reflected"
n_zerodiv:	.asciz	"zero divide reflected"
n_usermode:	.asciz	"virtual user mode stack switch"
n_userpriv:	.asciz	"privilege violation in user mode reflected"
n_usertrap:	.asciz	"trap from user mode"
n_kstack:	.asciz	"back on the supervisor stack"
n_mmu:		.asciz	"mmu register (pmove tc / movec tc)"
n_irq:		.asciz	"vertical blank interrupts delivered"
n_irqmask:	.asciz	"masked interrupts held back"
n_stop:		.asciz	"stop"

	.data
	.balign	4
tcval:		.long	0x00c07760
tcread:		.long	0
cpu:		.long	0
flag:		.long	0
flag2:		.long	0
fails:		.long	0
ksave:		.long	0
vblanks:	.long	0
loop_mode:	.long	0
srsave:		.word	0
ucheck:		.byte	0

	.data
	.balign	4
vectors:	.space	1024
		.space	2048
ustack_top:
		.space	4096
kstack_top:
