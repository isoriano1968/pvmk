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
#include "pvmk/gdb.h"
#include "pvmk/kprintf.h"
#include "pvmk/log.h"
#include "pvmk/paula.h"
#include "pvmk/vcpu.h"
#include "pvmk/vmmu.h"

struct vcpu guest;

/* serial input that arrives while the guest runs, for HC_GETC */
static u8 rxbuf[32];
static u32 rxhead, rxlen;

#define VEC_BUSERR	2
#define VEC_ILLEGAL	4
#define VEC_LINEA	10
#define VEC_PRIV	8
#define VEC_FORMAT	14
#define VEC_TRAP15	47

#define FAULT		(-1)

#define SIGINT		2
#define SIGTRAP		5
#define SIGSEGV		11

static void
stop_guest(struct excregs *r, const char *why, u32 sig, int swbreak)
    __attribute__((noreturn));

/*
 * Guest memory, as SORIX-LDOM touches it on the guest's behalf (decoding,
 * frames, MOVES): guest virtual addresses, translated through the guest's
 * tables in the current view (kernel view while virtual S = 1, or the
 * view a MOVES function code selects).
 */
static struct excregs *cur_regs;	/* the guest's registers, for faults */

/* profile of emulated instructions and shadow flushes ("guest stats") */
enum {
	P_SR_TO_DN, P_SR_TO_MEM, P_DN_TO_SR, P_IMM_TO_SR, P_MEM_TO_SR,
	P_SR_LOGIC, P_RTE, P_MOVEC, P_USP, P_STOP, P_MOVES, P_PMOVE_REG,
	P_PMOVE_MMU, P_PFLUSH, P_PTEST, P_FPU, P_CACHE, P_OTHER, P_SPL_PAIR,
	P_N
};
static const char *const pnames[P_N] = {
	"move sr,dn", "move sr,<mem>", "move dn,sr", "move #,sr",
	"move <mem>,sr", "andi/ori/eori sr", "rte", "movec", "move usp",
	"stop", "moves", "pmove (no flush)", "pmove tc/crp/srp/tt", "pflush",
	"ptest", "fsave/frestore", "cinv/cpush", "other",
	"  of which spl pairs",
};
static u32 prof[P_N];

/*
 * A guest access that fails while an instruction is emulated: the rest of
 * the instruction goes to a scratch buffer, its effects are undone and the
 * guest gets a bus error for the instruction (restarted after its RTE).
 */
static int in_emul;			/* emulate() is running */
static struct {
	int faulted, write;
	u32 va, size, fc;
} afault;

/* the first exceptions reflected to the guest (not interrupts) */
#define NRLOG		8
static struct { u32 vec, pc, vbr, handler; u16 sr; } rlog[NRLOG];
static u32 nrlog;
static int as_super = -1;		/* MOVES: view override */

static uintptr_t
gpa(u32 va, int write, u32 size)
{
	static u32 scratch[2];
	int super = as_super >= 0 ? as_super : (guest.vsr & VSR_S) != 0;
	u32 pa;
	u16 sr;

	if (afault.faulted)
		return (uintptr_t)scratch;
	sr = vmmu_translate(va, super, write, &pa);
	if (sr && in_emul) {
		afault.faulted = 1;
		afault.va = va;
		afault.write = write;
		afault.size = size;
		afault.fc = as_super >= 0 ? (write ? guest.dfc : guest.sfc) & 7 :
		    super ? 5 : 1;
		return (uintptr_t)scratch;
	}
	if (sr) {
		kprintf("vcpu: guest %s at %08x (%s view) failed, MMUSR %04x\n",
		    write ? "write" : "read", va, super ? "kernel" : "user", sr);
		if (cur_regs)
			stop_guest(cur_regs, "stopped (access fault)", SIGSEGV, 0);
		pa = va;
	}
	return pa;
}

static u8
rd8(u32 a)
{
	return *(volatile u8 *)gpa(a, 0, 1);
}

static void
wr8(u32 a, u8 v)
{
	*(volatile u8 *)gpa(a, 1, 1) = v;
}

/* accesses that may cross a page (256 bytes, the smallest) go bytewise */
static u16
rd16(u32 a)
{
	if ((a & 0xff) == 0xff)
		return (u16)(rd8(a) << 8 | rd8(a + 1));
	return *(volatile u16 *)gpa(a, 0, 2);
}

static u32
rd32(u32 a)
{
	if ((a & 0xff) > 0xfc)
		return (u32)rd16(a) << 16 | rd16(a + 2);
	return *(volatile u32 *)gpa(a, 0, 4);
}

static void
wr16(u32 a, u16 v)
{
	if ((a & 0xff) == 0xff) {
		wr8(a, (u8)(v >> 8));
		wr8(a + 1, (u8)v);
	} else
		*(volatile u16 *)gpa(a, 1, 2) = v;
}

static void
wr32(u32 a, u32 v)
{
	if ((a & 0xff) > 0xfc) {
		wr16(a, (u16)(v >> 16));
		wr16(a + 2, (u16)v);
	} else
		*(volatile u32 *)gpa(a, 1, 4) = v;
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
 * 68020 full extension word: base displacement, optional index, optional
 * memory indirection (pre- or post-indexed) and outer displacement.
 * base: An, or the extension word's address (PC relative).
 */
static int
ea_full(struct excregs *r, u32 base, u16 ext, u32 *pc, u32 *addr)
{
	u32 bd = 0, od = 0, xn = 0, is = ext & 7;

	*pc += 2;
	if (ext & 0x0008)
		return FAULT;
	if (ext & 0x0080)			/* BS: base suppressed */
		base = 0;
	if (!(ext & 0x0040))			/* IS = 0: index used */
		xn = index_value(r, ext);
	else if (is > 3)
		return FAULT;
	switch ((ext >> 4) & 3) {		/* base displacement size */
	case 0:
		return FAULT;
	case 2:
		bd = (u32)(s32)(s16)rd16(*pc);
		*pc += 2;
		break;
	case 3:
		bd = rd32(*pc);
		*pc += 4;
		break;
	}
	switch (is & 3) {			/* outer displacement size */
	case 2:
		od = (u32)(s32)(s16)rd16(*pc);
		*pc += 2;
		break;
	case 3:
		od = rd32(*pc);
		*pc += 4;
		break;
	}
	if (is == 0)				/* no memory indirection */
		*addr = base + bd + xn;
	else if (is == 4)
		return FAULT;
	else if (is < 4)			/* pre-indexed */
		*addr = rd32(base + bd + xn) + od;
	else					/* post-indexed */
		*addr = rd32(base + bd) + xn + od;
	return 0;
}

/*
 * Decode mode/reg; *pc points to the next extension word and is advanced.
 * Pre-decrement and post-increment are applied. Returns 0 or FAULT
 * (unsupported mode).
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
			return ea_full(r, *an, ext, pc, &e->addr);
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
			return ea_full(r, *pc, ext, pc, &e->addr);
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
	return rd8(e->addr);
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
		wr8(e->addr, (u8)v);
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
reflect_frame(struct excregs *r, u32 vec, const u8 *frame, u32 fmt, int ipl)
{
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
	if (ipl < 0 && nrlog < NRLOG) {
		rlog[nrlog].vec = vec;
		rlog[nrlog].pc = rd32(sp + 2);
		rlog[nrlog].sr = old;
		rlog[nrlog].vbr = guest.vbr;
		rlog[nrlog++].handler = rd32(guest.vbr + vec * 4);
	}
	r->pc = rd32(guest.vbr + vec * 4);
	guest.n_reflect++;
}

/* copy_frame: the real frame's format and extra words, else format 0 */
static void
reflect(struct excregs *r, u32 vec, int copy_frame, int ipl)
{
	reflect_frame(r, vec, (const u8 *)&r->sr,
	    copy_frame ? r->fmtvec >> 12 : 0, ipl);
}

/*
 * Bus error for an emulated instruction whose access failed: a 68030
 * short bus fault frame (format A) for a data fault, marked so that the
 * guest's RTE restarts the instruction (emulated again) instead of handing
 * the frame to the CPU.
 */
#define AF_MAGIC1	0x5058			/* internal register words */
#define AF_MAGIC2	0x534f

static void
access_fault(struct excregs *r)
{
	u16 f[30];
	u32 sz = afault.size == 1 ? 0x10 : afault.size == 2 ? 0x20 : 0;

	memset(f, 0, sizeof(f));
	if (cpuinfo.model == 40) {
		/*
		 * 68040 access error frame (format 7): EA and fault address,
		 * SSW with ATC, read/write, size and the function code, no
		 * write-backs; marked so that the guest's RTE restarts the
		 * instruction here instead of going to the CPU.
		 */
		f[4] = f[10] = (u16)(afault.va >> 16);
		f[5] = f[11] = (u16)afault.va;
		f[6] = (u16)(0x0400 | (afault.write ? 0 : 0x0100) |
		    (afault.size == 1 ? 0x20 : afault.size == 2 ? 0x40 : 0) |
		    (afault.fc & 7));
		f[26] = AF_MAGIC1;		/* PD2, +0x34 */
		f[27] = AF_MAGIC2;
		afault.faulted = 0;
		reflect_frame(r, VEC_BUSERR, (const u8 *)f, 7, -1);
		return;
	}
	if (cpuinfo.model == 60) {
		/*
		 * 68060 access error frame (format 4): fault address, FSLW
		 * with read/write, size, user/supervisor data and PF; its
		 * RTE restarts the instruction anyway.
		 */
		u32 fslw = (afault.write ? 0x00800000 : 0x01000000) |
		    (afault.size == 1 ? 0x00200000 : afault.size == 2 ?
		    0x00400000 : 0) | ((afault.fc & 7) << 16) | 0x00000200;

		f[4] = (u16)(afault.va >> 16);
		f[5] = (u16)afault.va;
		f[6] = (u16)(fslw >> 16);
		f[7] = (u16)fslw;
		afault.faulted = 0;
		reflect_frame(r, VEC_BUSERR, (const u8 *)f, 4, -1);
		return;
	}
	f[4] = AF_MAGIC1;
	f[5] = (u16)(0x0100 | (afault.write ? 0 : 0x0040) | sz | afault.fc);
	f[6] = rd16(r->pc);			/* stage C: the opcode */
	f[8] = (u16)(afault.va >> 16);
	f[9] = (u16)afault.va;
	f[10] = AF_MAGIC2;
	afault.faulted = 0;
	reflect_frame(r, VEC_BUSERR, (const u8 *)f, 0xa, -1);
}

/* ------------------------------------------------------------------ */
/* Emulation                                                           */

static void poll_serial(struct excregs *r);

/*
 * Break-in from the Amiga keyboard: Ctrl + Left Amiga + Help. The keyboard
 * belongs to the guest; on each level 2 interrupt SORIX-LDOM only looks
 * at CIA-A's serial data register (reading it clears nothing), so the
 * guest still gets every key.
 */
#define CIAA_SDR	0xbfec01
#define KEY_CTRL	0x63
#define KEY_LAMIGA	0x66
#define KEY_HELP	0x5f

static void
kbd_snoop(struct excregs *r)
{
	static u8 last;
	static int ctrl, lamiga;
	u8 x = (u8)~*(volatile u8 *)CIAA_SDR, code;
	int up;

	if (x == last)
		return;				/* nothing new shifted in */
	last = x;
	code = (u8)(x >> 1 | x << 7);
	up = code & 0x80;
	code &= 0x7f;
	if (code == KEY_CTRL)
		ctrl = !up;
	else if (code == KEY_LAMIGA)
		lamiga = !up;
	else if (code == KEY_HELP && !up && ctrl && lamiga) {
		ctrl = lamiga = 0;
		guest.kbd_break = 1;
		stop_guest(r, "stopped (Ctrl-Amiga-Help)", SIGINT, 0);
	}
}

/*
 * The guest waits for an interrupt (STOP, HC_IDLE): stop the real CPU at
 * the guest's interrupt level until an interrupt it has not masked
 * arrives, then deliver that interrupt. Serial input is SORIX-LDOM's
 * (Ctrl-C breaks in); the CPU goes back to sleep after it. With all
 * levels masked nothing could wake it, so the guest simply continues.
 */
static void
idle(struct excregs *r)
{
	u32 level = (guest.vsr & VSR_IPL) >> 8, vec, s0, ms0, s1, ms1;

	if (level >= 7)
		return;
	time_now(&s0, &ms0);
	for (;;) {
		vec = idle_stop(level);
		guest.n_idle++;
		if (vec == 29 && (REG16(CUSTOM_INTREQR) & 0x0800) &&
		    !(REG16(CUSTOM_INTREQR) & 0x1000)) {
			poll_serial(r);		/* may stop the guest */
			continue;
		}
		break;
	}
	time_now(&s1, &ms1);
	guest.idle_ms += (s1 - s0) * 1000 + ms1 - ms0;
	if (vec == 26)
		kbd_snoop(r);
	if (vec >= 25 && vec <= 31) {
		guest.n_irq++;
		reflect(r, vec, 0, (int)(vec - 24));
	}
}

static u32 guest_events(void);
static u32 crp_next = 1, crp_events;	/* the instruction after PMOVE CRP / MOVEC URP */

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
		if (m >= 40 && cr == 0x806) {	/* new user address space */
			vmmu_flush_user();
			crp_next = *pc;
			crp_events = guest_events();
		} else if (m >= 40 && (cr == 0x003 || (cr >= 0x004 &&
		    cr <= 0x007) || cr == 0x807))
			vmmu_flush();		/* new MMU state: TC, TTs, SRP */
		if (cr == 0x808) {
			/*
			 * 68060 PCR: the guest's FPU disable (DFP, bit 1) and
			 * superscalar enable (ESS, bit 0) go to the real PCR,
			 * as its FP instructions run natively; the ID and
			 * revision stay the CPU's.
			 */
			guest.pcr = (cpuinfo.pcr & ~3u) | (*rn & 3);
			pcr_write(guest.pcr);
		}
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
			if (p == guest.crp) {	/* new user address space */
				vmmu_flush_user();
				crp_next = *pc;
				crp_events = guest_events();
			} else
				vmmu_flush();	/* new tables or geometry */
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
		if (((ext & 0x1c00) == 0x1800 ||	/* PFLUSH fc,mask,<ea> */
		    (ext & 0x1c00) == 0x0000) &&	/* PLOAD fc,<ea> */
		    ea_decode(r, mode, regno, 4, pc, &e))
			return FAULT;
		/*
		 * PFLUSH: all of it, except right after a PMOVE to CRP (AMIX's
		 * hat_asload: pmove crp; pflusha), which already rebuilt the
		 * only view that changed.
		 */
		if ((ext & 0x1c00) && !(r->pc == crp_next &&
		    guest_events() == crp_events + 1))
			vmmu_pflush();
		crp_next = 1;
		return 0;
	case 0x8000: {				/* PTEST */
		u32 fc, desc;

		if (ea_decode(r, mode, regno, 4, pc, &e) || e.is_reg ||
		    e.is_imm)
			return FAULT;
		switch (ext & 0x18) {
		case 0x00: fc = ext & 1 ? guest.dfc : guest.sfc; break;
		case 0x08: fc = r->d[ext & 7]; break;
		default:   fc = ext & 7; break;	/* immediate */
		}
		guest.mmusr = vmmu_ptest(e.addr, fc & 7, !(ext & 0x0200),
		    (ext >> 10) & 7, &desc);
		if (ext & 0x0100)
			r->a[(ext >> 5) & 7] = desc;
		return 0;
	}
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
	/* the access goes to the address space DFC (write) or SFC selects */
	as_super = ((ext & 0x0800 ? guest.dfc : guest.sfc) & 4) != 0;
	if (ext & 0x0800)
		ea_write(&e, size, *rn);
	else if (ext & 0x8000)
		*rn = size == 4 ? ea_read(&e, size) :
		    (u32)(size == 2 ? (s32)(s16)ea_read(&e, 2) :
		    (s32)(s8)ea_read(&e, 1));
	else
		ea_write(&(struct ea){ .is_reg = 1, .reg = rn }, size,
		    ea_read(&e, size));
	as_super = -1;
	return 0;
}

/* guest exceptions so far: no guest instruction ran if it is unchanged */
static u32
guest_events(void)
{
	return guest.n_emul + guest.n_reflect + guest.n_hcall + guest.n_irq;
}

/*
 * FSAVE/FRESTORE for the guest, executed on the real FPU.
 *
 * An FSAVE right after a null-frame FRESTORE saves a null frame on a
 * 68881/68882 (the FPU is in its reset state); FS-UAE's 68882 saves an
 * idle frame instead, which overruns AMIX's 8-byte reset_fsave in
 * chk_fpu. The documented result is produced when no guest instruction
 * can have run in between.
 */
static int
fsave_frestore(struct excregs *r, u16 op, u32 *pc)
{
	static u8 buf[256] __attribute__((aligned(4)));
	static u32 null_pc, null_events;
	u32 mode = (op >> 3) & 7, regno = op & 7, n, i;
	struct ea e;

	if (cpuinfo.fpu < 0 && cpuinfo.model != 60)
		return FAULT;
	if (!(op & 0x0040)) {			/* FSAVE <ea>: -(An) or control */
		if (r->pc == null_pc && guest_events() == null_events + 1) {
			memset(buf, 0, 4);
			n = 4;			/* still in the reset state */
		} else
			n = fpu_save(buf);
		if (cpuinfo.model == 60)
			n = 12;			/* 68060 frames: always 12 bytes */
		if (mode == 4) {
			r->a[regno] -= n;
			e.addr = r->a[regno];
		} else if (ea_decode(r, mode, regno, 4, pc, &e) || e.is_reg)
			return FAULT;
		for (i = 0; i < n; i++)
			wr8(e.addr + i, buf[i]);
		return 0;
	}
	/* FRESTORE <ea>: (An)+ or control */
	if (mode == 3)
		e.addr = r->a[regno];
	else if (ea_decode(r, mode, regno, 4, pc, &e) || e.is_reg)
		return FAULT;
	n = cpuinfo.model == 60 ? 12 : (u32)rd8(e.addr + 1) + 4;
	if (n > sizeof(buf))
		return FAULT;
	for (i = 0; i < n; i++)
		buf[i] = rd8(e.addr + i);
	if (afault.faulted)
		return 0;
	fpu_restore(buf);
	if (mode == 3)
		r->a[regno] += n;
	null_pc = cpuinfo.model != 60 && buf[0] == 0 ? *pc : 1;	/* null */
	null_events = guest_events();
	return 0;
}

static void save_ctx(const struct excregs *r);

/*
 * RTE from virtual supervisor mode: pop a format 0/2 frame; a 68030 bus
 * error frame (A, B) goes back to the CPU itself, which continues or
 * reruns the faulted access.
 */
static int
rte(struct excregs *r)
{
	static u16 fr[46];
	u32 sp = r->a[7], i;
	u16 sr = rd16(sp), fv = rd16(sp + 6);
	u32 pc = rd32(sp + 2), fmt = fv >> 12, size = frame_size(fmt);

	if ((fmt == 0xa && rd16(sp + 8) == AF_MAGIC1 &&
	    rd16(sp + 0x14) == AF_MAGIC2) || (fmt == 7 &&
	    rd16(sp + 0x34) == AF_MAGIC1 && rd16(sp + 0x36) == AF_MAGIC2)) {
		r->a[7] = sp + size;		/* ours: restart the instruction */
		set_vsr(r, sr);
		r->pc = pc;
		return 0;
	}
	if (fmt == 0xa || fmt == 0xb || fmt == 7) {
		if ((cpuinfo.model == 30) != (fmt != 7))
			return FAULT;
		for (i = 0; i < size; i += 2)
			fr[i / 2] = rd16(sp + i);
		r->a[7] = sp + size;
		set_vsr(r, sr);
		r->pc = pc;
		if (afault.faulted)
			return 0;		/* frame not readable */
		fr[0] = real_sr(r);
		fr[fmt == 7 ? 6 : 5] &= (u16)~4;	/* a user cycle again */
		if (fmt == 7)
			fr[7] &= (u16)~4, fr[8] &= (u16)~4, fr[9] &= (u16)~4;
		save_ctx(r);
		guest.n_emul++;
		vmmu_activate();
		vcpu_enter_frame(&guest.ctx, fr, size, sstack_top());
	}
	if (fmt != 0 && fmt != 2 && fmt != 4) {	/* 4: 68060 restarts */
		kprintf("vcpu: RTE with frame format %x not supported yet\n",
		    fmt);
		return FAULT;
	}
	r->a[7] = sp + size;
	set_vsr(r, sr);
	r->pc = pc;
	return 0;
}

/* ------------------------------------------------------------------ */
/* spl sites patched to line A                                         */

/*
 * The guest kernel's SR moves (its spl sequences) trap on every
 * execution. Once one has trapped in the guest's kernel text, it is
 * rewritten in place as a line A opcode that the line A handler executes
 * with less work, a pair as one:
 *
 *	A10n	move sr,dn
 *	A11n	move dn,sr
 *	A120	move #imm,sr		(imm follows)
 *	A13n	move sr,dn; move #imm,sr
 *
 * Only code the CPU has just executed is patched (no guessing what is an
 * instruction), only in the kernel's text, and only where it is identity
 * mapped (written through the physical address).
 */
u32 spl_lo, spl_hi;			/* kernel text, 0 = no patching */
static u32 n_spl_patched, n_aline;
u32 aline_fast;				/* core/vectors.S: guest running */
u32 aline_count;			/* ... sites executed there */
u16 *const vcpu_vsr = &guest.vsr;

void
vcpu_spl_patching(u32 text, u32 size)
{
	spl_lo = text;
	spl_hi = text + size;
	n_spl_patched = 0;
}

static void
spl_patch(u32 pc, u16 op, u16 next)
{
	u32 pa;
	u16 a;

	if (!spl_lo || pc < spl_lo || pc + 6 > spl_hi ||
	    vmmu_translate(pc, 1, 0, &pa) || pa != pc)
		return;
	if ((op & 0xfff8) == 0x40c0)
		a = (u16)(next == 0x46fc ? 0xa130 : 0xa100) | (op & 7);
	else if ((op & 0xfff8) == 0x46c0)
		a = 0xa110 | (op & 7);
	else if (op == 0x46fc)
		a = 0xa120;
	else
		return;
	*(volatile u16 *)(uintptr_t)pc = a;
	icache_inval(cpuinfo.model);
	n_spl_patched++;
}

/* A line A trap: one of ours (1, done) or the guest's (0) */
static int
aline(struct excregs *r)
{
	u16 op = rd16(r->pc);
	u32 *dn = &r->d[op & 7];

	if (!spl_lo || r->pc < spl_lo || r->pc >= spl_hi ||
	    (op & 0xffc0) != 0xa100 || !(guest.vsr & VSR_S))
		return 0;
	n_aline++;
	switch (op & 0xfff0) {
	case 0xa100:				/* move sr,dn */
		*dn = (*dn & 0xffff0000) | cur_vsr(r);
		r->pc += 2;
		return 1;
	case 0xa110:				/* move dn,sr */
		set_vsr(r, (u16)*dn);
		r->pc += 2;
		return 1;
	case 0xa120:				/* move #imm,sr */
		if (op & 0xf)
			return 0;
		set_vsr(r, rd16(r->pc + 2));
		r->pc += 4;
		return 1;
	case 0xa130:				/* move sr,dn; move #imm,sr */
		*dn = (*dn & 0xffff0000) | cur_vsr(r);
		set_vsr(r, rd16(r->pc + 4));
		r->pc += 6;
		return 1;
	}
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
	prof[(op & 0xfff8) == 0x40c0 ? P_SR_TO_DN :
	    (op & 0xffc0) == 0x40c0 ? P_SR_TO_MEM :
	    (op & 0xfff8) == 0x46c0 ? P_DN_TO_SR : op == 0x46fc ? P_IMM_TO_SR :
	    (op & 0xffc0) == 0x46c0 ? P_MEM_TO_SR :
	    op == 0x027c || op == 0x007c || op == 0x0a7c ? P_SR_LOGIC :
	    op == 0x4e73 ? P_RTE : (op & 0xfffe) == 0x4e7a ? P_MOVEC :
	    (op & 0xfff0) == 0x4e60 ? P_USP : op == 0x4e72 ? P_STOP :
	    (op & 0xff00) == 0x0e00 ? P_MOVES :
	    (op & 0xffc0) == 0xf000 ? ((rd16(pc) & 0xe000) == 0x2000 ? P_PFLUSH :
	    (rd16(pc) & 0xe000) == 0x8000 ? P_PTEST : (rd16(pc) & 0x0200) ||
	    (rd16(pc) & 0xe000) == 0x6000 ? P_PMOVE_REG : P_PMOVE_MMU) :
	    (op & 0xff80) == 0xf300 ? P_FPU : (op & 0xff00) == 0xf400 ? P_CACHE :
	    (op & 0xffe0) == 0xf500 ? P_PFLUSH : P_OTHER]++;
	if ((op & 0xfff8) == 0x40c0 && rd16(pc) == 0x46fc)
		prof[P_SPL_PAIR]++;
	if ((op & 0xfff8) == 0x40c0 || (op & 0xfff8) == 0x46c0 || op == 0x46fc)
		spl_patch(r->pc, op, rd16(pc));
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
	} else if (op == 0x4e72) {			/* STOP #imm: wait */
		set_vsr(r, rd16(pc));
		r->pc = pc + 2;
		guest.n_emul++;
		idle(r);
		return 0;
	} else if (op == 0x4e70) {		/* RESET: the guest reboots */
		guest.reboot = 1;
		guest.active = 0;
		r->pc = pc;
		stop_guest(r, "reset (reboot)", 0, 0);
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
		/* PFLUSH (An), PFLUSHN, PFLUSHA; right after MOVEC URP (a
		 * context switch) the user view is new already */
		if (!(r->pc == crp_next && guest_events() == crp_events + 1))
			vmmu_pflush();
		crp_next = 1;
	} else if ((op & 0xffd8) == 0xf548 && cpuinfo.model == 40) {
		guest.mmusr40 = 0;			/* PTEST */
	} else if ((op & 0xffb8) == 0xf588 && cpuinfo.model == 60) {
		/* PLPAR/PLPAW (An): the guest's physical address, in An */
		u32 pa, *an = &r->a[op & 7];
		int sup = (guest.dfc & 4) != 0;

		if (vmmu_translate(*an, sup, !(op & 0x0040), &pa))
			return FAULT;
		*an = pa;
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
save_ctx(const struct excregs *r)
{
	u32 i;

	for (i = 0; i < 8; i++) {
		guest.ctx.d[i] = r->d[i];
		guest.ctx.a[i] = r->a[i];
	}
	guest.ctx.sr = real_sr(r);
	guest.ctx.pc = r->pc;
}

/*
 * Stop the guest and go to the shell (or back to GDB if it is attached);
 * its registers are kept for resume.
 */
static void
stop_guest(struct excregs *r, const char *why, u32 sig, int swbreak)
{
	save_ctx(r);
	guest.running = 0;
	aline_fast = 0;
	guest.stop_sig = sig;
	guest.stop_swbreak = swbreak;
	gdb_step = 0;
	REG16(CUSTOM_INTENA) = 0x0800;		/* serial receive interrupt off */
	con_interactive(0);
	kprintf("vcpu: %s %s at PC %08x, SR %04x\n", guest.name, why, r->pc,
	    cur_vsr(r));
	restart(sstack_top(), shell_main);
}

/*
 * HC_COPYIN/HC_COPYOUT: copy between the guest's user and kernel views,
 * up to a page at a time. A0, A1, D1 advance as it goes; an address the
 * guest's tables refuse gives it a bus error at the TRAP (restarted after
 * its RTE, so the copy resumes once the page is in).
 */
static void
hc_copy(struct excregs *r, int out)
{
	u32 src = r->a[0], dst = r->a[1], n = r->d[1], sp, dp, chunk, pg;
	int bad;

	pg = guest.tc & 0x80000000 ? 1u << ((guest.tc >> 20) & 15) : 4096;
	while (n) {
		bad = 0;
		if (vmmu_translate(src, out, 0, &sp))
			bad = 1;
		else if (vmmu_translate(dst, !out, 1, &dp))
			bad = 2;
		if (bad) {
			afault.va = bad == 1 ? src : dst;
			afault.write = bad == 2;
			afault.size = 1;
			afault.fc = (bad == 1) == !out ? 1 : 5;
			r->pc -= 2;			/* the TRAP, to restart */
			access_fault(r);
			return;
		}
		chunk = n;
		if (chunk > pg - (src & (pg - 1)))
			chunk = pg - (src & (pg - 1));
		if (chunk > pg - (dst & (pg - 1)))
			chunk = pg - (dst & (pg - 1));
		memcpy((void *)(uintptr_t)dp, (const void *)(uintptr_t)sp, chunk);
		src += chunk;
		dst += chunk;
		n -= chunk;
		r->a[0] = src;
		r->a[1] = dst;
		r->d[1] = n;
	}
	guest.sfc = guest.dfc = 1;		/* as the MOVES loops left them */
	r->d[0] = 0;
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
		if (gdb_active) {
			u8 c8 = (u8)r->d[1];

			gdb_console(&c8, 1);
		} else
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
		idle(r);
		break;
	case HC_WRITE:
		if (gdb_active)
			gdb_console((const u8 *)(uintptr_t)r->a[0], r->d[1]);
		else
			for (i = 0; i < r->d[1]; i++)
				con_putc(rd8(r->a[0] + i));
		break;
	case HC_COPYIN:
	case HC_COPYOUT:
		hc_copy(r, r->d[0] == HC_COPYOUT);
		break;
	case HC_HALT:
		guest.reboot = r->d[1] != 0;
		guest.active = 0;
		guest.kbd_break = !guest.reboot;	/* halted: console on screen */
		stop_guest(r, guest.reboot ? "rebooting" : "halted", 0, 0);
	case HC_EXIT:
		guest.exit_code = (int)r->d[1];
		guest.active = 0;
		stop_guest(r, "exited", 0, 0);
	case HC_BREAK:
		stop_guest(r, "stopped (break)", SIGTRAP, 0);
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
			stop_guest(r, "stopped (Ctrl-C)", SIGINT, 0);
		if (rxlen < sizeof(rxbuf))
			rxbuf[(rxhead + rxlen++) % sizeof(rxbuf)] = (u8)c;
	}
}

/*
 * Injected call (vcpu_inject): what the CPU does for an interrupt, with the
 * handler given: a format 0 frame on the guest's supervisor stack, virtual
 * S and IPL 7, then pc. The handler's RTE returns where the guest was.
 */
static void
inject(struct excregs *r)
{
	u16 old = cur_vsr(r);
	u32 sp;

	set_vsr(r, (u16)((old | VSR_S | VSR_IPL) & ~(VSR_T1 | VSR_T0)));
	sp = r->a[7] - 8;
	wr16(sp, old);
	wr32(sp + 2, r->pc);
	wr16(sp + 6, 0);
	r->a[7] = sp;
	r->pc = guest.inject_pc;
	guest.inject_pc = 0;
}

void
vcpu_inject(u32 pc)
{
	guest.inject_pc = pc;
}

void
vcpu_exception(struct excregs *r)
{
	u32 vec = (r->fmtvec & 0xfff) >> 2;
	int k;

	cur_regs = r;
	in_emul = 0;			/* an emulation that stopped the guest */
	afault.faulted = 0;
	poll_serial(r);
	{
		u32 o, n;

		if (vmmu_watch_check(&o, &n)) {
			kprintf("vcpu: watched long changed %08x -> %08x without a "
			    "guest write (SDMAC ACR %08x, WTC %08x)\n", o, n,
			    *(volatile u32 *)0xdd000c, *(volatile u32 *)0xdd0004);
			stop_guest(r, "stopped (watch)", SIGTRAP, 0);
		}
	}
	if (vec == 9 && vmmu_watch_step()) {		/* watchpoint step done */
		r->sr = real_sr(r);
		return;
	}
	if (vec == 9 && gdb_step)			/* GDB single-step */
		stop_guest(r, "stepped", SIGTRAP, 0);
	if (vec == VEC_ILLEGAL && rd16(r->pc) == 0x4afc &&
	    gdb_breakpoint_at(r->pc))			/* GDB breakpoint */
		stop_guest(r, "at breakpoint", SIGTRAP, 1);
	if (vec >= 25 && vec <= 31) {			/* autovector interrupt */
		if (vec == 29 && !(REG16(CUSTOM_INTREQR) & 0x1000)) {
			r->sr = real_sr(r);		/* serial receive: ours */
			return;
		}
		if (vec == 26)
			kbd_snoop(r);
		guest.n_irq++;
		reflect(r, vec, 0, (int)(vec - 24));
	} else if (vec == VEC_PRIV && (guest.vsr & VSR_S)) {
		struct excregs save = *r;
		u32 vsr = guest.vsr, usp = guest.usp, isp = guest.isp;
		u32 msp = guest.msp;

		in_emul = 1;
		k = emulate(r);
		in_emul = 0;
		as_super = -1;
		if (afault.faulted) {		/* undo, then bus error */
			*r = save;
			guest.vsr = (u16)vsr;
			guest.usp = usp;
			guest.isp = isp;
			guest.msp = msp;
			access_fault(r);
		} else if (k)
			reflect(r, VEC_ILLEGAL, 0, -1);
	} else if (vec == VEC_TRAP15 && (guest.vsr & VSR_S)) {
		hypercall(r);				/* PC is past the TRAP */
	} else if (vec == VEC_LINEA && aline(r)) {
		/* a patched spl site */
	} else if (vec == VEC_BUSERR && (k = vmmu_fault(r)) >= 0) {
		r->sr = real_sr(r);			/* shadow miss: rerun */
		if (k)
			r->sr |= VSR_T1;		/* watchpoint: one step */
		return;
	} else if (vec == VEC_BUSERR && ((r->fmtvec >> 12) == 0xa ||
	    (r->fmtvec >> 12) == 0xb || (r->fmtvec >> 12) == 4 ||
	    (r->fmtvec >> 12) == 7)) {
		/*
		 * The guest sees the function code of its own mode: the
		 * CPU ran it in user mode, its kernel runs with virtual S
		 * (68030 SSW FC bit 2; 68060 FSLW TM bit 2, bit 18).
		 */
		if ((guest.vsr & VSR_S) && (r->fmtvec >> 12) == 4)
			*(u32 *)((u8 *)&r->sr + 0x0c) |= 0x00040000;
		else if ((guest.vsr & VSR_S) && (r->fmtvec >> 12) == 7) {
			u16 *f7 = (u16 *)&r->sr;

			f7[6] |= 4;		/* SSW TM: supervisor */
			for (k = 7; k <= 9; k++)	/* WB3S, WB2S, WB1S TM */
				if (f7[k] & 0x80)
					f7[k] |= 4;
		}
		else if (guest.vsr & VSR_S)
			*((u16 *)&r->sr + 5) |= 4;	/* SSW FC: supervisor */
		reflect(r, vec, 1, -1);
		/*
		 * Leave through a fresh frame: an RTE of the real bus error
		 * frame would make the CPU rerun or continue the faulted
		 * access. The guest's RTE of its copy does that later.
		 */
		guest.ctx.sr = 0;
		save_ctx(r);
		if (gdb_step)
			stop_guest(r, "stepped", SIGTRAP, 0);
		vmmu_activate();
		vcpu_enter(&guest.ctx, sstack_top());
	} else
		reflect(r, vec, 1, -1);
	if (gdb_step)		/* an emulated or reflected instruction: one step */
		stop_guest(r, "stepped", SIGTRAP, 0);
	if (guest.inject_pc && !(guest.vsr & VSR_IPL))
		inject(r);			/* not inside a critical section */
	r->sr = real_sr(r);
	vmmu_activate();			/* view of the new virtual S */
}

/* ------------------------------------------------------------------ */
/* Starting, resuming                                                  */

void
vcpu_prepare(const char *name, u32 pc, u32 ssp, u32 d0)
{
	memset(&guest, 0, sizeof(guest));
	memset(prof, 0, sizeof(prof));
	spl_lo = spl_hi = 0;			/* the AMIX loader turns it on */
	n_aline = aline_count = aline_fast = 0;
	rxhead = rxlen = nrlog = 0;
	vmmu_reset();
	guest.ctx.d[0] = d0;
	guest.name = name;
	guest.active = 1;
	guest.vsr = 0x2700;
	guest.ctx.a[7] = ssp;			/* virtual S = 1: real USP = ISP */
	guest.ctx.sr = real_sr(&(struct excregs){ .sr = 0 });
	guest.ctx.pc = pc;
	guest.stop_sig = SIGTRAP;
	guest.pcr = cpuinfo.pcr;		/* 68060: as the CPU reports it */
}

void
vcpu_start(const char *name, u32 pc, u32 ssp, u32 d0)
{
	vcpu_prepare(name, pc, ssp, d0);
	kprintf("vcpu: starting %s at %08x (deprivileged, virtual SR 2700)\n",
	    name, pc);
	vcpu_resume();
}

void
vcpu_set_ps(u16 sr)
{
	struct excregs r;
	u32 i;

	for (i = 0; i < 8; i++) {
		r.d[i] = guest.ctx.d[i];
		r.a[i] = guest.ctx.a[i];
	}
	r.sr = guest.ctx.sr;
	set_vsr(&r, sr);
	guest.ctx.a[7] = r.a[7];
	guest.ctx.sr = real_sr(&r);
}

void
vcpu_resume(void)
{
	if (!guest.active) {
		kprintf("vcpu: no guest to resume\n");
		restart(sstack_top(), shell_main);
	}
	guest.running = 1;
	guest.ctx.sr &= (u16)~VSR_T1;
	if (gdb_step || (guest.vsr & VSR_T1))
		guest.ctx.sr |= VSR_T1;			/* trace */
	con_interactive(0);
	REG16(CUSTOM_INTENA) = 0xc800;		/* master + serial receive */
	cur_regs = NULL;
	aline_fast = spl_lo != 0;
	vmmu_activate();
	vcpu_enter(&guest.ctx, sstack_top());
}

void
vcpu_stats(void)
{
	u32 i, t = 0;

	for (i = 0; i < P_SPL_PAIR; i++)
		t += prof[i];
	kprintf("emulated instructions: %u\n", t);
	for (i = 0; i < P_N; i++)
		if (prof[i])
			kprintf("  %-22s %9u  %3u%%\n", pnames[i], prof[i],
			    t >= 100 ? prof[i] / (t / 100) : 0);
	kprintf("reflected %u, interrupts %u, hypercalls %u, idle wake-ups %u\n",
	    guest.n_reflect, guest.n_irq, guest.n_hcall, guest.n_idle);
	kprintf("spl sites patched %u, executed through line A %u (fast "
	    "path %u)\n", n_spl_patched, n_aline, aline_count);
	vmmu_show();
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
	kprintf("  idle: %u wake-ups, %u ms waiting in STOP\n", guest.n_idle,
	    guest.idle_ms);
	for (i = 0; i < (int)nrlog; i++)
		kprintf("  reflected: %s (vector %u) at PC %08x, SR %04x -> "
		    "VBR %08x handler %08x\n", exc_name(rlog[i].vec), rlog[i].vec,
		    rlog[i].pc, rlog[i].sr, rlog[i].vbr, rlog[i].handler);
	vmmu_show();
}
