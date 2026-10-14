/*
 * Virtual CPU: trap-and-emulate for a deprivileged guest.
 *
 * The guest always runs in real user mode. Its virtual supervisor state is
 * vsr (system byte; the condition codes stay in the real SR), and while it
 * is in virtual supervisor mode the real USP holds its supervisor stack
 * pointer. Every exception the guest causes enters SORIX-LDOM:
 *
 *  - privilege violation in virtual supervisor mode: the instruction is
 *    emulated against the virtual registers;
 *  - TRAP #15 in virtual supervisor mode: a hypercall;
 *  - anything else (traps, faults, interrupts, privilege violations in
 *    virtual user mode): reflected to the guest through its virtual VBR,
 *    with a frame on its supervisor stack, as the CPU itself would.
 *
 * Interrupts: on every return to the guest the real interrupt mask is set to
 * the virtual one, so the hardware masks exactly what the guest masked and
 * an interrupt that arrives can always be delivered.
 */
#include "pvmk/amiga.h"
#include "pvmk/kprintf.h"
#include "pvmk/log.h"
#include "pvmk/paula.h"
#include "pvmk/vcpu.h"

struct vcpu guest;

/* serial input that arrives while the guest runs, for HC_GETC */
static u8 rxbuf[32];
static u32 rxhead, rxlen;

#define VEC_ILLEGAL	4
#define VEC_PRIV	8
#define VEC_FORMAT	14
#define VEC_TRAP15	47

#define FAULT		(-1)

static u16
rd16(u32 a)
{
	return *(volatile u16 *)(uintptr_t)a;
}

static u32
rd32(u32 a)
{
	return *(volatile u32 *)(uintptr_t)a;
}

static void
wr16(u32 a, u16 v)
{
	*(volatile u16 *)(uintptr_t)a = v;
}

static void
wr32(u32 a, u32 v)
{
	*(volatile u32 *)(uintptr_t)a = v;
}

/* Full virtual SR: system byte from vsr, condition codes from the real SR. */
static u16
cur_vsr(const struct excregs *r)
{
	return (guest.vsr & 0xff00) | (r->sr & VSR_CCR);
}

/*
 * Change the virtual SR, switching the guest stack pointer (real USP, in
 * r->a[7]) between user, interrupt and master stacks as the CPU would.
 */
static void
set_vsr(struct excregs *r, u16 nsr)
{
	u16 old = guest.vsr;

	if (cpuinfo.model == 60)
		nsr &= ~VSR_M;			/* the 68060 has no MSP */
	if ((old ^ nsr) & VSR_S) {
		if (nsr & VSR_S) {
			guest.usp = r->a[7];
			r->a[7] = nsr & VSR_M ? guest.msp : guest.isp;
		} else {
			if (old & VSR_M)
				guest.msp = r->a[7];
			else
				guest.isp = r->a[7];
			r->a[7] = guest.usp;
		}
	} else if ((nsr & VSR_S) && ((old ^ nsr) & VSR_M)) {
		if (old & VSR_M) {
			guest.msp = r->a[7];
			r->a[7] = guest.isp;
		} else {
			guest.isp = r->a[7];
			r->a[7] = guest.msp;
		}
	}
	guest.vsr = nsr & 0xff00;
	r->sr = (r->sr & 0xff00) | (nsr & VSR_CCR);
}

/* The real SR to run the guest with: user mode, virtual IPL and trace. */
static u16
real_sr(const struct excregs *r)
{
	return (guest.vsr & (VSR_T1 | VSR_T0 | VSR_IPL)) | (r->sr & VSR_CCR);
}

/* ------------------------------------------------------------------ */
/* Effective addresses                                                 */

struct ea {
	int is_reg;			/* Dn or An */
	u32 *reg;
	int is_imm;
	u32 imm;
	u32 addr;
};

static u32 *
reg_ptr(struct excregs *r, u32 n)	/* 0-7 Dn, 8-15 An */
{
	return n < 8 ? &r->d[n] : &r->a[n - 8];
}

static u32
index_value(struct excregs *r, u16 ext)
{
	u32 v = *reg_ptr(r, (ext >> 12) & 15);

	if (!(ext & 0x0800))
		v = (u32)(s32)(s16)v;		/* .w index */
	return v << ((ext >> 9) & 3);		/* scale (68020+) */
}

/*
 * Decode mode/reg; *pc points to the next extension word and is advanced.
 * Pre-decrement and post-increment are applied. Returns 0 or FAULT
 * (unsupported mode, e.g. full extension word).
 */
static int
ea_decode(struct excregs *r, u32 mode, u32 regno, u32 size, u32 *pc,
    struct ea *e)
{
	u32 *an = &r->a[regno];
	u32 step = size == 1 && regno == 7 ? 2 : size;
	u16 ext;

	e->is_reg = e->is_imm = 0;
	switch (mode) {
	case 0:
		e->is_reg = 1;
		e->reg = &r->d[regno];
		return 0;
	case 1:
		e->is_reg = 1;
		e->reg = an;
		return 0;
	case 2:
		e->addr = *an;
		return 0;
	case 3:
		e->addr = *an;
		*an += step;
		return 0;
	case 4:
		*an -= step;
		e->addr = *an;
		return 0;
	case 5:
		e->addr = *an + (u32)(s32)(s16)rd16(*pc);
		*pc += 2;
		return 0;
	case 6:
		ext = rd16(*pc);
		if (ext & 0x0100)
			return FAULT;		/* full format: not needed here */
		e->addr = *an + (u32)(s32)(s8)ext + index_value(r, ext);
		*pc += 2;
		return 0;
	}
	switch (regno) {			/* mode 7 */
	case 0:
		e->addr = (u32)(s32)(s16)rd16(*pc);
		*pc += 2;
		return 0;
	case 1:
		e->addr = rd32(*pc);
		*pc += 4;
		return 0;
	case 2:
		e->addr = *pc + (u32)(s32)(s16)rd16(*pc);
		*pc += 2;
		return 0;
	case 3:
		ext = rd16(*pc);
		if (ext & 0x0100)
			return FAULT;
		e->addr = *pc + (u32)(s32)(s8)ext + index_value(r, ext);
		*pc += 2;
		return 0;
	case 4:
		e->is_imm = 1;
		if (size == 4) {
			e->imm = rd32(*pc);
			*pc += 4;
		} else {
			e->imm = size == 2 ? rd16(*pc) : rd16(*pc) & 0xff;
			*pc += 2;
		}
		return 0;
	}
	return FAULT;
}

static u32
ea_read(const struct ea *e, u32 size)
{
	if (e->is_imm)
		return e->imm;
	if (e->is_reg)
		return size == 4 ? *e->reg : size == 2 ? *e->reg & 0xffff :
		    *e->reg & 0xff;
	if (size == 4)
		return rd32(e->addr);
	if (size == 2)
		return rd16(e->addr);
	return *(volatile u8 *)(uintptr_t)e->addr;
}

static void
ea_write(const struct ea *e, u32 size, u32 v)
{
	if (e->is_reg) {
		if (size == 4)
			*e->reg = v;
		else if (size == 2)
			*e->reg = (*e->reg & 0xffff0000) | (v & 0xffff);
		else
			*e->reg = (*e->reg & 0xffffff00) | (v & 0xff);
	} else if (size == 4)
		wr32(e->addr, v);
	else if (size == 2)
		wr16(e->addr, (u16)v);
	else
		*(volatile u8 *)(uintptr_t)e->addr = (u8)v;
}

/* ------------------------------------------------------------------ */
/* Reflection                                                          */

static u32
frame_size(u32 fmt)
{
	static const u8 sizes[16] = {
		8, 8, 12, 12, 16, 0, 0, 60, 0, 20, 32, 92, 0, 0, 0, 0
	};

	return sizes[fmt & 15];
}

/*
 * Deliver exception vec to the guest: switch to its supervisor stack, push
 * a frame (a copy of the real one, or format 0) and continue at its
 * handler. ipl >= 0 also raises the virtual interrupt mask (interrupts).
 */
static void
reflect(struct excregs *r, u32 vec, int copy_frame, int ipl)
{
	const u8 *frame = (const u8 *)&r->sr;
	u32 fmt = copy_frame ? r->fmtvec >> 12 : 0;
	u32 size = frame_size(fmt), sp, i;
	u16 old = cur_vsr(r), nsr;

	if (!size) {
		fmt = 0;
		size = 8;
	}
	nsr = (old | VSR_S) & ~(VSR_T1 | VSR_T0);
	if (ipl >= 0)
		nsr = (nsr & ~VSR_IPL) | (u16)(ipl << 8);
	set_vsr(r, nsr);
	sp = r->a[7] - size;
	for (i = 8; i < size; i += 2)		/* extra words of the frame */
		wr16(sp + i, *(const u16 *)(frame + i));
	wr16(sp, old);
	wr32(sp + 2, r->pc);
	wr16(sp + 6, (u16)(fmt << 12 | vec << 2));
	r->a[7] = sp;
	r->pc = rd32(guest.vbr + vec * 4);
	guest.n_reflect++;
}

/* ------------------------------------------------------------------ */
/* Emulation                                                           */

static int
movec(struct excregs *r, u16 op, u32 *pc)
{
	u16 ext = rd16(*pc);
	u32 *rn = reg_ptr(r, ext >> 12), cr = ext & 0xfff, *p = NULL;
	int to_cr = op & 1, m = (int)cpuinfo.model;

	*pc += 2;
	switch (cr) {
	case 0x000: p = &guest.sfc; break;
	case 0x001: p = &guest.dfc; break;
	case 0x002: p = &guest.cacr; break;
	case 0x800: p = &guest.usp; break;
	case 0x801: p = &guest.vbr; break;
	case 0x802: if (m <= 30) p = &guest.caar; break;
	case 0x803: if (m < 60) p = &guest.msp; break;
	case 0x804: p = &guest.isp; break;
	case 0x003: if (m >= 40) p = &guest.tcr; break;
	case 0x004: if (m >= 40) p = &guest.itt0; break;
	case 0x005: if (m >= 40) p = &guest.itt1; break;
	case 0x006: if (m >= 40) p = &guest.dtt0; break;
	case 0x007: if (m >= 40) p = &guest.dtt1; break;
	case 0x805: if (m == 40) p = &guest.mmusr40; break;
	case 0x806: if (m >= 40) p = &guest.urp; break;
	case 0x807: if (m >= 40) p = &guest.srp40; break;
	case 0x008: if (m == 60) p = &guest.buscr; break;
	case 0x808: if (m == 60) p = &guest.pcr; break;
	}
	if (!p)
		return FAULT;			/* the CPU would: illegal */
	/* the active supervisor stack pointer is the real USP */
	if ((guest.vsr & VSR_S) && ((cr == 0x804 && !(guest.vsr & VSR_M)) ||
	    (cr == 0x803 && (guest.vsr & VSR_M))))
		p = &r->a[7];
	if (to_cr) {
		*p = *rn;
		if (cr == 0x002 && (*rn & 0x0808))	/* 030 cache clear bits */
			cache_push_all(cpuinfo.model);
	} else
		*rn = *p;
	return 0;
}

/* 68030 PMMU: PMOVE, PFLUSH, PLOAD, PTEST (line F, coprocessor id 0). */
static int
pmmu030(struct excregs *r, u16 op, u32 *pc)
{
	u16 ext = rd16(*pc);
	u32 mode = (op >> 3) & 7, regno = op & 7, *p = NULL, size = 4;
	struct ea e;

	*pc += 2;
	switch (ext & 0xe000) {
	case 0x0000:				/* PMOVE TT0/TT1 */
	case 0x4000:				/* PMOVE TC/SRP/CRP */
		switch ((ext >> 10) & 7) {
		case 0:
			if (ext & 0x4000)
				p = &guest.tc;
			break;
		case 2:
			p = ext & 0x4000 ? guest.srp : &guest.tt0;
			break;
		case 3:
			p = ext & 0x4000 ? guest.crp : &guest.tt1;
			break;
		}
		if ((ext & 0x4000) && p != &guest.tc)
			size = 8;			/* SRP, CRP: 64 bits */
		if (!p || ea_decode(r, mode, regno, 4, pc, &e) || e.is_reg)
			return FAULT;
		if (ext & 0x0200) {		/* MMU register to memory */
			wr32(e.addr, p[0]);
			if (size == 8)
				wr32(e.addr + 4, p[1]);
		} else {
			p[0] = rd32(e.addr);
			if (size == 8)
				p[1] = rd32(e.addr + 4);
		}
		return 0;
	case 0x6000:				/* PMOVE MMUSR */
		if (ea_decode(r, mode, regno, 2, pc, &e))
			return FAULT;
		if (ext & 0x0200)
			ea_write(&e, 2, guest.mmusr);
		else
			guest.mmusr = (u16)ea_read(&e, 2);
		return 0;
	case 0x2000:				/* PFLUSH / PLOAD */
		if ((ext & 0x1c00) == 0x1800 &&	/* PFLUSH fc,mask,<ea> */
		    ea_decode(r, mode, regno, 4, pc, &e))
			return FAULT;
		return 0;			/* nothing cached yet (M4) */
	case 0x8000:				/* PTEST */
		if (ea_decode(r, mode, regno, 4, pc, &e))
			return FAULT;
		guest.mmusr = 0;
		return 0;
	}
	return FAULT;
}

/* MOVES: guest address spaces are all the same memory for now. */
static int
moves(struct excregs *r, u16 op, u32 *pc)
{
	u16 ext = rd16(*pc);
	u32 size = ((op >> 6) & 3) == 0 ? 1 : ((op >> 6) & 3) == 1 ? 2 : 4;
	u32 *rn = reg_ptr(r, ext >> 12);
	struct ea e;

	*pc += 2;
	if (ea_decode(r, (op >> 3) & 7, op & 7, size, pc, &e) || e.is_reg ||
	    e.is_imm)
		return FAULT;
	if (ext & 0x0800)
		ea_write(&e, size, *rn);
	else if (ext & 0x8000)
		*rn = size == 4 ? ea_read(&e, size) :
		    (u32)(size == 2 ? (s32)(s16)ea_read(&e, 2) :
		    (s32)(s8)ea_read(&e, 1));
	else
		ea_write(&(struct ea){ .is_reg = 1, .reg = rn }, size,
		    ea_read(&e, size));
	return 0;
}

static int
fsave_frestore(struct excregs *r, u16 op, u32 *pc)
{
	static u8 buf[256] __attribute__((aligned(4)));
	u32 mode = (op >> 3) & 7, regno = op & 7, n, i;
	struct ea e;

	if (cpuinfo.fpu < 0 && cpuinfo.model != 60)
		return FAULT;
	if (!(op & 0x0040)) {			/* FSAVE <ea>: -(An) or control */
		n = fpu_save(buf);
		if (mode == 4) {
			r->a[regno] -= n;
			e.addr = r->a[regno];
		} else if (ea_decode(r, mode, regno, 4, pc, &e) || e.is_reg)
			return FAULT;
		for (i = 0; i < n; i++)
			*(volatile u8 *)(uintptr_t)(e.addr + i) = buf[i];
		return 0;
	}
	/* FRESTORE <ea>: (An)+ or control */
	if (mode == 3)
		e.addr = r->a[regno];
	else if (ea_decode(r, mode, regno, 4, pc, &e) || e.is_reg)
		return FAULT;
	n = (u32)*(volatile u8 *)(uintptr_t)(e.addr + 1) + 4;
	if (n > sizeof(buf))
		return FAULT;
	for (i = 0; i < n; i++)
		buf[i] = *(volatile u8 *)(uintptr_t)(e.addr + i);
	fpu_restore(buf);
	if (mode == 3)
		r->a[regno] += n;
	return 0;
}

/* RTE from virtual supervisor mode: pop a format 0/2 frame. */
static int
rte(struct excregs *r)
{
	u32 sp = r->a[7];
	u16 sr = rd16(sp), fv = rd16(sp + 6);
	u32 pc = rd32(sp + 2), size = frame_size(fv >> 12);

	if ((fv >> 12) != 0 && (fv >> 12) != 2) {
		kprintf("vcpu: RTE with frame format %x not supported yet\n",
		    fv >> 12);
		return FAULT;
	}
	r->a[7] = sp + size;
	set_vsr(r, sr);
	r->pc = pc;
	return 0;
}

/*
 * Emulate the privileged instruction at r->pc (virtual supervisor mode).
 * Returns 0 when done (r->pc advanced), FAULT for "reflect as illegal".
 */
static int
emulate(struct excregs *r)
{
	u32 pc = r->pc + 2, mode, regno;
	u16 op = rd16(r->pc);
	struct ea e;

	mode = (op >> 3) & 7;
	regno = op & 7;
	if ((op & 0xffc0) == 0x40c0) {			/* MOVE SR,<ea> */
		if (ea_decode(r, mode, regno, 2, &pc, &e) || e.is_imm)
			return FAULT;
		ea_write(&e, 2, cur_vsr(r));
	} else if ((op & 0xffc0) == 0x46c0) {		/* MOVE <ea>,SR */
		if (ea_decode(r, mode, regno, 2, &pc, &e))
			return FAULT;
		set_vsr(r, (u16)ea_read(&e, 2));
	} else if (op == 0x027c) {			/* ANDI #,SR */
		set_vsr(r, cur_vsr(r) & rd16(pc));
		pc += 2;
	} else if (op == 0x007c) {			/* ORI #,SR */
		set_vsr(r, cur_vsr(r) | rd16(pc));
		pc += 2;
	} else if (op == 0x0a7c) {			/* EORI #,SR */
		set_vsr(r, cur_vsr(r) ^ rd16(pc));
		pc += 2;
	} else if ((op & 0xfff0) == 0x4e60) {		/* MOVE An,USP / USP,An */
		if (op & 8)
			r->a[regno] = guest.usp;
		else
			guest.usp = r->a[regno];
	} else if ((op & 0xfffe) == 0x4e7a) {		/* MOVEC */
		if (movec(r, op, &pc))
			return FAULT;
	} else if (op == 0x4e73) {			/* RTE */
		return rte(r);
	} else if (op == 0x4e72) {			/* STOP #imm */
		set_vsr(r, rd16(pc));
		pc += 2;				/* no idling yet */
	} else if (op == 0x4e70) {			/* RESET: ignored */
	} else if ((op & 0xff00) == 0x0e00) {		/* MOVES */
		if (moves(r, op, &pc))
			return FAULT;
	} else if ((op & 0xffc0) == 0xf000 && cpuinfo.model == 30) {
		if (pmmu030(r, op, &pc))		/* PMOVE, PFLUSH, ... */
			return FAULT;
	} else if ((op & 0xff80) == 0xf300) {		/* FSAVE / FRESTORE */
		if (fsave_frestore(r, op, &pc))
			return FAULT;
	} else if ((op & 0xff00) == 0xf400 && cpuinfo.model >= 40) {
		cache_push_all(cpuinfo.model);		/* CINV / CPUSH */
	} else if ((op & 0xffe0) == 0xf500 && cpuinfo.model >= 40) {
		/* PFLUSH (An), PFLUSHN, PFLUSHA: nothing cached yet (M4) */
	} else if ((op & 0xffd8) == 0xf548 && cpuinfo.model == 40) {
		guest.mmusr40 = 0;			/* PTEST */
	} else if ((op & 0xffb8) == 0xf588 && cpuinfo.model == 60) {
		/* PLPA: identity mapping, address unchanged */
	} else
		return FAULT;
	if (op != 0x4e73)
		r->pc = pc;
	guest.n_emul++;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Hypercalls and stopping                                             */

static void
stop_guest(struct excregs *r, const char *why) __attribute__((noreturn));

static void
stop_guest(struct excregs *r, const char *why)
{
	u32 i;

	for (i = 0; i < 8; i++) {
		guest.ctx.d[i] = r->d[i];
		guest.ctx.a[i] = r->a[i];
	}
	guest.ctx.sr = real_sr(r);
	guest.ctx.pc = r->pc;
	guest.running = 0;
	REG16(CUSTOM_INTENA) = 0x0800;		/* serial receive interrupt off */
	con_interactive(0);
	kprintf("vcpu: %s %s at PC %08x, SR %04x\n", guest.name, why, r->pc,
	    cur_vsr(r));
	restart(sstack_top(), shell_main);
}

static void
hypercall(struct excregs *r)
{
	u32 i;
	int c;

	guest.n_hcall++;
	switch (r->d[0]) {
	case HC_VERSION:
		r->d[0] = HC_MAGIC;
		r->d[1] = HC_ABI;
		r->d[2] = cpuinfo.model;
		break;
	case HC_PUTC:
		con_putc((int)(r->d[1] & 0xff));
		break;
	case HC_GETC:
		if (rxlen) {
			c = rxbuf[rxhead];
			rxhead = (rxhead + 1) % sizeof(rxbuf);
			rxlen--;
		} else
			c = paula_getc();
		r->d[0] = (u32)c;
		break;
	case HC_IDLE:
		break;
	case HC_WRITE:
		for (i = 0; i < r->d[1]; i++)
			con_putc(*(volatile u8 *)(uintptr_t)(r->a[0] + i));
		break;
	case HC_EXIT:
		guest.exit_code = (int)r->d[1];
		guest.active = 0;
		stop_guest(r, "exited");
	case HC_BREAK:
		stop_guest(r, "stopped (break)");
	default:
		r->d[0] = (u32)-1;
		break;
	}
}

/*
 * Serial input while the guest runs belongs to SORIX-LDOM: Ctrl-C breaks
 * into the shell, anything else is kept for HC_GETC. Checked on the serial
 * receive interrupt and on every other entry from the guest, so a guest
 * running with interrupts masked can still be stopped whenever it traps.
 */
static void
poll_serial(struct excregs *r)
{
	int c;

	while ((c = paula_getc()) >= 0) {
		if (c == 0x03)
			stop_guest(r, "stopped (Ctrl-C)");
		if (rxlen < sizeof(rxbuf))
			rxbuf[(rxhead + rxlen++) % sizeof(rxbuf)] = (u8)c;
	}
}

void
vcpu_exception(struct excregs *r)
{
	u32 vec = (r->fmtvec & 0xfff) >> 2;

	poll_serial(r);
	if (vec >= 25 && vec <= 31) {			/* autovector interrupt */
		if (vec == 29 && !(REG16(CUSTOM_INTREQR) & 0x1000)) {
			r->sr = real_sr(r);		/* serial receive: ours */
			return;
		}
		guest.n_irq++;
		reflect(r, vec, 0, (int)(vec - 24));
	} else if (vec == VEC_PRIV && (guest.vsr & VSR_S)) {
		if (emulate(r))
			reflect(r, VEC_ILLEGAL, 0, -1);
	} else if (vec == VEC_TRAP15 && (guest.vsr & VSR_S)) {
		hypercall(r);				/* PC is past the TRAP */
	} else
		reflect(r, vec, 1, -1);
	r->sr = real_sr(r);
}

/* ------------------------------------------------------------------ */
/* Starting, resuming                                                  */

void
vcpu_start(const char *name, u32 pc, u32 ssp, u32 d0)
{
	memset(&guest, 0, sizeof(guest));
	rxhead = rxlen = 0;
	guest.ctx.d[0] = d0;
	guest.name = name;
	guest.active = 1;
	guest.vsr = 0x2700;
	guest.ctx.a[7] = ssp;			/* virtual S = 1: real USP = ISP */
	guest.ctx.sr = real_sr(&(struct excregs){ .sr = 0 });
	guest.ctx.pc = pc;
	kprintf("vcpu: starting %s at %08x (deprivileged, virtual SR 2700)\n",
	    name, pc);
	vcpu_resume();
}

void
vcpu_resume(void)
{
	if (!guest.active) {
		kprintf("vcpu: no guest to resume\n");
		restart(sstack_top(), shell_main);
	}
	guest.running = 1;
	con_interactive(0);
	REG16(CUSTOM_INTENA) = 0xc800;		/* master + serial receive */
	vcpu_enter(&guest.ctx, sstack_top());
}

void
vcpu_show(void)
{
	const struct vctx *c = &guest.ctx;
	int i;

	if (!guest.active && !guest.name) {
		kprintf("no guest\n");
		return;
	}
	kprintf("guest %s: %s\n", guest.name, guest.active ? "stopped" :
	    "exited");
	kprintf("  PC %08x  virtual SR %04x  VBR %08x  CACR %08x\n", c->pc,
	    (guest.vsr & 0xff00) | (c->sr & VSR_CCR), guest.vbr, guest.cacr);
	for (i = 0; i < 8; i++)
		kprintf("  D%u %08x%s", i, c->d[i], i % 4 == 3 ? "\n" : "");
	for (i = 0; i < 8; i++)
		kprintf("  A%u %08x%s", i, c->a[i], i % 4 == 3 ? "\n" : "");
	kprintf("  USP %08x ISP %08x MSP %08x SFC %u DFC %u\n",
	    guest.vsr & VSR_S ? guest.usp : c->a[7],
	    guest.vsr & VSR_S && !(guest.vsr & VSR_M) ? c->a[7] : guest.isp,
	    guest.vsr & VSR_S && (guest.vsr & VSR_M) ? c->a[7] : guest.msp,
	    guest.sfc, guest.dfc);
	kprintf("  emulated %u, reflected %u, hypercalls %u, interrupts %u\n",
	    guest.n_emul, guest.n_reflect, guest.n_hcall, guest.n_irq);
}
