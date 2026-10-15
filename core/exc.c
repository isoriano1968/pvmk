/*
 * Exception reporting. Faults print a decoded report on the console (and
 * into the boot log) and drop into the shell on a fresh stack.
 */
#include "pvmk/kprintf.h"
#include "pvmk/log.h"
#include "pvmk/mk.h"
#include "pvmk/vcpu.h"

static struct excregs last;
static u32 last_ssp;
static u32 last_fault_addr;
static int last_valid, last_has_addr;

const char *
exc_name(u32 vec)
{
	static const char *const names[] = {
		[2] = "bus/access error", [3] = "address error",
		[4] = "illegal instruction", [5] = "zero divide",
		[6] = "CHK", [7] = "TRAPV", [8] = "privilege violation",
		[9] = "trace", [10] = "line A", [11] = "line F",
		[13] = "coprocessor protocol violation", [14] = "format error",
		[15] = "uninitialized interrupt", [24] = "spurious interrupt",
		[48] = "FP branch/set on unordered", [49] = "FP inexact",
		[50] = "FP divide by zero", [51] = "FP underflow",
		[52] = "FP operand error", [53] = "FP overflow",
		[54] = "FP signaling NaN", [55] = "FP unimplemented data type",
		[56] = "MMU configuration error", [60] = "unimplemented EA",
		[61] = "unimplemented integer instruction",
	};

	if (vec >= 25 && vec <= 31)
		return vec == 31 ? "level 7 interrupt (NMI)" : "autovector interrupt";
	if (vec >= 32 && vec <= 47)
		return "TRAP";
	if (vec < sizeof(names) / sizeof(names[0]) && names[vec])
		return names[vec];
	return "unassigned vector";
}

static void
show(const struct excregs *r, u32 ssp, int has_addr, u32 addr)
{
	u32 vec = (r->fmtvec & 0xfff) >> 2, fmt = r->fmtvec >> 12;
	int i;

	kprintf("*** exception: %s (vector %u, frame format %x)\n",
	    exc_name(vec), vec, fmt);
	if (vec >= 32 && vec <= 47)
		kprintf("    trap #%u\n", vec - 32);
	kprintf("    PC %08x  SR %04x", r->pc, r->sr);
	if (has_addr)
		kprintf("  fault address %08x", addr);
	kprintf("\n");
	for (i = 0; i < 8; i++)
		kprintf("    D%u %08x%s", i, r->d[i], i % 4 == 3 ? "\n" : "");
	for (i = 0; i < 8; i++)
		kprintf("    A%u %08x%s", i, i < 7 ? r->a[i] :
		    r->sr & 0x2000 ? ssp : r->a[7], i % 4 == 3 ? "\n" : "");
	if (r->sr & 0x2000)
		kprintf("    USP %08x\n", r->a[7]);
}

void
exception(struct excregs *r)
{
	u32 vec = (r->fmtvec & 0xfff) >> 2, fmt = r->fmtvec >> 12;
	const u8 *frame = (const u8 *)&r->sr;

	if (idle_active && vec >= 24 && vec <= 31) {
		/* the interrupt that ends a guest's STOP: keep it pending */
		idle_vec = vec;
		r->sr |= 0x0700;
		return;
	}
	if (guest.running && (r->sr & 0x2000) && vec >= 25 && vec <= 31) {
		/*
		 * An interrupt for the guest taken in SORIX-LDOM: the guest
		 * trapped at an interrupt level below it and the interrupt
		 * came in before exc_entry masked them. Keep it pending; it is
		 * taken again when the guest runs.
		 */
		r->sr |= 0x0700;
		return;
	}
	if (!(r->sr & 0x2000) && guest.running) {
		vcpu_exception(r);		/* from the guest (user mode) */
		return;
	}
	if (probe_bus_resume && (vec == 2 || vec == 3))
		probe_abort();			/* MMU self-test: expected fault */
	if (probe_resume && (vec == 4 || vec == 8 || vec == 10 || vec == 11)) {
		r->pc = probe_resume;
		return;
	}

	con_interactive(0);
	last = *r;
	last_ssp = (u32)(uintptr_t)&r->sr;	/* SSP: the exception frame */
	last_valid = 1;
	last_has_addr = 1;
	switch (fmt) {
	case 0x4:	/* 060 access error: FA at +8 */
	case 0x2:	/* address for CHK2/TRAPcc etc. */
		last_fault_addr = *(const u32 *)(frame + 8);
		break;
	case 0x7:	/* 040 access error: FA at +0x14 */
		last_fault_addr = *(const u32 *)(frame + 0x14);
		break;
	case 0xa:	/* 030 short/long bus fault: data fault address at +0x10 */
	case 0xb:
		last_fault_addr = *(const u32 *)(frame + 0x10);
		break;
	default:
		last_has_addr = 0;
		break;
	}
	kprintf("\n");
	show(r, last_ssp, last_has_addr, last_fault_addr);
	kprintf("    entering shell\n");
	restart(sstack_top(), shell_main);
}

void
exc_show_last(void)
{
	if (!last_valid) {
		kprintf("no exception recorded since boot\n");
		return;
	}
	show(&last, last_ssp, last_has_addr, last_fault_addr);
}
