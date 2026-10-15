/*
 * CPU identification by probing (after takeover, with pvmk's vectors), not
 * from Kickstart's AttnFlags: those report a 68060 as 68040 until
 * 68060.library is loaded.
 */
#include "pvmk/kprintf.h"
#include "pvmk/mk.h"

struct cpuinfo cpuinfo;

void
cpu_probe(void)
{
	u32 pcr = 0;

	if (probe_pcr(&pcr)) {
		cpuinfo.model = 60;
		cpuinfo.pcr = pcr;
	} else if (probe_urp())
		cpuinfo.model = 40;
	else if (probe_cacr_fd())
		cpuinfo.model = 30;
	else
		cpuinfo.model = 20;
	cpuinfo.fpu = probe_fpu();
}

#define PCR_ID(p)	((p) >> 16)		/* 0x0430 full, 0x0431 EC/LC */
#define PCR_DFP		0x00000002		/* FPU disabled */

static const char *
fpu_desc(void)
{
	if (cpuinfo.model == 60 && PCR_ID(cpuinfo.pcr) == 0x0430)
		return cpuinfo.pcr & PCR_DFP ?
		    "68060 internal (disabled, PCR.DFP)" : "68060 internal";
	if (cpuinfo.fpu < 0)
		return "none";
	if (cpuinfo.model == 40)
		return "68040 internal";
	if (cpuinfo.fpu == 0x18)
		return "68881";
	if (cpuinfo.fpu == 0x38)
		return "68882";
	return "6888x";
}

void
cpu_show(void)
{
	kprintf("cpu: 680%u", cpuinfo.model);
	if (cpuinfo.model == 60)
		kprintf(" (%s, revision %u, PCR %08x)",
		    PCR_ID(cpuinfo.pcr) == 0x0431 ? "EC/LC" : "full",
		    (cpuinfo.pcr >> 8) & 0xff, cpuinfo.pcr);
	kprintf(", fpu %s, mmu %s\n", fpu_desc(),
	    cpuinfo.model == 30 ? "68030 PMMU" :
	    cpuinfo.model >= 40 ? "040/060" : "none");
	kprintf("cpu: MMU self-test %s\n", mmu_result());
	kprintf("cpu: VBR %08x, CACR %08x, SR %04x\n", get_vbr(), get_cacr(),
	    get_sr());
	if (cpuinfo.model == 20)
		kprintf("cpu: warning: 68020 is not supported\n");
}
