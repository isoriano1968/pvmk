/*
 * Virtual CPU: a guest runs in user mode while SORIX-LDOM keeps supervisor
 * mode, its own VBR and the MMU. Privileged instructions trap and are
 * emulated against the virtual state below; exceptions are reflected to the
 * guest through its virtual VBR; TRAP #15 from guest supervisor mode is a
 * hypercall.
 *
 * Memory is identity mapped with the MMU off in M3 (no protection yet);
 * the shadow MMU arrives in M4.
 */
#ifndef PVMK_VCPU_H
#define PVMK_VCPU_H

#include "pvmk/mk.h"
#include "pvmk/types.h"

/* virtual SR bits */
#define VSR_T1		0x8000
#define VSR_T0		0x4000
#define VSR_S		0x2000
#define VSR_M		0x1000
#define VSR_IPL		0x0700
#define VSR_CCR		0x001f

/* hypercalls: TRAP #15 with virtual S = 1, D0 = number, result in D0 */
#define HC_VERSION	0		/* D0 = 'SORX', D1 = ABI, D2 = CPU */
#define HC_PUTC		1		/* D1 = character */
#define HC_GETC		2		/* D0 = character or -1 */
#define HC_IDLE		3
#define HC_EXIT		7		/* D1 = exit code: back to the shell */
#define HC_BREAK	8		/* enter the SORIX-LDOM shell */
#define HC_WRITE	9		/* A0 = buffer, D1 = length */
#define HC_MAGIC	0x534f5258	/* 'SORX' */
#define HC_ABI		1

/* guest register context while it is not running */
struct vctx {
	u32 d[8];
	u32 a[8];			/* a[7]: guest's current A7 */
	u16 sr;				/* real SR to return with (user mode) */
	u32 pc;
} __attribute__((packed));

struct vcpu {
	int active;			/* a guest exists */
	int running;			/* exceptions from user mode are its */
	struct vctx ctx;		/* while stopped (break, exit) */
	const char *name;

	u16 vsr;			/* system byte; CCR lives in the real SR */
	u32 usp;			/* guest USP while virtual S = 1 */
	u32 isp, msp;			/* inactive supervisor stack(s) */
	u32 vbr, sfc, dfc, cacr, caar;
	/* 68030 PMMU */
	u32 tc, crp[2], srp[2], tt0, tt1;
	u16 mmusr;
	/* 68040/68060 */
	u32 urp, srp40, tcr, itt0, itt1, dtt0, dtt1, pcr, buscr, mmusr40;

	int exit_code;
	u32 stop_sig;			/* why it stopped (GDB signal number) */
	int stop_swbreak;		/* ... at a GDB software breakpoint */
	u32 n_emul, n_reflect, n_hcall, n_irq, n_idle;
	u32 idle_ms;			/* time spent waiting in STOP */
};

extern struct vcpu guest;

/* Start a guest at pc with virtual SR = $2700, supervisor stack ssp and
 * D0 = d0. */
void vcpu_start(const char *name, u32 pc, u32 ssp, u32 d0)
	__attribute__((noreturn));
void vcpu_prepare(const char *name, u32 pc, u32 ssp, u32 d0);	/* stopped */
void vcpu_resume(void) __attribute__((noreturn));
void vcpu_set_ps(u16 sr);		/* change the stopped guest's SR */
void vcpu_show(void);

/* From exception(): an exception taken while the guest ran (user mode). */
void vcpu_exception(struct excregs *r);

/* core/vcpu_s.S */
void vcpu_enter(const struct vctx *c, u32 ssp) __attribute__((noreturn));
u32  fpu_save(void *buf);		/* FSAVE to buf; returns frame bytes */
void fpu_restore(const void *buf);
void cache_push_all(u32 model);
u32  idle_stop(u32 level);		/* STOP until an interrupt > level */
extern volatile u32 idle_active, idle_vec;

#endif /* PVMK_VCPU_H */
