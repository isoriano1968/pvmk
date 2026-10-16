/*
 * pvtest — conformance test guest for the SORIX-ILOM-LDOM virtual CPU.
 *
 * Runs deprivileged (real user mode) as if it were a kernel in supervisor
 * mode, linked at PVTEST_BASE. Every privileged instruction below traps to
 * SORIX-ILOM-LDOM and is emulated; exceptions come back through pvtest's own
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
	/* MMU tests: tables and pages in chip RAM above pvtest */
	.set	TAB_A,		0x70000		/* 68030: 256 x 4 */
	.set	TAB_B,		0x70400		/* 256 x 4 */
	.set	TAB_C,		0x70800		/* 16 x 4 */
	.set	ROOT40,		0x70000		/* 68040/68060: 128 x 4 */
	.set	PTR40,		0x70200		/* 128 x 4 */
	.set	PT40,		0x70400		/* 64 x 4 */
	.set	PG0,		0x78000
	.set	PG1,		0x79000
	.set	VA_T,		0x40000000	/* the test window */
	.set	PAT0,		0x5a5a0000
	.set	PAT1,		0xa5a50001

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
1:	move.l	#0x00004000,%d0			/* P (8 KB), E off */
	.short	0x4e7b,0x0003			/* movec %d0,%tc */
	.short	0x4e7a,0x1003			/* movec %tc,%d1 */
	cmp.l	%d0,%d1
2:	RESULT	n_mmu, eq

	/* --- translation through the guest's own tables --- */
	cmp.l	#30,cpu
	bne	mmu040_test
	bsr	mmu030_tables
	lea	h_buserr(%pc),%a1
	move.l	%a1,vectors+2*4
	clr.l	nfaults
	lea	crp030,%a0
	.short	0xf010,0x4c00			/* pmove (%a0),%crp */
	.short	0xf000,0x2400			/* pflusha */
	lea	tc030,%a0
	.short	0xf010,0x4000			/* pmove (%a0),%tc */
	move.l	VA_T,%d0			/* mapped page 0 */
	cmp.l	#PAT0,%d0
	RESULT	n_mmuread, eq
	move.l	#0x11223344,VA_T+4		/* write-protected: fixed */
	cmp.l	#0x11223344,PG0+4		/*  by the handler, rerun */
	bne.s	1f
	cmp.l	#1,nfaults
	bne.s	1f
	cmp.l	#VA_T+4,faddr
1:	RESULT	n_mmuwp, eq
	move.l	VA_T+0x1000,%d0			/* invalid page 1: mapped */
	cmp.l	#PAT1,%d0			/*  by the handler */
	bne.s	1f
	cmp.l	#2,nfaults
	bne.s	1f
	cmp.l	#VA_T+0x1000,faddr
1:	RESULT	n_mmuinval, eq
	lea	VA_T,%a0			/* ptest: mapped, then invalid */
	.short	0xf010,0x9e15			/* ptestr #5,(%a0),#7 */
	.short	0xf039,0x6200			/* pmove %psr,mmusr */
	.long	mmusr
	move.w	mmusr,%d0
	and.w	#0x0400,%d0			/* I */
	bne.s	1f
	lea	VA_T+0x2000,%a0
	.short	0xf010,0x9e15
	.short	0xf039,0x6200
	.long	mmusr
	move.w	mmusr,%d0
	and.w	#0x0400,%d0
	cmp.w	#0x0400,%d0
1:	RESULT	n_mmuptest, eq
	lea	tcoff,%a0
	.short	0xf010,0x4000			/* pmove (%a0),%tc: off */
	.short	0xf000,0x2400			/* pflusha */
	move.l	PG0+4,%d0			/* identity again */
	cmp.l	#0x11223344,%d0
	RESULT	n_mmuoff, eq
	bra	8f

	/* 68040/68060: low 16 MB through DTT0/ITT0, VA_T through tables */
mmu040_test:
	bsr	mmu040_tables
	lea	h_buserr(%pc),%a1
	move.l	%a1,vectors+2*4
	clr.l	nfaults
	move.l	#0x0000c000,%d0			/* 00xxxxxx, any FC, cached */
	.short	0x4e7b,0x0006			/* movec %d0,%dtt0 */
	.short	0x4e7b,0x0004			/* movec %d0,%itt0 */
	move.l	#ROOT40,%d0
	.short	0x4e7b,0x0806			/* movec %d0,%urp */
	.short	0x4e7b,0x0807			/* movec %d0,%srp */
	.short	0xf518				/* pflusha */
	move.l	#0x00008000,%d0			/* E, 4 KB pages */
	.short	0x4e7b,0x0003			/* movec %d0,%tc */
	move.l	VA_T,%d0
	cmp.l	#PAT0,%d0
	RESULT	n_mmuread, eq
	move.l	#0x11223344,VA_T+4
	cmp.l	#0x11223344,PG0+4
	bne.s	1f
	cmp.l	#1,nfaults
	bne.s	1f
	cmp.l	#VA_T+4,faddr
1:	RESULT	n_mmuwp, eq
	move.l	VA_T+0x1000,%d0
	cmp.l	#PAT1,%d0
	bne.s	1f
	cmp.l	#2,nfaults
	bne.s	1f
	cmp.l	#VA_T+0x1000,faddr
1:	RESULT	n_mmuinval, eq
	moveq	#0,%d0
	.short	0x4e7b,0x0003			/* movec %d0,%tc: off */
	.short	0x4e7b,0x0006			/* dtt0 */
	.short	0x4e7b,0x0004			/* itt0 */
	.short	0xf518				/* pflusha */
	move.l	PG1,%d0				/* identity again */
	cmp.l	#PAT1,%d0
	RESULT	n_mmuoff, eq
8:

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

	stop	#0x2700				/* all masked: continues */
	RESULT	n_stop, ra

	/* STOP with interrupts enabled waits for one, which is delivered */
	clr.l	vblanks
	move.w	#0x0020,INTREQ
	move.w	#0xc020,INTENA
	stop	#0x2000
	move.w	#0x2700,%sr
	move.w	#0x0020,INTENA
	tst.l	vblanks
	RESULT	n_idle, ne

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

/*
 * 68030 tables for the MMU tests (in chip RAM above pvtest): 4 KB pages,
 * TIA 8 / TIB 8 / TIC 4; level A maps every 16 MB identically (early
 * termination) except VA_T's, which goes through B and C: page 0 of the
 * window is PG0, write-protected; page 1 is invalid until a fault maps it
 * to PG1.
 */
mmu030_tables:
	lea	TAB_A,%a0
	moveq	#0,%d0
	move.w	#255,%d1
1:	move.l	%d0,%d2
	or.l	#1,%d2				/* page descriptor: identity */
	move.l	%d2,(%a0)+
	add.l	#0x01000000,%d0
	dbf	%d1,1b
	move.l	#TAB_B+2,TAB_A+(VA_T>>24)*4	/* short table */
	lea	TAB_B,%a0
	move.w	#255+16,%d1
1:	clr.l	(%a0)+				/* B (256) and C (16): invalid */
	dbf	%d1,1b
	move.l	#TAB_C+2,TAB_B
	move.l	#PG0+5,TAB_C			/* page, write-protected */
	move.l	#PAT0,PG0
	move.l	#PAT1,PG1
	clr.l	PG0+4
	rts

/* --- handlers (run in virtual supervisor mode) --- */

/*
 * Bus/access error from the MMU tests: note the fault address, fix the
 * mapping and return to rerun or restart the access. The fault address
 * is at +0x10 in 68030 frames (format A, B), +0x14 in 68040 frames (7),
 * +8 in 68060 frames (4).
 */
h_buserr:
	movem.l	%d0/%a0,-(%sp)
	addq.l	#1,nfaults
	move.w	8+6(%sp),%d0			/* format */
	lsr.w	#8,%d0
	lsr.w	#4,%d0
	lea	8+0x10(%sp),%a0
	cmp.w	#7,%d0
	bne.s	1f
	lea	8+0x14(%sp),%a0
1:	cmp.w	#4,%d0
	bne.s	1f
	lea	8+8(%sp),%a0
1:	move.l	(%a0),%d0
	move.l	%d0,faddr
	and.l	#0xfffff000,%d0
	cmp.l	#VA_T,%d0
	bne.s	2f
	bsr	mmu_fix_wp			/* page 0: clear WP */
	bra.s	3f
2:	cmp.l	#VA_T+0x1000,%d0
	bne.s	9f
	bsr	mmu_map_p1			/* page 1: map it */
3:	move.w	8+6(%sp),%d0			/* 68040: complete the */
	and.w	#0xf000,%d0			/*  pending write-backs */
	cmp.w	#0x7000,%d0
	bne.s	4f
	lea	8(%sp),%a0
	bsr	wb040
4:	movem.l	(%sp)+,%d0/%a0
	rte
9:	movem.l	(%sp)+,%d0/%a0
	bra	unexpected

/*
 * 68040 access error frame at a0: do the valid write-backs WB3, WB2, WB1
 * (status +0x0E/+0x10/+0x12, address and data +0x18/+0x1C, +0x20/+0x24,
 * +0x28/+0x2C) and clear their valid bits, as the CPU does not.
 */
wb040:
	movem.l	%d0-%d2/%a1-%a2,-(%sp)
	moveq	#0x0e,%d1			/* WB3 */
	moveq	#0x18,%d2
	bsr.s	1f
	moveq	#0x10,%d1			/* WB2 */
	moveq	#0x20,%d2
	bsr.s	1f
	moveq	#0x12,%d1			/* WB1 */
	moveq	#0x28,%d2
	bsr.s	1f
	movem.l	(%sp)+,%d0-%d2/%a1-%a2
	rts
1:	move.w	(%a0,%d1.w),%d0			/* status */
	btst	#7,%d0
	beq.s	9f
	bclr	#7,1(%a0,%d1.w)
	move.l	(%a0,%d2.w),%a1			/* address */
	lea	4(%a0,%d2.w),%a2		/* data */
	lsr.w	#5,%d0
	and.w	#3,%d0
	cmp.w	#1,%d0
	beq.s	2f
	cmp.w	#2,%d0
	beq.s	3f
	move.l	(%a2),(%a1)			/* long */
	rts
2:	move.b	3(%a2),(%a1)			/* byte */
	rts
3:	move.w	2(%a2),(%a1)			/* word */
9:	rts

mmu_fix_wp:
	cmp.l	#30,cpu
	bne.s	1f
	move.l	#PG0+1,TAB_C
	.short	0xf000,0x2400			/* pflusha */
	rts
1:	move.l	#PG0+1,PT40
	.short	0xf518				/* pflusha (68040/68060) */
	rts

mmu_map_p1:
	cmp.l	#30,cpu
	bne.s	1f
	move.l	#PG1+1,TAB_C+4
	.short	0xf000,0x2400			/* pflusha */
	rts
1:	move.l	#PG1+1,PT40+4
	.short	0xf518
	rts

/*
 * 68040/68060 tables, 4 KB pages: root (128) and pointer (128) tables,
 * one page table (64) for VA_T: page 0 PG0 write-protected, page 1
 * invalid until a fault maps it.
 */
mmu040_tables:
	lea	ROOT40,%a0
	move.w	#128+128+64-1,%d1
1:	clr.l	(%a0)+
	dbf	%d1,1b
	move.l	#PTR40+2,ROOT40+(VA_T>>25)*4	/* resident */
	move.l	#PT40+2,PTR40
	move.l	#PG0+5,PT40			/* resident, W */
	move.l	#PAT0,PG0
	move.l	#PAT1,PG1
	clr.l	PG0+4
	rts

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
n_stop:		.asciz	"stop with all interrupts masked"
n_idle:		.asciz	"stop waits for an interrupt"
n_mmuread:	.asciz	"mmu: read through the guest's tables"
n_mmuwp:	.asciz	"mmu: write-protect fault, fixed, rerun"
n_mmuinval:	.asciz	"mmu: invalid page fault, mapped, rerun"
n_mmuptest:	.asciz	"mmu: ptest (mapped, invalid)"
n_mmuoff:	.asciz	"mmu: translation off again"

	.data
	.balign	4
tcval:		.long	0x00c07760
tc030:		.long	0x80c08840		/* E, 4K, TIA 8 TIB 8 TIC 4 */
tcoff:		.long	0
crp030:		.long	0x7fff0002, TAB_A	/* short descriptors */
nfaults:	.long	0
faddr:		.long	0
mmusr:		.word	0
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
