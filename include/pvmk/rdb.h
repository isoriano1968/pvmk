/*
 * Amiga RigidDiskBlock partition tables on SCSI disks.
 */
#ifndef PVMK_RDB_H
#define PVMK_RDB_H

#include "pvmk/types.h"

#define MAXPARTS	24
#define PBFF_BOOTABLE	1
#define PBFF_NOMOUNT	2

/* Native drivers that can read a partition after takeover */
#define NATIVE_NONE	0		/* only through Kickstart, at stage0 */
#define NATIVE_A3000	1		/* A3000 internal SCSI, unit = target */
#define NATIVE_Z3660	2		/* Z3660 piscsi, unit = piscsi unit */

struct partition {
	char name[32];			/* DOS device name, e.g. DH0 */
	char device[24];		/* Kickstart driver, e.g. scsi.device */
	u32 unit;			/* its unit number */
	u8 native;			/* NATIVE_*: reachable after takeover */
	u32 dostype;
	u32 start, nblocks;		/* 512-byte blocks on the disk */
	s32 bootpri;
	u32 flags;			/* PBFF_* */
	u32 bootblocks;			/* DE_BOOTBLOCKS, 0 if absent */
	char volume[32];		/* AmigaDOS volume name, if readable */
};

extern struct partition parts[MAXPARTS];
extern u32 nparts;

typedef int (*rdb_read_fn)(void *ctx, u32 blk, void *buf);	/* 0 = ok */

/*
 * Parse the RDB of one disk through rd and add (or, for the same device,
 * unit and name, update) its partitions. native says which native driver
 * reaches the disk after takeover (NATIVE_*). Returns the number of
 * partitions found on this disk, -1 if there is no RDB.
 */
int  rdb_parse(rdb_read_fn rd, void *ctx, const char *device, u32 unit,
	int native);
int  rdb_scan(int verbose);		/* disks of all native drivers */
const char *native_name(u32 native);
void part_show(void);
struct partition *part_find(const char *name);	/* "dh0" or "DH0:" */
const char *dostype_kind(u32 dostype);	/* "AmigaOS", "AMIX", ... */
int  dostype_is_amigaos(u32 dostype);
void dostype_str(u32 dostype, char *buf);	/* "DOS\1" */

/* Read count blocks at a partition-relative block number. */
int  part_read(const struct partition *p, u32 blk, u32 count, void *buf);

#endif /* PVMK_RDB_H */
