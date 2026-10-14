/*
 * void takeover(const struct takeover_args *ta, void *sysbase)
 *						-- does not return
 *
 * Take the machine away from Kickstart and move the image into the hole.
 * Boot block code runs as a user-mode task, so first enter supervisor mode
 * with exec Supervisor() (whose frame is simply abandoned), then:
 *  1. mask interrupts, stop all custom chip DMA and interrupts, CIA ints
 *  2. MMU off (030 PMMU, or 040/060 TC + transparent translation regs)
 *  3. caches: data cache off, instruction cache on (flushed)
 *  4. copy the image (text, data, bss, relocation table) to new_base and
 *     re-apply its relocations with delta = new_base - old_base
 *  5. switch to the new supervisor stack and call entry(arg), both
 *     relocated by delta
 *
 * Runtime data copied in step 4 must not hold pointers into the image that
 * were stored at run time from relocated values; such pointers would not
 * be adjusted. bootcap holds only external addresses.
 *
 * Privileged MMU/cache instructions are emitted as .short so the file
 * assembles with -m68020-60.
 */
#include "pvmk/amiga.h"

	.set	TA_OLD,		0
	.set	TA_NEW,		4
	.set	TA_LEN,		8	/* bytes to copy, multiple of 4 */
	.set	TA_RELOFF,	12	/* relocation table offset from base */
	.set	TA_NREL,	16
	.set	TA_CPU,		20	/* 30, 40 or 60 */
	.set	TA_SP,		24	/* new supervisor stack top (absolute) */
	.set	TA_ENTRY,	28	/* entry, old address */
	.set	TA_ARG,		32	/* argument, old address */

	.set	INTENA,		0xdff09a
	.set	DMACON,		0xdff096
	.set	INTREQ,		0xdff09c
	.set	CIAA_ICR,	0xbfed01
	.set	CIAB_ICR,	0xbfdd00

	.set	LVO_Supervisor, -30

	.text
	.globl	takeover
takeover:
	move.l	4(%sp),%a4
	move.l	8(%sp),%a6
	lea	super(%pc),%a5
	jsr	LVO_Supervisor(%a6)	/* continues at super, never returns */
super:
	move.l	%a4,%a5
	move.w	#0x2700,%sr
	move.w	#0x7fff,INTENA
	move.w	#0x7fff,DMACON
	move.w	#0x7fff,INTREQ
	move.w	#0x7fff,INTREQ
	move.b	#0x7f,CIAA_ICR
	move.b	#0x7f,CIAB_ICR
	move.w	#0x000,COLOR00

	moveq	#0,%d0
	cmp.l	#40,TA_CPU(%a5)
	bhs.s	mmu_040

	/* 68030: TC, TT0, TT1 := 0; PFLUSHA; CACR := EI|CI|CD */
	lea	zero(%pc),%a0
	.short	0xf010,0x4000		/* pmove (%a0),%tc */
	.short	0xf010,0x0800		/* pmove (%a0),%tt0 */
	.short	0xf010,0x0c00		/* pmove (%a0),%tt1 */
	.short	0xf000,0x2400		/* pflusha */
	move.l	#0x0809,%d0
	movec	%d0,%cacr
	bra.s	copy

mmu_040:
	/* 68040/68060: TC, ITT0/1, DTT0/1 := 0; PFLUSHA; CPUSHA; CACR := IE */
	.short	0x4e7b,0x0003		/* movec %d0,%tc */
	.short	0x4e7b,0x0004		/* movec %d0,%itt0 */
	.short	0x4e7b,0x0005		/* movec %d0,%itt1 */
	.short	0x4e7b,0x0006		/* movec %d0,%dtt0 */
	.short	0x4e7b,0x0007		/* movec %d0,%dtt1 */
	.short	0xf518			/* pflusha */
	.short	0xf4f8			/* cpusha %bc */
	move.l	#0x00008000,%d0		/* IE; 060: also clears branch cache below */
	cmp.l	#60,TA_CPU(%a5)
	bne.s	1f
	move.l	#0x00408000,%d0		/* EIC | CABC */
1:	movec	%d0,%cacr

copy:
	move.l	TA_OLD(%a5),%a0
	move.l	TA_NEW(%a5),%a1
	move.l	TA_LEN(%a5),%d0
	lsr.l	#2,%d0
	bra.s	2f
1:	move.l	(%a0)+,(%a1)+
2:	subq.l	#1,%d0
	bpl.s	1b

	/* relocate the copy */
	move.l	TA_NEW(%a5),%a1
	move.l	%a1,%d4
	sub.l	TA_OLD(%a5),%d4		/* delta */
	move.l	%a1,%a2
	add.l	TA_RELOFF(%a5),%a2
	move.l	TA_NREL(%a5),%d3
	bra.s	4f
3:	move.l	(%a2)+,%d2
	add.l	%d4,(%a1,%d2.l)
4:	subq.l	#1,%d3
	bpl.s	3b

	/* the copied code is new to the instruction cache */
	cmp.l	#40,TA_CPU(%a5)
	bhs.s	5f
	movec	%cacr,%d0
	or.w	#0x0008,%d0		/* CI */
	movec	%d0,%cacr
	bra.s	6f
5:	.short	0xf4f8			/* cpusha %bc */
6:
	move.l	TA_SP(%a5),%sp
	move.l	TA_ARG(%a5),%d0
	add.l	%d4,%d0
	move.l	%d0,-(%sp)
	move.l	TA_ENTRY(%a5),%a0
	add.l	%d4,%a0
	jsr	(%a0)
7:	bra.s	7b

	.balign	4
zero:	.long	0, 0
