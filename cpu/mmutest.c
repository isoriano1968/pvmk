/*
 * MMU self-test: SORIX-ILOM-LDOM requires a working MMU to run guests.
 *
 * Everything below 0xC0000000 stays identity mapped through the
 * transparent translation registers (code, stack, chip RAM, I/O, Zorro);
 * only the test region at VA_OK is translated through page tables:
 *
 *	VA_OK  (0xC0000000) -> a scratch page in the hole
 *	VA_BAD (0xC0001000) -> invalid: must cause an access fault
 *
 * Checks: the value read through the mapping is the scratch page's, a
 * write through it lands in the scratch page, and VA_BAD faults.
 *
 * 68030 tables (TC: 4 KB pages, TIA 8, TIB 4, TIC 8): A[0xC0] -> B,
 * B[0] -> C, C[0] = page. 68040/68060 (TC: 4 KB pages): root[0x60] ->
 * pointer table, pointer[0] -> page table, page[0] = page.
 */
#include "pvmk/vmmu.h"
#include "pvmk/bootcap.h"
#include "pvmk/kprintf.h"
#include "pvmk/layout.h"
#include "pvmk/mk.h"

#define VA_OK		0xc0000000
#define VA_BAD		0xc0001000
#define PATTERN_R	0x13579bdf
#define PATTERN_W	0x5a5aa5a5

int mmu_test030(const u32 *regs, u32 va_ok, u32 va_bad, u32 *out);
int mmu_test040(const u32 *regs, u32 va_ok, u32 va_bad, u32 *out);

static const char *result_text = "not run";

const char *
mmu_result(void)
{
	return result_text;
}

int
mmu_selftest(void)
{
	u32 page = vmmu_scratch(0x2000);	/* the shadow pool, still unused */
	u32 tab = page + 0x1000, out = 0, regs[6];
	volatile u32 *pg = (volatile u32 *)(uintptr_t)page;
	u32 *t = (u32 *)(uintptr_t)tab;
	int r;

	cpuinfo.mmu_ok = 0;
	if (cpuinfo.model < 30) {
		result_text = "no MMU (68020)";
		return 0;
	}
	if (!page) {
		result_text = "no room for test tables";
		return 0;
	}
	memset(t, 0, 0x1000);
	pg[0] = PATTERN_R;

	if (cpuinfo.model == 30) {
		u32 *a = t, *b = t + 0x100, *c = t + 0x140;

		a[VA_OK >> 24] = (u32)(uintptr_t)b | 2;		/* table */
		b[0] = (u32)(uintptr_t)c | 2;
		c[0] = page | 1;				/* page */
		/* CRP: L/U = 0 means upper limit; 0x7fff allows every index */
		regs[0] = 0x7fff0002;
		regs[1] = tab;
		regs[2] = 0x80c08480;				/* TC */
		regs[3] = 0x007f8107;		/* TT0: 0-7FFFFFFF, any FC/RW */
		regs[4] = 0x803f8107;		/* TT1: 80000000-BFFFFFFF */
		regs[5] = 0;
		r = mmu_test030(regs, VA_OK, VA_BAD, &out);
	} else {
		u32 *root = t, *ptr = t + 0x80, *pt = t + 0x100;

		root[VA_OK >> 25] = (u32)(uintptr_t)ptr | 3;	/* resident */
		ptr[0] = (u32)(uintptr_t)pt | 3;
		pt[0] = page | 0x41;		/* resident, noncached serialized */
		regs[0] = tab;
		regs[1] = 0x8000;				/* TC: E, 4 KB */
		regs[2] = 0x007fc040;		/* TT0: 0-7FFFFFFF, uncached */
		regs[3] = 0x803fc040;		/* TT1: 80000000-BFFFFFFF */
		r = mmu_test040(regs, VA_OK, VA_BAD, &out);
	}

	if (r < 0)
		result_text = "no MMU (MMU instructions trap)";
	else if (out != PATTERN_R)
		result_text = "FAILED: no translation (EC CPU, or an accelerator "
		    "mode without MMU?)";
	else if (pg[0] != PATTERN_W)
		result_text = "FAILED: write through the mapping lost";
	else if (r == 0)
		result_text = "FAILED: invalid page did not fault";
	else {
		result_text = "passed (translation, write, fault)";
		cpuinfo.mmu_ok = 1;
	}
	return cpuinfo.mmu_ok;
}
