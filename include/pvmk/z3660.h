/*
 * Z3660 accelerator: disks (HDFs on its SD card) through the Z3660's
 * piscsi mailbox, after takeover.
 */
#ifndef PVMK_Z3660_H
#define PVMK_Z3660_H

#include "pvmk/types.h"

#define Z3660_MANUFACTURER	0x144b
#define Z3660_PRODUCT		0x01	/* the combo board: SCSI, network, RTG */
#define Z3660_RAM_PRODUCT	0x02	/* its Zorro III RAM board */
#define Z3660_FIXED		0x10000000	/* combo window when not autoconfigured */
#define Z3660_UNITS		18	/* units 0-7 and 10-17 */

struct z3660_unit {
	u8 present;
	u32 bsize, blocks;
};

extern struct z3660_unit z3660_units[Z3660_UNITS];

/* 0 if the board is in the captured expansion board list. */
int z3660_init(void);
u32 z3660_board(void);			/* board address, 0 if absent */
int z3660_scan(int verbose);		/* number of units found */
int z3660_read(u32 unit, u32 lba, u32 count, void *buf);	/* 512 bytes */
int z3660_write(u32 unit, u32 lba, u32 count, const void *buf);

#endif /* PVMK_Z3660_H */
