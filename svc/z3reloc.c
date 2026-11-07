/*
 * z3reloc.c -- "z3reloc": move the Z3660's Zorro III RAM and RTG board below
 * the AMIX kernel's virtual space (AMIX mode, agreed with the Z3660's author
 * 2026-11-07; the firmware side is the fork's z3_map.c, AMIX-MODE.md there).
 *
 * The firmware (amix_relocate YES) has registers in the RTG board:
 *   0x4E0 RAM base, 0x4E4 RTG base (the 060 addresses wanted, then active)
 *   0x4E8 W "COMT" commit / "DEFT" back to the Amiga bases; R "AMX1"
 *   0x4EC status: 0 Amiga bases, 1 committed, 2/3 bus reset, < 0 refused
 *   0x4F0 W "BRST": FPGA bus reset (REG0 bit 31, 10 ms, after the ack); R 1
 * Steps: the bus reset (z3reloc_pulse: interrupts off, no bus cycle for
 * Z3R_WAIT_MS), then autoconfig at 0xFF000000 as the FPGA offers the boards
 * (the RAM board first, product 2, then the RTG, product 1): RAM to
 * 0x20000000, RTG to 0x30000000; then the bases and COMT to the registers
 * at the RTG's new place.  The firmware goes back to the Amiga bases by
 * itself at the next Amiga reset.
 *
 * boot afterwards hands AMIX the new bases (z3reloc_bootinfo); a reset
 * brings the Amiga bases back.
 */
#include "pvmk/types.h"
#include "pvmk/bootcap.h"
#include "pvmk/kprintf.h"
#include "pvmk/log.h"
#include "pvmk/z3660.h"
#include "pvmk/z3reloc.h"

#define Z3R_RAM_NEW	0x20000000u
#define Z3R_RTG_NEW	0x30000000u
#define Z3R_WAIT_MS	40		/* >= 0.2 + 10 ms pulse + core 0's loop, with margin */

#define R_RAM		0x4E0
#define R_RTG		0x4E4
#define R_CMD		0x4E8
#define R_STATUS	0x4EC
#define R_RESET		0x4F0
#define CMD_COMMIT	0x434F4D54u	/* "COMT" */
#define CMD_BUSRESET	0x42525354u	/* "BRST" */
#define AMIX_ID		0x414D5831u	/* "AMX1" */

#define AC_BASE		0xFF000000u

u32 z3reloc_done;

void z3reloc_pulse(volatile u32 *reg, u32 val, u32 n);
u32  z3reloc_spin(u32 n);
int  safe_write16(u32 addr, u32 val);
int  safe_read32(u32 addr, u32 *val);
int  safe_copy(void *dst, const void *src, u32 n);

#define REG(b, off)	(*(volatile u32 *)(uintptr_t)((b) + (off)))

static u32
ms_since(u32 s0, u32 m0)
{
	u32 s, m;

	time_now(&s, &m);
	return (s - s0) * 1000 + m - m0;
}

/* wait-loop turns per millisecond, timed against the CIA's TOD (>= 0.5 s) */
static u32
loops_per_ms(void)
{
	u32 n = 1u << 20, s0, m0, ms;

	time_now(&s0, &m0);		/* the first call starts the clock */
	for (;;) {
		time_now(&s0, &m0);
		z3reloc_spin(n);
		ms = ms_since(s0, m0);
		if (ms >= 500 || n >= 1u << 30)
			break;
		n <<= 1;
	}
	return ms ? n / ms : 0;
}

/* the high nibble of autoconfig register off, -1 if nothing answers */
static int
ac_nibble(u32 off)
{
	u8 b;

	if (safe_copy(&b, (const void *)(uintptr_t)(AC_BASE + off), 1))
		return -1;
	return b >> 4;
}

/* the board waiting at 0xFF000000: its product number, -1 if none */
static int
ac_product(void)
{
	int t = ac_nibble(0x000), hi = ac_nibble(0x004), lo = ac_nibble(0x104);
	int m1 = ac_nibble(0x010), m2 = ac_nibble(0x110), m3 = ac_nibble(0x014),
	    m4 = ac_nibble(0x114);
	u32 manuf;

	if (t < 0 || hi < 0 || lo < 0 || m1 < 0 || m2 < 0 || m3 < 0 || m4 < 0)
		return -1;
	if ((t & 0xc) != 0x8)		/* er_Type 10xx: Zorro III */
		return -1;
	manuf = ~((u32)m1 << 12 | (u32)m2 << 8 | (u32)m3 << 4 | (u32)m4) & 0xffff;
	if (manuf != Z3660_MANUFACTURER)
		return -1;
	return ~(hi << 4 | lo) & 0xff;
}

/* the board waiting is product p: configure it at base */
static int
ac_place(int p, u32 base, const char *what)
{
	int got = ac_product();

	if (got != p) {
		kprintf("z3reloc: autoconfig: wanted the %s (product %d), found %d\n",
		    what, p, got);
		return -1;
	}
	if (safe_write16(AC_BASE + 0x44, base >> 16)) {
		kprintf("z3reloc: autoconfig: bus error writing the %s's base\n", what);
		return -1;
	}
	kprintf("z3reloc: %s configured at %08x\n", what, base);
	return 0;
}

static u32
ram_board(void)
{
	u32 i;

	for (i = 0; i < bootcap.ncd; i++) {
		const u8 *cd = bootcap.configdev[i];

		if (PEEK16(cd, 20) == Z3660_MANUFACTURER &&
		    PEEK8(cd, 17) == Z3660_RAM_PRODUCT)
			return PEEK32(cd, 32);
	}
	return 0;
}

void
cmd_z3reloc(int argc, char **argv)
{
	u32 rtg, ram, id, st, lpm, n, i, ok;
	u32 probe[4], again[4];

	(void)argv;
	if (z3reloc_done) {
		kprintf("z3reloc: already done (RAM %08x, RTG %08x); reset to go back\n",
		    Z3R_RAM_NEW, Z3R_RTG_NEW);
		return;
	}
	if (argc > 1) {
		kprintf("usage: z3reloc\n");
		return;
	}
	if (z3660_init() < 0 || !(rtg = z3660_board())) {
		kprintf("z3reloc: no Z3660 board\n");
		return;
	}
	ram = ram_board();
	if (!ram) {
		kprintf("z3reloc: the Z3660's RAM board is not configured (autoconfig_ram NO?)\n");
		return;
	}
	if (safe_read32(rtg + R_CMD, &id) || id != AMIX_ID) {
		kprintf("z3reloc: firmware without AMIX mode, or amix_relocate NO "
		    "(%08x reads %08x)\n", rtg + R_CMD, id);
		return;
	}
	if (REG(rtg, R_RESET) != 1 || REG(rtg, R_STATUS) != 0) {
		kprintf("z3reloc: firmware: bus reset %u, status %d (want 1, 0)\n",
		    REG(rtg, R_RESET), (int)REG(rtg, R_STATUS));
		return;
	}
	if (rtg >= Z3R_RTG_NEW && rtg < Z3R_RTG_NEW + 0x08000000) {
		kprintf("z3reloc: the RTG is already at %08x\n", rtg);
		return;
	}
	for (i = 0; i < 4; i++)		/* Z3 RAM keeps its contents: compare later */
		probe[i] = REG(ram, 0x00100000 + i * 0x01000000);

	lpm = loops_per_ms();
	if (lpm < 1000) {
		kprintf("z3reloc: wait loop calibration failed (%u turns/ms)\n", lpm);
		return;
	}
	n = lpm * Z3R_WAIT_MS;
	kprintf("z3reloc: RAM %08x -> %08x, RTG %08x -> %08x\n", ram, Z3R_RAM_NEW,
	    rtg, Z3R_RTG_NEW);
	kprintf("z3reloc: bus reset, %u turns/ms, off the bus for %u ms\n", lpm,
	    Z3R_WAIT_MS);

	z3reloc_pulse((volatile u32 *)(uintptr_t)(rtg + R_RESET), CMD_BUSRESET, n);

	if (ac_product() < 0) {
		kprintf("z3reloc: no board at %08x after the reset: did it happen?\n",
		    AC_BASE);
		kprintf("z3reloc: status now %d at the old place\n",
		    safe_read32(rtg + R_STATUS, &st) ? -99 : (int)st);
		return;
	}
	z3reloc_done = 1;		/* from here on the old bases are gone */
	if (ac_place(Z3660_RAM_PRODUCT, Z3R_RAM_NEW, "Z3 RAM") ||
	    ac_place(Z3660_PRODUCT, Z3R_RTG_NEW, "RTG")) {
		kprintf("z3reloc: stopped; reset to go back\n");
		return;
	}
	if (ac_product() >= 0)
		kprintf("z3reloc: another Z3660 board still waits at %08x\n", AC_BASE);

	rtg = Z3R_RTG_NEW;
	if (safe_read32(rtg + R_CMD, &id) || id != AMIX_ID) {
		kprintf("z3reloc: the RTG does not answer at %08x (%08x)\n", rtg, id);
		return;
	}
	kprintf("z3reloc: firmware status after the reset: %d\n", (int)REG(rtg, R_STATUS));
	REG(rtg, R_RAM) = Z3R_RAM_NEW;
	REG(rtg, R_RTG) = Z3R_RTG_NEW;
	REG(rtg, R_CMD) = CMD_COMMIT;
	st = REG(rtg, R_STATUS);
	kprintf("z3reloc: commit: status %d, RAM %08x, RTG %08x\n", (int)st,
	    REG(rtg, R_RAM), REG(rtg, R_RTG));
	if (st != 1) {
		kprintf("z3reloc: the firmware refused the bases; reset to go back\n");
		return;
	}
	ok = 1;
	for (i = 0; i < 4; i++) {
		if (safe_read32(Z3R_RAM_NEW + 0x00100000 + i * 0x01000000, &again[i]) ||
		    again[i] != probe[i])
			ok = 0;
	}
	kprintf("z3reloc: Z3 RAM at %08x: contents %s\n", Z3R_RAM_NEW,
	    ok ? "kept" : "DIFFER");
	z3660_relocated(Z3R_RTG_NEW);
	kprintf("z3reloc: done.  The disks go through %08x now (scsi, part, ls);\n"
	    "z3reloc: boot gives AMIX the new bases; a reset brings the Amiga "
	    "bases back\n", Z3R_RTG_NEW);
}

/*
 * After z3reloc, AMIX's bootinfo (amix_start) gets the new bases: the
 * captured ConfigDevs and MemHeaders are AmigaOS's, from before the move.
 * The Z3660's ConfigDevs get their cd_BoardAddr (offset 32) moved, and the
 * Z3 RAM's MemHeader its mh_First/mh_Lower/mh_Upper (offsets 16, 20, 24).
 */
#define CD_BOARDADDR	32
#define MH_FIRST_OFF	16

void
z3reloc_bootinfo(u8 (*autocon)[0x44], u32 ncd, u8 (*memory)[32], u32 nmem)
{
	u32 i, old = 0, a, k;

	if (!z3reloc_done)
		return;
	for (i = 0; i < ncd; i++) {
		u8 *cd = autocon[i];

		if (PEEK16(cd, 20) != Z3660_MANUFACTURER)
			continue;
		if (PEEK8(cd, 17) == Z3660_RAM_PRODUCT) {
			old = PEEK32(cd, CD_BOARDADDR);
			PEEK32(cd, CD_BOARDADDR) = Z3R_RAM_NEW;
		} else if (PEEK8(cd, 17) == Z3660_PRODUCT)
			PEEK32(cd, CD_BOARDADDR) = Z3R_RTG_NEW;
	}
	if (!old)
		return;
	for (i = 0; i < nmem; i++) {
		u8 *mh = memory[i];
		u32 lo = PEEK32(mh, 20);

		if (lo < old || lo >= old + 0x10000000)
			continue;
		for (k = MH_FIRST_OFF; k <= 24; k += 4) {
			a = PEEK32(mh, k);
			if (a >= old && a <= old + 0x10000000)
				PEEK32(mh, k) = a - old + Z3R_RAM_NEW;
		}
		kprintf("boot: Z3 RAM for AMIX at %08x-%08x (moved by z3reloc)\n",
		    PEEK32(mh, 20), PEEK32(mh, 24) - 1);
	}
	kprintf("boot: Z3660 boards for AMIX: RAM %08x, RTG %08x\n", Z3R_RAM_NEW,
	    Z3R_RTG_NEW);
}
