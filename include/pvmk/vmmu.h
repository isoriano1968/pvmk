/*
 * Shadow MMU ("virtual TLB") for the guest.
 *
 * The guest sees the MMU of the real CPU: a 68030 PMMU (TC, CRP, SRP,
 * TT0/TT1, MMUSR, PFLUSH, PTEST) or a 68040/68060 MMU (TC, URP, SRP,
 * ITT/DTT, PFLUSH, PTEST/PLPA), and builds its own tables, which
 * SORIX-ILOM-LDOM never installs. The real MMU translates the guest (always in
 * real user mode) through shadow tables of a fixed geometry (68030: 8/8/5
 * index bits, 2 KB pages; 68040/68060: 7/7/6, 4 KB pages), filled on
 * demand from the guest's tables:
 *
 *  - two views, chosen by the virtual S bit: kernel (guest SRP if SRE,
 *    else CRP; supervisor-only pages accessible) and user (guest CRP;
 *    supervisor-only pages invalid). The ATC is flushed when the view
 *    changes, since both run with user function codes;
 *  - an access fault whose page the guest maps is resolved here (U and M
 *    bits kept in the guest's descriptors; pages stay write-protected in
 *    the shadow until the guest's M bit is set); any other fault is the
 *    guest's;
 *  - guest PFLUSH and any change of TC, CRP, SRP, TT0, TT1 drop the
 *    shadow tables;
 *  - SORIX-ILOM-LDOM's hole is never mapped.
 *
 * SORIX-ILOM-LDOM itself runs with transparent translation covering everything
 * for supervisor accesses (68030 TT0; 68040/68060 DTT0/DTT1/ITT0), so its
 * own accesses are physical and never fault. Access errors: 68030 frames
 * A/B (rerun), 68060 format 4 (restart), 68040 format 7 (restart, with
 * the pending write-backs completed by SORIX-ILOM-LDOM for shadow misses).
 */
#ifndef PVMK_VMMU_H
#define PVMK_VMMU_H

#include "pvmk/mk.h"
#include "pvmk/types.h"

/* 68030 MMUSR bits */
#define MMUSR_B		0x8000		/* bus error (here: SORIX-ILOM-LDOM's hole) */
#define MMUSR_L		0x4000		/* limit violation */
#define MMUSR_S		0x2000		/* supervisor only */
#define MMUSR_W		0x0800		/* write protected */
#define MMUSR_I		0x0400		/* invalid */
#define MMUSR_M		0x0200		/* modified */
#define MMUSR_T		0x0040		/* transparent translation */
#define MMUSR_N		0x0007		/* levels searched */
#define MMUSR_FAULT	(MMUSR_B | MMUSR_L | MMUSR_S | MMUSR_I)

void vmmu_reset(void);			/* new guest */
u32  vmmu_scratch(u32 size);		/* boot: 4 KB aligned, before use */
void vmmu_flush(void);			/* guest flush, root or TC change */
void vmmu_flush_user(void);		/* new CRP: the user view only */
void vmmu_pflush(void);			/* guest PFLUSH: revalidate */
void vmmu_activate(void);		/* before entering the guest */
void vmmu_off(void);			/* SORIX-ILOM-LDOM gives the machine away */

/*
 * Translate a guest virtual address the way the guest's MMU would
 * (kernel view if super), setting U (and M if write) in its descriptors.
 * 0 and *pa on success; otherwise the MMUSR fault bits.
 */
u16  vmmu_translate(u32 va, int super, int write, u32 *pa);

/* PTEST for the guest: MMUSR; *desc gets the last descriptor's address */
u16  vmmu_ptest(u32 va, u32 fc, int write, u32 level, u32 *desc);

/* A bus error from the guest: 0 if it was a shadow miss (now mapped; RTE
 * reruns the access), 1 the same but trace one instruction (watchpoint),
 * -1 if it is the guest's to handle. */
int  vmmu_fault(struct excregs *r);

void vmmu_watch(u32 va);		/* guest write watchpoint, 0 = off */
int  vmmu_watch_step(void);		/* 1: the trace was the watchpoint's */
int  vmmu_watch_check(u32 *old, u32 *now);	/* changed behind the MMU? */

void vmmu_show(void);
void vmmu_dump(u32 va, int super);	/* shell "pt" */

/* core/vcpu_s.S */
void mmu030_load(const u32 *crp, u32 tc, u32 tt0);
void mmu030_root(const u32 *crp);	/* new CRP, flush the ATC */
void mmu030_flush(void);		/* PFLUSHA */
void mmu030_off(void);
void mmu040_load(u32 urp, u32 tc, u32 dtt0, u32 dtt1, u32 itt0);
void mmu040_root(u32 urp);		/* new URP, flush the ATCs */
void mmu040_flush(void);		/* PFLUSHA */
void mmu040_off(void);

#endif /* PVMK_VMMU_H */
