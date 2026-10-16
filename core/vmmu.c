/*
 * Shadow MMU for the guest (68030): see include/pvmk/vmmu.h.
 */
#include "pvmk/bootcap.h"
#include "pvmk/kprintf.h"
#include "pvmk/vcpu.h"
#include "pvmk/vmmu.h"

/* 68030 TC */
#define TC_E		0x80000000
#define TC_SRE		0x02000000
#define TC_FCL		0x01000000

/* descriptor bits */
#define D_DT		0x00000003	/* 0 invalid, 1 page, 2 short, 3 long */
#define D_WP		0x00000004
#define D_U		0x00000008
#define D_M		0x00000010
#define D_CI		0x00000040
#define D_S		0x00000100	/* long descriptors */
#define DT_PAGE		1
#define DT_SHORT	2
#define DT_LONG		3

/*
 * Shadow geometry, by CPU family (set in vmmu_reset):
 *  68030: TIA 8, TIB 8, TIC 5, 2 KB pages, short descriptors;
 *         TC = E | PS 2K | IS 0 | 8/8/5. SORIX-ILOM-LDOM: TT0 = everything for
 *         supervisor function codes.
 *  68040/68060: 7/7/6, 4 KB pages (TC = E). SORIX-ILOM-LDOM: supervisor-only
 *         transparent translation, DTT0 the low 16 MB (chip RAM, I/O)
 *         non-cacheable, DTT1 and ITT0 everything else cacheable
 *         (write-through), so its accesses stay coherent with the guest's
 *         cached data (the caches are physical).
 * The descriptor bits used are the same in both formats: table 2, page 1,
 * WP/W bit 2, U bit 3, M bit 4, no-cache 0x40 (CI / CM precise).
 */
#define SH_TC		0x80b08850
#define SH_TT0		0x00ff8143	/* all addresses, FC 4-7, R and W */
#define SH_TC40		0x00008000	/* E, 4 KB pages */
#define SH_DTT0_40	0x0000a040	/* 00xxxxxx, supervisor, precise */
#define SH_DTT1_40	0x00ffa000	/* all, supervisor, write-through */
#define SH_ITT0_40	0x00ffa000

static u32 sh_page, sh_n1, sh_n2, sh_n3;	/* page size, entries per level */
static u32 sh_s1, sh_s2, sh_s3;		/* index shifts */
static u32 sh_al1, sh_al3;		/* table alignment (L1/L2, L3) */
#define SH_PAGE		sh_page
#define SH_L1		sh_n1
#define SH_L2		sh_n2
#define SH_L3		sh_n3
#define I1(va)		((va) >> sh_s1)
#define I2(va)		(((va) >> sh_s2) & (sh_n2 - 1))
#define I3(va)		(((va) >> sh_s3) & (sh_n3 - 1))

#define POOL_SIZE	(64 * 1024)

static u8 pool[POOL_SIZE] __attribute__((aligned(512)));
static u32 pool_used;

/*
 * Revalidation instead of rebuilding (guest PFLUSH). Every shadow page
 * table (L3) carries what its entries were made from: the upper guest
 * descriptors (the same for all its entries, else it is not kept) and,
 * per entry, the guest page descriptor and its value. Entries go in
 * without U, so the CPU marks those it uses. At a guest PFLUSH the L3
 * tables are checked: entries used since the last PFLUSH whose guest
 * descriptors are unchanged stay (U cleared again), the others go, and an
 * L3 left empty is recycled. Cheap compares for the working set instead
 * of a bus error per page after every flush.
 */
#define NL3		192
#define MAXPATH		4		/* guest descriptors per walk kept */
struct l3meta {
	u32 *l3;
	u32 *l2e;			/* the L2 entry pointing to it */
	u8 view, uniform, nup, pad;
	u32 up_a[MAXPATH - 1], up_v[MAXPATH - 1];
	u32 pd_a[64], pd_v[64];		/* 0: made without a guest table */
};
#define L3SIZE		((SH_L3 * 4 + sizeof(struct l3meta) + 15) & ~15u)	/* runtime */
static struct l3meta *l3list[NL3];
static u32 nl3;
static u32 *l3free;			/* recycled L3 blocks */
static u32 n_kept, n_dropped, n_pflush;
static void l3_release(u32 i);
static int fault040(struct excregs *r);
static int next_page(u32 fa, int write);

static u32
dsig(u32 a)				/* a guest descriptor's value */
{
	u32 v = *(volatile u32 *)(uintptr_t)(a & ~3u);

	if (a & 1)			/* long: both words */
		v ^= *(volatile u32 *)(uintptr_t)((a & ~3u) + 4) * 0x9e3779b1u;
	return v;
}

/* software TLB for SORIX-ILOM-LDOM's own accesses to guest memory */
#define NTLB		16
struct tlb {
	u32 va, pa;			/* 256-byte pages; va = 1: empty */
	int w;				/* writable */
};

struct view {
	u32 *l1;			/* shadow root table */
	u32 crp[2];			/* real CRP for it */
	struct tlb tlb[NTLB];
};

static void
tlb_clear(struct view *v)
{
	int i;

	for (i = 0; i < NTLB; i++)
		v->tlb[i].va = 1;
}

/* views[2] (above): 0 user, 1 kernel */
static int loaded = -1;			/* view in the real CRP, -1 MMU off */
static u32 n_flush_user;
static struct view views[2];

/* the real MMU, per CPU family */
static void
mmu_load(int v)
{
	if (cpuinfo.model == 30)
		mmu030_load(views[v].crp, SH_TC, SH_TT0);
	else
		mmu040_load(views[v].crp[1], SH_TC40, SH_DTT0_40, SH_DTT1_40,
		    SH_ITT0_40);
}

static void
mmu_root(int v)
{
	if (cpuinfo.model == 30)
		mmu030_root(views[v].crp);
	else
		mmu040_root(views[v].crp[1]);
}

static void
mmu_flush(void)
{
	if (cpuinfo.model == 30)
		mmu030_flush();
	else
		mmu040_flush();
}

static void
mmu_off(void)
{
	if (cpuinfo.model == 30)
		mmu030_off();
	else
		mmu040_off();
}
static int enabled;			/* shadow active (68030) */
static u32 n_fill, n_wp, n_guestfault, n_flush, n_reset;

/* the first faults passed to the guest, for the shell's "guest" */
#define NLOG		12		/* the first 4, then the last 8 */
static struct { u32 va, pc; u16 ssw, mmusr; u8 super, write; } flog[NLOG];
static u32 nflog;			/* faults logged in all */

static void
flog_add(u32 va, u32 pc, u32 ssw, u32 mmusr, int super, int write)
{
	u32 i = nflog < 4 ? nflog : 4 + (nflog - 4) % (NLOG - 4);

	flog[i].va = va;
	flog[i].pc = pc;
	flog[i].ssw = (u16)ssw;
	flog[i].mmusr = (u16)mmusr;
	flog[i].super = (u8)super;
	flog[i].write = (u8)write;
	nflog++;
}
static u32 cur_pc;
static u16 cur_ssw;

/*
 * Write watchpoint (shell "watch"): the page stays write-protected in the
 * shadow; a write to it is logged, let through for one instruction (trace)
 * and the page protected again. Kept across guest restarts.
 */
#define NWLOG		8
static u32 watch_va;
static struct { u32 pc, va; } wlog[NWLOG];
static u32 nwlog, nwatch;
static u32 watch_open;			/* va made writable for one step */
static u32 watch_val;			/* value after the last logged write */

static int walk_trace;			/* shell "pt": print each descriptor */

static u32
pmask(u32 bits)
{
	return bits >= 32 ? 0xffffffff : (1u << bits) - 1;
}

static u32
prd(u32 pa)
{
	return *(volatile u32 *)(uintptr_t)pa;
}

static void
pwr(u32 pa, u32 v)
{
	*(volatile u32 *)(uintptr_t)pa = v;
}

static void *
pool_alloc_al(u32 size, u32 al)
{
	void *p;

	pool_used = (pool_used + al - 1) & ~(al - 1);
	if (pool_used + size > POOL_SIZE)
		return NULL;
	p = pool + pool_used;
	pool_used += size;
	memset(p, 0, size);
	return p;
}

#define pool_alloc_tab()	pool_alloc_al(SH_L1 * 4, sh_al1)	/* L1, L2 */

/* Drop all shadow tables and start again with empty roots. */
static void
shadow_clear(void)
{
	int i;

	pool_used = 0;
	nl3 = 0;
	l3free = NULL;
	for (i = 0; i < 2; i++) {
		views[i].l1 = pool_alloc_tab();
		views[i].crp[0] = 0x7fff0002;	/* upper limit 7fff, short */
		views[i].crp[1] = (u32)(uintptr_t)views[i].l1;
		tlb_clear(&views[i]);
	}
	if (loaded >= 0)
		mmu_root(loaded);	/* the old tables are gone */
}

/* 4 KB aligned scratch memory for the boot-time MMU self-test */
u32
vmmu_scratch(u32 size)
{
	u32 a = ((u32)(uintptr_t)pool + 0xfff) & ~0xfffu;

	return a + size <= (u32)(uintptr_t)pool + POOL_SIZE ? a : 0;
}

void
vmmu_reset(void)
{
	enabled = cpuinfo.mmu_ok && cpuinfo.model >= 30;
	if (cpuinfo.model == 30) {
		sh_page = 2048;
		sh_n1 = sh_n2 = 256;
		sh_n3 = 32;
		sh_s1 = 24;
		sh_s2 = 16;
		sh_s3 = 11;
		sh_al1 = sh_al3 = 16;
	} else {
		sh_page = 4096;
		sh_n1 = sh_n2 = 128;
		sh_n3 = 64;
		sh_s1 = 25;
		sh_s2 = 18;
		sh_s3 = 12;
		sh_al1 = 512;
		sh_al3 = 256;
	}
	n_fill = n_wp = n_guestfault = n_flush = nflog = n_flush_user = 0;
	n_pflush = n_kept = n_dropped = 0;
	n_reset++;
	shadow_clear();
	if (loaded >= 0)
		mmu_flush();
}

void
vmmu_flush(void)
{
	n_flush++;
	shadow_clear();
	if (loaded >= 0)
		mmu_root(loaded);
}

/*
 * A new CRP only changes the user view when the guest's supervisor
 * accesses use the SRP (TC SRE), as with AMIX's hat_asload (pmove crp;
 * pflusha); the kernel view is kept. The old user view's tables stay
 * allocated until the pool is next cleared.
 */
void
vmmu_flush_user(void)
{
	struct view *v = &views[0];

	if (cpuinfo.model == 30 && !(guest.tc & TC_SRE)) {	/* CRP is both */
		vmmu_flush();
		return;
	}
	n_flush_user++;
	for (u32 i = 0; i < nl3; )		/* recycle the user view's L3s */
		if (l3list[i]->view == 0)
			l3_release(i);
		else
			i++;
	if (!(v->l1 = pool_alloc_tab())) {
		vmmu_flush();			/* pool full: start over */
		return;
	}
	v->crp[1] = (u32)(uintptr_t)v->l1;
	tlb_clear(v);
	if (loaded >= 0)
		mmu_root(loaded);
}

/*
 * Guest PFLUSH: keep what is still right (see struct l3meta). Entries the
 * CPU used since the last PFLUSH (U set by its table walk) are checked
 * against the guest's descriptors; everything else goes.
 */
void
vmmu_pflush(void)
{
	u32 i = 0, k, j, pd, any;
	struct l3meta *m;

	n_pflush++;
	while (i < nl3) {
		m = l3list[i];
		any = 0;
		if (m->uniform == 1) {
			for (j = 0; j < m->nup; j++)
				if (dsig(m->up_a[j]) != m->up_v[j])
					break;
			if (j == m->nup)
				for (k = 0; k < SH_L3; k++) {
					pd = m->l3[k];
					if (!(pd & D_DT))
						continue;
					if ((pd & D_U) && (!m->pd_a[k] ||
					    dsig(m->pd_a[k]) == m->pd_v[k])) {
						m->l3[k] = pd & ~(u32)D_U;
						any++;
					} else {
						m->l3[k] = 0;
						n_dropped++;
					}
				}
		}
		if (!any) {
			l3_release(i);
			continue;
		}
		n_kept += any;
		i++;
	}
	tlb_clear(&views[0]);
	tlb_clear(&views[1]);
	if (loaded >= 0)
		mmu_flush();
}

void
vmmu_activate(void)
{
	int v = (guest.vsr & VSR_S) != 0;

	if (!enabled || loaded == v)
		return;
	if (loaded < 0)
		mmu_load(v);
	else
		mmu_root(v);
	loaded = v;
}

void
vmmu_off(void)
{
	if (loaded >= 0)
		mmu_off();
	loaded = -1;
}

/* ------------------------------------------------------------------ */
/* The guest's tables                                                  */

struct walk {
	u32 pa;
	u32 desc;			/* address of the last descriptor */
	u32 pdesc;			/* page descriptor, 0 for TT/no MMU */
	int wp, ci, mod;
	u32 levels;
	/* the descriptors used, root first (address | 1 if long) */
	u32 npath, path[MAXPATH];
};

/* 68040/68060 ITTx/DTTx match: base/mask, E, S field (user/super/both) */
static int
tt40_match(u32 tt, u32 va, u32 fc)
{
	u32 base = tt >> 24, mask = (tt >> 16) & 0xff, sf = (tt >> 13) & 3;

	if (!(tt & 0x8000))
		return 0;
	if (((va >> 24) & ~mask & 0xff) != (base & ~mask))
		return 0;
	if ((sf == 0 && (fc & 4)) || (sf == 1 && !(fc & 4)))
		return 0;
	return 1;
}

/*
 * The guest's 68040/68060 tables: TC E (bit 15), P (8 KB pages, bit 14);
 * URP/SRP; root (7 bits) and pointer (7 bits) descriptors resident if
 * UDT bit 1; page descriptors resident if PDT bit 0, indirect if PDT 2;
 * W bit 2, U 3, M 4, CM 6-5, S 7. ITTx (program FCs) and DTTx (data)
 * translate transparently. Returns MMUSR bits in the 68030 sense
 * (I, W, S, B, T) for the shared callers.
 */
static u16
walk040(u32 va, u32 fc, int write, int update, struct walk *w)
{
	u32 tc = guest.tcr, big = (tc & 0x4000) != 0, ea, d, tt0, tt1;
	int s = 0;
	u16 sr = 0;

	if ((fc & 3) == 2) {			/* program space: ITTs */
		tt0 = guest.itt0;
		tt1 = guest.itt1;
	} else {
		tt0 = guest.dtt0;
		tt1 = guest.dtt1;
	}
	if (tt40_match(tt0, va, fc) || tt40_match(tt1, va, fc)) {
		u32 tt = tt40_match(tt0, va, fc) ? tt0 : tt1;

		sr = MMUSR_T;
		w->ci = ((tt >> 5) & 3) >= 2;
		if ((tt & 4) && write)
			w->wp = 1, sr |= MMUSR_W;
		goto phys;
	}
	if (!(tc & 0x8000))
		goto phys;
	/* root */
	ea = (((fc & 4) ? guest.srp40 : guest.urp) & ~0x1ffu) + (va >> 25) * 4;
	d = prd(ea);
	w->path[w->npath++] = ea;
	w->desc = ea;
	w->levels++;
	if (!(d & 2)) {
		sr |= MMUSR_I;
		goto done;
	}
	if (update && !(d & D_U))
		pwr(ea, d |= D_U);
	if (d & D_WP)
		w->wp = 1;
	/* pointer */
	ea = (d & ~0x1ffu) + ((va >> 18) & 0x7f) * 4;
	d = prd(ea);
	w->path[w->npath++] = ea;
	w->desc = ea;
	w->levels++;
	if (!(d & 2)) {
		sr |= MMUSR_I;
		goto done;
	}
	if (update && !(d & D_U))
		pwr(ea, d |= D_U);
	if (d & D_WP)
		w->wp = 1;
	/* page */
	ea = big ? (d & ~0x7fu) + ((va >> 13) & 0x1f) * 4 :
	    (d & ~0xffu) + ((va >> 12) & 0x3f) * 4;
	d = prd(ea);
	w->path[w->npath++] = ea;
	w->desc = ea;
	w->levels++;
	if ((d & 3) == 2) {			/* indirect */
		ea = d & ~3u;
		d = prd(ea);
		w->path[w->npath++] = ea;
		w->desc = ea;
		w->levels++;
	}
	if (!(d & 1)) {
		sr |= MMUSR_I;
		goto done;
	}
	w->pdesc = ea;
	if (d & D_WP)
		w->wp = 1;
	if (d & 0x80)
		s = 1;
	w->ci = ((d >> 5) & 3) >= 2;
	w->pa = big ? (d & ~0x1fffu) | (va & 0x1fff) : (d & ~0xfffu) | (va & 0xfff);
	if (s && !(fc & 4))
		sr |= MMUSR_S;
	if (w->wp)
		sr |= MMUSR_W;
	if (!(sr & MMUSR_S) && !(write && w->wp) && update) {
		u32 nv = d | D_U | (write ? D_M : 0);

		if (nv != d)
			pwr(ea, d = nv);
	}
	w->mod = (d & D_M) != 0;
	if (d & D_M)
		sr |= MMUSR_M;
phys:
	if (w->pa >= bootcap.hole_base && w->pa < bootcap.hole_top)
		sr |= MMUSR_B;
	if (!w->pdesc)
		w->mod = 1;
done:
	return sr | (u16)(w->levels & MMUSR_N);
}

/* 68030 TT0/TT1 match for a data access */
static int
tt_match(u32 tt, u32 va, u32 fc, int write)
{
	u32 base = tt >> 24, mask = (tt >> 16) & 0xff;
	u32 fcbase = (tt >> 4) & 7, fcmask = tt & 7;

	if (!(tt & 0x8000))
		return 0;
	if (((va >> 24) & ~mask & 0xff) != (base & ~mask))
		return 0;
	if ((fc & ~fcmask & 7) != (fcbase & ~fcmask))
		return 0;
	return (tt & 0x0100) || (!(tt & 0x0200) == !!write);
}

/*
 * Walk the guest's tables like the 68030 does (up to 'maxlev' levels;
 * 7 = all). update: set U in the descriptors used, M in the page
 * descriptor on a write that is allowed. Returns MMUSR bits.
 */
static u16
walk(u32 va, u32 fc, int write, int update, u32 maxlev, struct walk *w)
{
	u32 tc = guest.tc, ti[5], n = 0, i, idx, lim, bits, a;
	u32 d0, d1, dt, nd0, nd1, ea, ps;
	int cur_long, s = 0;
	u16 sr = 0;

	w->wp = w->ci = w->mod = 0;
	w->pdesc = w->desc = 0;
	w->npath = 0;
	w->levels = 0;
	w->pa = va;
	if (cpuinfo.model >= 40)
		return walk040(va, fc, write, update, w);
	if (tt_match(guest.tt0, va, fc, write) ||
	    tt_match(guest.tt1, va, fc, write)) {
		sr = MMUSR_T;
		goto phys;
	}
	if (!(tc & TC_E))
		goto phys;
	ps = (tc >> 20) & 15;
	if (tc & TC_FCL)
		ti[n++] = 0;			/* function code level */
	for (i = 0; i < 4 && ((tc >> (12 - 4 * i)) & 15); i++)
		ti[n++] = (tc >> (12 - 4 * i)) & 15;
	if ((fc & 4) && (tc & TC_SRE)) {
		d0 = guest.srp[0];
		d1 = guest.srp[1];
	} else {
		d0 = guest.crp[0];
		d1 = guest.crp[1];
	}
	cur_long = 1;
	dt = d0 & D_DT;
	bits = 32 - ((tc >> 16) & 15);		/* after the initial shift */
	a = va << ((tc >> 16) & 15);
	for (i = 0; i < n && dt >= DT_SHORT; i++) {
		if (w->levels >= maxlev)
			goto done;
		if (ti[i] == 0) {		/* FCL: indexed by FC */
			idx = fc & 7;
		} else {
			idx = a >> (32 - ti[i]);
			a <<= ti[i];
			bits -= ti[i];
		}
		if (cur_long) {
			lim = (d0 >> 16) & 0x7fff;
			if ((d0 & 0x80000000) ? idx < lim : idx > lim) {
				sr |= MMUSR_L;
				goto done;
			}
		}
		ea = ((cur_long ? d1 : d0) & ~0xf) + idx * (dt == DT_LONG ? 8 : 4);
		nd0 = prd(ea);
		nd1 = dt == DT_LONG ? prd(ea + 4) : 0;
		if (w->npath < MAXPATH)
			w->path[w->npath++] = ea | (dt == DT_LONG);
		if (walk_trace)
			kprintf("  level %u: index %x, %s descriptor at %08x: %08x"
			    " %08x\n", w->levels + 1, idx, dt == DT_LONG ? "long" :
			    "short", ea, nd0, dt == DT_LONG ? nd1 : 0);
		w->desc = ea;
		w->levels++;
		if ((nd0 & D_DT) == 0) {
			sr |= MMUSR_I;
			goto done;
		}
		if (update && (nd0 & D_DT) != DT_PAGE && !(nd0 & D_U))
			pwr(ea, nd0 |= D_U);	/* table descriptor used */
		if (nd0 & D_WP)
			w->wp = 1;
		if (dt == DT_LONG && (nd0 & D_S))
			s = 1;
		cur_long = dt == DT_LONG;
		d0 = nd0;
		d1 = nd1;
		dt = nd0 & D_DT;
	}
	if (dt == DT_SHORT || dt == DT_LONG) {	/* indirect descriptor */
		if (w->levels >= maxlev)
			goto done;
		ea = (cur_long ? d1 : d0) & ~3;
		cur_long = dt == DT_LONG;
		d0 = prd(ea);
		d1 = cur_long ? prd(ea + 4) : 0;
		w->desc = ea;
		if (w->npath < MAXPATH)
			w->path[w->npath++] = ea | (u32)cur_long;
		w->levels++;
		if (walk_trace)
			kprintf("  indirect: page descriptor at %08x: %08x %08x\n",
			    ea, d0, d1);
		if ((d0 & D_DT) != DT_PAGE) {
			sr |= MMUSR_I;
			goto done;
		}
		if (d0 & D_WP)
			w->wp = 1;
		if (cur_long && (d0 & D_S))
			s = 1;
	} else if (dt != DT_PAGE) {
		sr |= MMUSR_I;
		goto done;
	}
	/* d0/d1: page descriptor (normal or early termination) at w->desc */
	w->pdesc = w->desc;
	w->ci = (d0 & D_CI) != 0;
	w->pa = (((cur_long ? d1 : d0) & ~0xff) & ~pmask(ps)) +
	    (va & pmask(bits));
	if (s && !(fc & 4))
		sr |= MMUSR_S;
	if (w->wp)
		sr |= MMUSR_W;
	if (!(sr & MMUSR_S) && !(write && w->wp) && update && w->pdesc) {
		u32 nv = d0 | D_U | (write ? D_M : 0);

		if (nv != d0)
			pwr(w->pdesc, d0 = nv);
	}
	w->mod = (d0 & D_M) != 0;
	if (d0 & D_M)
		sr |= MMUSR_M;
phys:
	if (w->pa >= bootcap.hole_base && w->pa < bootcap.hole_top)
		sr |= MMUSR_B;
	if (!w->pdesc)
		w->mod = 1;			/* untranslated: no M bit to keep */
done:
	return sr | (u16)(w->levels & MMUSR_N);
}

u16
vmmu_translate(u32 va, int super, int write, u32 *pa)
{
	struct view *v = &views[super != 0];
	struct tlb *t = &v->tlb[(va >> 8) & (NTLB - 1)];
	struct walk w;
	u16 sr;

	if ((va & ~(u32)0xff) == t->va && (t->w || !write)) {
		*pa = t->pa | (va & 0xff);
		return 0;
	}
	sr = walk(va, super ? 5 : 1, write, 1, 7, &w);
	if ((sr & MMUSR_FAULT) || (write && (sr & MMUSR_W)))
		return sr & (MMUSR_FAULT | MMUSR_W);
	*pa = w.pa;
	t->va = va & ~(u32)0xff;		/* smallest 68030 page */
	t->pa = w.pa & ~(u32)0xff;
	t->w = !w.wp && w.mod;
	return 0;
}

u16
vmmu_ptest(u32 va, u32 fc, int write, u32 level, u32 *desc)
{
	struct walk w;
	u16 sr;

	(void)write;
	sr = walk(va, fc, 0, 0, level ? level : 7, &w);
	if (level == 0)			/* level 0: the ATC's view, no N */
		sr = (u16)(sr & ~MMUSR_N);
	*desc = w.desc;
	return sr;
}

/* ------------------------------------------------------------------ */
/* Shadow fill                                                         */

/* a new L3 shadow table with its record */
static u32 *
l3_alloc(int vi, u32 *l2e)
{
	struct l3meta *m;
	u32 *l3;

	if (nl3 >= NL3)
		return NULL;
	if (l3free) {
		l3 = l3free;
		l3free = (u32 *)(uintptr_t)l3[0];
		memset(l3, 0, L3SIZE);
	} else if (!(l3 = pool_alloc_al(L3SIZE, sh_al3)))
		return NULL;
	m = (struct l3meta *)(l3 + SH_L3);
	m->l3 = l3;
	m->l2e = l2e;
	m->view = (u8)vi;
	m->uniform = 2;			/* 2: no entry yet */
	l3list[nl3++] = m;
	return l3;
}

static void
l3_release(u32 i)			/* list index i: unlink and recycle */
{
	struct l3meta *m = l3list[i];

	*m->l2e = 0;
	m->l3[0] = (u32)(uintptr_t)l3free;
	l3free = m->l3;
	l3list[i] = l3list[--nl3];
}

/*
 * Install the mapping for one shadow page, made from walk w. 0 or -1
 * (pool exhausted).
 */
static int
shadow_set(struct view *v, u32 va, u32 pd, const struct walk *w)
{
	u32 *l2, *l3, i1 = I1(va), i2 = I2(va), k, j, nup;
	struct l3meta *m;

	if (!(v->l1[i1] & D_DT)) {
		if (!(l2 = pool_alloc_tab()))
			return -1;
		v->l1[i1] = (u32)(uintptr_t)l2 | DT_SHORT;
	}
	l2 = (u32 *)(uintptr_t)(v->l1[i1] & ~0xf);
	if (!(l2[i2] & D_DT)) {
		if (!(l3 = l3_alloc(v == &views[1], &l2[i2])))
			return -1;
		l2[i2] = (u32)(uintptr_t)l3 | DT_SHORT;
	}
	l3 = (u32 *)(uintptr_t)(l2[i2] & ~0xf);
	k = I3(va);
	l3[k] = pd;
	if (!w)
		return 0;			/* same entry, new protection */
	m = (struct l3meta *)(l3 + SH_L3);
	nup = w->npath ? w->npath - 1 : 0;
	if (m->uniform == 2) {			/* first entry: its path */
		m->uniform = 1;
		m->nup = (u8)nup;
		for (j = 0; j < nup; j++) {
			m->up_a[j] = w->path[j];
			m->up_v[j] = dsig(w->path[j]);
		}
	} else if (m->nup != nup)
		m->uniform = 0;
	else
		for (j = 0; j < nup; j++)
			if (m->up_a[j] != w->path[j])
				m->uniform = 0;
	if (w->npath) {
		m->pd_a[k] = w->path[w->npath - 1];
		m->pd_v[k] = dsig(m->pd_a[k]);
	} else
		m->pd_a[k] = 0;
	return 0;
}

static u32
shadow_get(const struct view *v, u32 va)
{
	u32 d = v->l1[I1(va)];

	if (!(d & D_DT))
		return 0;
	d = ((u32 *)(uintptr_t)(d & ~0xf))[I2(va)];
	if (!(d & D_DT))
		return 0;
	return ((u32 *)(uintptr_t)(d & ~0xf))[I3(va)];
}

/*
 * Make the shadow map va for this access if the guest's tables allow it.
 * 1: mapped now, 0: already mapped as asked (not a shadow miss),
 * -1: the guest's tables refuse it.
 */
static int
shadow_fill(u32 va, int write)
{
	int super = (guest.vsr & VSR_S) != 0;
	struct view *v = &views[super];
	u32 old = shadow_get(v, va), pd;
	struct walk w;
	u16 sr;

	if ((old & D_DT) && (!write || !(old & D_WP)))
		return 0;
	sr = walk(va, super ? 5 : 1, write, 1, 7, &w);
	if ((sr & MMUSR_FAULT) || (write && w.wp)) {
		flog_add(va, cur_pc, cur_ssw, sr,
		    (u8)super, (u8)write);
		n_guestfault++;
		return -1;
	}
	/* write-protect until the guest's M bit is set, so it gets set */
	pd = ((w.pa & ~(SH_PAGE - 1)) | D_M | DT_PAGE) |
	    (w.ci ? D_CI : 0) | (w.wp || !w.mod ? D_WP : 0);
	if (watch_va && !((w.pa ^ watch_va) & ~(SH_PAGE - 1))) {
		if (!watch_open)
			pd |= D_WP;
		else
			pd &= ~(u32)D_WP;
	}
	if (shadow_set(v, va, pd, &w)) {
		shadow_clear();			/* pool full: start over */
		v = &views[super];
		if (shadow_set(v, va, pd, &w))
			return -1;
	}
	if (old & D_DT)
		n_wp++;
	else
		n_fill++;
	tlb_clear(v);
	if (loaded >= 0)
		mmu_root(loaded);	/* also flushes the ATC */
	return 1;
}

/*
 * 68040 access error frame (format 7): SSW at +0x0C (ATC bit 10: an
 * address translation fault; RW bit 8: read; LK bit 9), fault address at
 * +0x14, write-backs WB3/WB2/WB1 (status words at +0x0E/+0x10/+0x12: V bit
 * 7, size bits 6-5; address and data at +0x18/+0x1C, +0x20/+0x24,
 * +0x28/+0x2C). The CPU restarts the instruction but does not do pending
 * write-backs, the handler does: for a shadow miss SORIX-ILOM-LDOM fills the
 * shadow, completes the write-backs through the guest's tables and
 * returns. Anything that does not work out is the guest's to handle.
 */
static int
writeback040(u8 *f, u32 so, u32 ao, u32 dO)
{
	u16 wbs = *(u16 *)(f + so);
	u32 a = *(u32 *)(f + ao), d = *(u32 *)(f + dO), pa;
	int super = (guest.vsr & VSR_S) != 0, sz = (wbs >> 5) & 3;

	if (!(wbs & 0x80))
		return 0;
	if (sz == 3 || shadow_fill(a, 1) < 0 ||
	    vmmu_translate(a, super, 1, &pa))
		return -1;			/* MOVE16 line, or the guest's */
	if (sz == 1)
		*(volatile u8 *)(uintptr_t)pa = (u8)d;
	else if (sz == 2)
		*(volatile u16 *)(uintptr_t)pa = (u16)d;
	else
		*(volatile u32 *)(uintptr_t)pa = d;
	*(u16 *)(f + so) = wbs & ~0x80;
	return 0;
}

/*
 * The 68040/68060 report an access that runs into the next page (an
 * instruction or operand straddling the boundary) at its start, whose page
 * may already be mapped: then the next page is the one missing.
 */
static int
next_page(u32 fa, int write)
{
	if ((fa & (SH_PAGE - 1)) < SH_PAGE - 16)
		return 0;
	return shadow_fill((fa | (SH_PAGE - 1)) + 1, write);
}

static int
fault040(struct excregs *r)
{
	u8 *f = (u8 *)&r->sr;
	u16 ssw = *(u16 *)(f + 0x0c);
	u32 fa = *(u32 *)(f + 0x14);
	int write = !(ssw & 0x0100) || (ssw & 0x0200), k;

	cur_pc = r->pc;
	cur_ssw = ssw;
	if (!(ssw & 0x0400))
		return -1;			/* a bus error, not the MMU */
	if ((k = shadow_fill(fa, write)) == 0 && (k = next_page(fa, write)) < 0) {
		/*
		 * The access runs into the next page, which the guest has not
		 * mapped: name that page in the frame (the 68040 reports the
		 * start of the access, with MA), so the guest maps the right one.
		 */
		*(u32 *)(f + 0x14) = (fa | (SH_PAGE - 1)) + 1;
		*(u16 *)(f + 0x0c) = ssw & ~0x0800;
		return -1;
	}
	if (k <= 0) {
		if (k == 0)	/* a real access error */
			flog_add(fa, r->pc, ssw, 0,
			    (guest.vsr & VSR_S) != 0, (u8)write);
		return -1;
	}
	if (writeback040(f, 0x0e, 0x18, 0x1c) ||	/* WB3 */
	    writeback040(f, 0x10, 0x20, 0x24) ||	/* WB2 */
	    writeback040(f, 0x12, 0x28, 0x2c))		/* WB1 */
		return -1;
	return 0;
}

/*
 * 68030 bus error frames (format A short, B long): SSW at +0x0A, data
 * fault address at +0x10, stage B address at +0x24 (format B only;
 * otherwise PC + 4). Stage C is the word before stage B.
 */
int
vmmu_fault(struct excregs *r)
{
	u8 *f = (u8 *)&r->sr;
	u16 ssw = *(const u16 *)(f + 0x0a);
	u32 fmt = r->fmtvec >> 12, sb, done = 0;
	int k;

	if (!enabled)
		return -1;
	if (fmt == 4) {
		/*
		 * 68060 access error: fault address at +8, FSLW at +0x0C
		 * (bit 23: write, also read-modify-write). The CPU restarts
		 * the instruction after the RTE.
		 */
		u32 fa = *(const u32 *)(f + 8), fslw = *(const u32 *)(f + 0x0c);

		cur_pc = r->pc;
		cur_ssw = (u16)fslw;
		if ((k = shadow_fill(fa, (fslw & 0x00800000) != 0)) > 0)
			return 0;
		if (k == 0 && (k = next_page(fa, (fslw & 0x00800000) != 0)) > 0)
			return 0;
		if (k < 0 && (fa & (SH_PAGE - 1)) >= SH_PAGE - 16 &&
		    shadow_get(&views[(guest.vsr & VSR_S) != 0], fa) & D_DT) {
			/* the next page is the guest's missing one: name it */
			*(u32 *)(f + 8) = (fa | (SH_PAGE - 1)) + 1;
			*(u32 *)(f + 0x0c) = fslw & ~0x08000000;
			return -1;
		}
		if (k == 0)	/* a real access error */
			flog_add(fa, r->pc, (u16)fslw, 0,
			    (guest.vsr & VSR_S) != 0, (fslw & 0x00800000) != 0);
		return -1;
	}
	if (fmt == 7)
		return fault040(r);
	if (fmt != 0xa && fmt != 0xb)
		return -1;
	sb = fmt == 0xb ? *(const u32 *)(f + 0x24) : r->pc + 4;
	cur_pc = r->pc;
	cur_ssw = ssw;
	if (ssw & 0x8000) {			/* FC: stage C fetch */
		if ((k = shadow_fill(sb - 2, 0)) < 0)
			return -1;
		done += (u32)k;
	}
	if (ssw & 0x4000) {			/* FB: stage B fetch */
		if ((k = shadow_fill(sb, 0)) < 0)
			return -1;
		done += (u32)k;
	}
	if (ssw & 0x0100) {			/* DF: data cycle */
		u32 da = *(const u32 *)(f + 0x10);
		int write = !(ssw & 0x0040);

		struct walk w;
		u16 sr = write && watch_va ?
		    walk(da, guest.vsr & VSR_S ? 5 : 1, 1, 0, 7, &w) : MMUSR_I;

		if (!(sr & MMUSR_FAULT) &&
		    !((w.pa ^ watch_va) & ~(SH_PAGE - 1))) {
			if (!w.wp) {
				nwatch++;
				if (w.pa + 4 > watch_va && w.pa < watch_va + 4 &&
				    nwlog < NWLOG) {
					wlog[nwlog].pc = r->pc;
					wlog[nwlog++].va = da;
				}
				watch_open = da;
				shadow_fill(da, 1);
				return 1;		/* step it, then close */
			}
		}
		if ((k = shadow_fill(da, write)) < 0)
			return -1;
		done += (u32)k;
	}
	if (!done)		/* a real bus error: log it */
		flog_add(*(const u32 *)(f + 0x10), r->pc, ssw, 0,
		    (guest.vsr & VSR_S) != 0, !(ssw & 0x0040));
	return done ? 0 : -1;			/* nothing missing: real error */
}

/* trace after a watched write: protect the page again */
int
vmmu_watch_step(void)
{
	struct view *v;
	u32 pd;

	if (!watch_open)
		return 0;
	watch_val = prd(watch_va);
	v = &views[(guest.vsr & VSR_S) != 0];
	pd = shadow_get(v, watch_open);
	if (pd & D_DT)
		shadow_set(v, watch_open, pd | D_WP, NULL);
	watch_open = 0;
	if (loaded >= 0)
		mmu_root(loaded);
	return 1;
}

/*
 * The watched long changed without a write through the MMU (DMA, or
 * SORIX-ILOM-LDOM itself)? Checked on every entry from the guest.
 */
int
vmmu_watch_check(u32 *old, u32 *now)
{
	u32 v;

	if (!watch_va || watch_open)
		return 0;
	v = prd(watch_va);
	if (v == watch_val)
		return 0;
	*old = watch_val;
	*now = watch_val = v;
	return 1;
}

void
vmmu_watch(u32 va)
{
	watch_va = va;
	watch_val = va ? prd(va) : 0;
	nwlog = nwatch = 0;
	watch_open = 0;
	shadow_clear();
	if (loaded >= 0)
		mmu_root(loaded);
}

/* shell "pt": the guest's table walk for va, descriptor by descriptor */
void
vmmu_dump(u32 va, int super)
{
	struct walk w;
	u16 sr;
	u32 pd;

	kprintf("guest TC %08x, %s root %08x %08x\n", guest.tc,
	    super && (guest.tc & TC_SRE) ? "SRP" : "CRP",
	    super && (guest.tc & TC_SRE) ? guest.srp[0] : guest.crp[0],
	    super && (guest.tc & TC_SRE) ? guest.srp[1] : guest.crp[1]);
	walk_trace = 1;
	sr = walk(va, super ? 5 : 1, 0, 0, 7, &w);
	walk_trace = 0;
	kprintf("  -> %08x, MMUSR %04x%s%s%s%s%s\n", w.pa, sr,
	    sr & MMUSR_I ? " invalid" : "", sr & MMUSR_W ? " write-protected" : "",
	    sr & MMUSR_S ? " supervisor-only" : "", sr & MMUSR_L ? " limit" : "",
	    sr & MMUSR_B ? " SORIX-ILOM-LDOM hole" : "");
	pd = shadow_get(&views[super != 0], va);
	kprintf("  shadow (%s view): %08x\n", super ? "kernel" : "user", pd);
}

void
vmmu_show(void)
{
	if (!enabled && !watch_va) {
		kprintf("  shadow MMU: off (no working MMU)\n");
		return;
	}
	if (cpuinfo.model >= 40)
		kprintf("  shadow MMU: guest TC %08x URP %08x SRP %08x ITT0 %08x "
		    "DTT0 %08x\n", guest.tcr, guest.urp, guest.srp40, guest.itt0,
		    guest.dtt0);
	else
		kprintf("  shadow MMU: guest TC %08x CRP %08x%08x SRP %08x%08x\n",
		    guest.tc, guest.crp[0], guest.crp[1], guest.srp[0],
		    guest.srp[1]);
	kprintf("  shadow MMU: %u fills, %u M-bit upgrades, %u guest faults, "
	    "%u flushes (+%u user view only), pool %u/%u bytes\n", n_fill,
	    n_wp, n_guestfault, n_flush, n_flush_user, pool_used, POOL_SIZE);
	kprintf("  shadow MMU: %u PFLUSHes revalidated: %u entries kept, %u "
	    "dropped; %u L3 tables live\n", n_pflush, n_kept, n_dropped, nl3);
	if (watch_va)
		kprintf("  watch %08x: %u writes to its page\n", watch_va, nwatch);
	for (u32 i = 0; i < nwlog; i++)
		kprintf("  watch: write via %08x at PC %08x\n", wlog[i].va,
		    wlog[i].pc);
	for (u32 n = 0; n < nflog && n < NLOG; n++) {
		u32 i = n < 4 || nflog <= NLOG ? n :
		    4 + (nflog - (NLOG - 4) + (n - 4) - 4) % (NLOG - 4);

		if (n == 4 && nflog > NLOG)
			kprintf("  ... %u more, the last %u:\n", nflog - NLOG,
			    NLOG - 4);
		kprintf("  guest fault: %s %s %08x at PC %08x, SSW %04x, "
		    "MMUSR %04x\n", flog[i].super ? "kernel" : "user",
		    flog[i].write ? "write" : "read", flog[i].va, flog[i].pc,
		    flog[i].ssw, flog[i].mmusr);
	}
}
