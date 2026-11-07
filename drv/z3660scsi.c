/*
 * Z3660 disks through the piscsi mailbox (Z3660 firmware, PiStorm
 * heritage): 32-bit registers at board + 0x2000. A read is synchronous: set
 * the unit, the byte offset, length and buffer address, then write the unit
 * number to the "read bytes" register; the write completes when the data is
 * in place. If the firmware used its staging buffer (USED_DMA != 0), the
 * data is at board + 0x80000 instead. There are no interrupts and no SCSI
 * command blocks.
 *
 * The microkernel runs with the data cache off, so no cache maintenance is
 * needed around the transfer.
 */
#include "pvmk/bootcap.h"
#include "pvmk/kprintf.h"
#include "pvmk/z3660.h"

#define REGS_OFFSET	0x2000
#define STAGING_OFFSET	0x80000
#define MAX_XFER	65536

#define R_WRITE		0x00		/* block write: LBA in WRITE_ADDR1 */
#define R_DRVNUM	0x08
#define R_DRVTYPE	0x0c
#define R_READ_ADDR1	0x20		/* byte offset (low 32 bits) */
#define R_READ_ADDR2	0x24		/* length */
#define R_READ_ADDR3	0x28		/* buffer address */
#define R_READ_ADDR4	0x2c		/* byte offset (high 32 bits, READ64) */
#define R_READ64	0x64
#define R_READBYTES	0x88
#define R_DRVNUMX	0x90
#define R_USED_DMA	0x9c
#define R_WRITE_ADDR1	0x240		/* LBA */
#define R_WRITE_ADDR2	0x244		/* length */
#define R_WRITE_ADDR3	0x248		/* buffer address */
#define DIRECT_BASE	0x08000000	/* below: not the card's RAM, staged */
#define R_BLOCKSIZE0	0x200		/* units 0-7 */
#define R_BLOCKS0	0x220
#define R_BLOCKSIZE10	0x250		/* units 10-17 */
#define R_BLOCKS10	0x270

struct z3660_unit z3660_units[Z3660_UNITS];

static u32 board;
static u32 board_moved;		/* z3reloc: the board's new address */

#define REG(off)	(*(volatile u32 *)(uintptr_t)(board + REGS_OFFSET + (off)))

int
z3660_init(void)
{
	u32 i;

	if (board_moved) {		/* the captured list has the old address */
		board = board_moved;
		return 0;
	}
	board = 0;
	for (i = 0; i < bootcap.ncd; i++) {
		const u8 *cd = bootcap.configdev[i];

		if (PEEK16(cd, 20) == Z3660_MANUFACTURER &&
		    PEEK8(cd, 17) == Z3660_PRODUCT) {
			board = PEEK32(cd, 32);	/* cd_BoardAddr */
			return 0;
		}
	}
	/*
	 * Not autoconfigured (autoconfig_rtg NO): the combo window is at its
	 * fixed address. Only probed when the Z3660's RAM board is in the
	 * chain, so a machine without a Z3660 never touches it.
	 */
	for (i = 0; i < bootcap.ncd; i++) {
		const u8 *cd = bootcap.configdev[i];

		if (PEEK16(cd, 20) == Z3660_MANUFACTURER &&
		    PEEK8(cd, 17) == Z3660_RAM_PRODUCT) {
			board = Z3660_FIXED;
			kprintf("z3660: combo board not autoconfigured, using the "
			    "fixed window at %08x\n", board);
			return 0;
		}
	}
	return -1;
}

void
z3660_relocated(u32 b)
{
	board_moved = board = b;
}

u32
z3660_board(void)
{
	return board;
}

static int
valid_unit(u32 u)
{
	return u < Z3660_UNITS && (u < 8 || u >= 10);
}

static u32
size_reg(u32 u, u32 base0, u32 base10)
{
	return u < 8 ? base0 + u * 4 : base10 + (u - 10) * 4;
}

int
z3660_scan(int verbose)
{
	u32 u;
	int found = 0;

	if (!board)
		return 0;
	for (u = 0; u < Z3660_UNITS; u++) {
		struct z3660_unit *z = &z3660_units[u];
		u32 bs;

		memset(z, 0, sizeof(*z));
		if (!valid_unit(u))
			continue;
		REG(R_DRVNUM) = u;
		if (!REG(R_DRVTYPE))
			continue;
		/*
		 * Block size before block count: the firmware divides by the
		 * block size when reporting blocks.
		 */
		bs = REG(size_reg(u, R_BLOCKSIZE0, R_BLOCKSIZE10));
		if (bs < 512 || bs > 4096 || (bs & (bs - 1)))
			continue;
		z->bsize = bs;
		z->blocks = REG(size_reg(u, R_BLOCKS0, R_BLOCKS10));
		z->present = 1;
		found++;
		if (verbose)
			kprintf("z3660: unit %u: %u blocks x %u = %u MB%s\n", u,
			    z->blocks, bs, z->blocks / (1048576 / bs),
			    bs != 512 ? " (unsupported block size)" : "");
	}
	return found;
}

int
z3660_read(u32 unit, u32 lba, u32 count, void *buf)
{
	u8 *p = buf;

	if (!board || !valid_unit(unit) || !z3660_units[unit].present ||
	    z3660_units[unit].bsize != 512 ||
	    lba + count > z3660_units[unit].blocks)
		return -1;
	while (count) {
		u32 n = count > MAX_XFER / 512 ? MAX_XFER / 512 : count;

		REG(R_DRVNUMX) = unit;
		REG(R_READ_ADDR1) = lba << 9;
		REG(R_READ_ADDR2) = n * 512;
		REG(R_READ_ADDR3) = (u32)(uintptr_t)p;
		if (lba >= 0x800000) {		/* beyond 4 GB: 64-bit offset */
			REG(R_READ_ADDR4) = lba >> 23;
			REG(R_READ64) = unit;
		} else
			REG(R_READBYTES) = unit;
		if (REG(R_USED_DMA))
			memcpy(p, (const void *)(uintptr_t)(board + STAGING_OFFSET),
			    n * 512);
		p += n * 512;
		lba += n;
		count -= n;
	}
	return 0;
}

/*
 * Block write, as the AMIX z3660scsi driver does it (P_WRITE, LBA in
 * WRITE_ADDR1). A buffer outside the card's own RAM is copied to the
 * staging buffer first. Synchronous like the read.
 */
int
z3660_write(u32 unit, u32 lba, u32 count, const void *buf)
{
	const u8 *p = buf;

	if (!board || !valid_unit(unit) || !z3660_units[unit].present ||
	    z3660_units[unit].bsize != 512 ||
	    lba + count > z3660_units[unit].blocks)
		return -1;
	while (count) {
		u32 n = count > MAX_XFER / 512 ? MAX_XFER / 512 : count;

		if ((u32)(uintptr_t)p < DIRECT_BASE)
			memcpy((void *)(uintptr_t)(board + STAGING_OFFSET), p,
			    n * 512);
		REG(R_DRVNUMX) = unit;
		REG(R_WRITE_ADDR1) = lba;
		REG(R_WRITE_ADDR2) = n * 512;
		REG(R_WRITE_ADDR3) = (u32)(uintptr_t)p;
		REG(R_WRITE) = unit;		/* the write completes the op */
		p += n * 512;
		lba += n;
		count -= n;
	}
	return 0;
}
