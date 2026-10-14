/*
 * SCSI: A3000 internal controller (WD33C93A behind the SDMAC), polled, and
 * the disk layer on top of it.
 */
#ifndef PVMK_SCSI_H
#define PVMK_SCSI_H

#include "pvmk/types.h"

#define SCSI_OK		0
#define SCSI_NODEV	(-1)		/* selection timeout */
#define SCSI_ECHECK	(-2)		/* CHECK CONDITION; sense in scsi_sense */
#define SCSI_EIO	(-3)		/* bus or controller error */
#define SCSI_ETIMEOUT	(-4)

#define SCSI_HOST_ID	7
#define SCSI_DMA_BLOCKS	16		/* bounce buffer: 8 KB of chip RAM */
#define SCSI_MAXTARGET	7

/* drv/wd33c93.c */
void scsi_dma_buffer(u32 chipmem, u32 size);	/* call before scsi_init */
int  scsi_init(void);			/* 0 if the controller responds */
int  scsi_cmd(u32 target, u32 lun, const u8 *cdb, void *buf, u32 len,
	int data_in);
extern u8 scsi_sense[18];
extern u32 scsi_last;
const char *scsi_strerror(int err);

/* svc/disk.c */
struct sdisk {
	u8 present;
	u8 target, lun;
	u8 type;			/* peripheral device type, 0 = disk */
	u8 removable;
	char vendor[9], product[17], rev[5];
	u32 blocks, bsize;		/* from READ CAPACITY */
	int ready;
};

extern struct sdisk sdisks[SCSI_MAXTARGET];

int  disk_scan(int verbose);		/* number of devices found */
int  disk_read(const struct sdisk *d, u32 lba, u32 count, void *buf);

#endif /* PVMK_SCSI_H */
