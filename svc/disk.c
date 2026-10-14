/*
 * SCSI disk layer: device scan (INQUIRY, TEST UNIT READY, READ CAPACITY)
 * and block reads (READ(10)).
 */
#include "pvmk/kprintf.h"
#include "pvmk/scsi.h"

struct sdisk sdisks[SCSI_MAXTARGET];

static const char *
devtype(u8 t)
{
	switch (t) {
	case 0:	return "disk";
	case 1:	return "tape";
	case 4:	return "WORM";
	case 5:	return "CD-ROM";
	case 7:	return "optical";
	}
	return "other";
}

static void
copy_trim(char *dst, const u8 *src, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		dst[i] = src[i] >= 0x20 && src[i] < 0x7f ? (char)src[i] : ' ';
	while (n && dst[n - 1] == ' ')
		n--;
	dst[n] = 0;
}

/* TEST UNIT READY, retried through unit attention after reset. */
static int
unit_ready(u32 target)
{
	u8 tur[6] = { 0 };
	int i, err = SCSI_EIO;

	for (i = 0; i < 4; i++) {
		err = scsi_cmd(target, 0, tur, NULL, 0, 1);
		if (err != SCSI_ECHECK || (scsi_sense[2] & 0x0f) != 0x06)
			break;		/* retry only on UNIT ATTENTION */
	}
	return err;
}

int
disk_scan(int verbose)
{
	u32 t;
	int found = 0;

	for (t = 0; t < SCSI_MAXTARGET; t++) {
		struct sdisk *d = &sdisks[t];
		u8 inq[6] = { 0x12, 0, 0, 0, 36, 0 };
		u8 rcap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		u8 data[36];
		int err;

		memset(d, 0, sizeof(*d));
		d->target = (u8)t;
		memset(data, 0, sizeof(data));
		err = scsi_cmd(t, 0, inq, data, sizeof(data), 1);
		if (err == SCSI_NODEV)
			continue;
		if (err) {
			kprintf("scsi: target %u: INQUIRY: %s (%08x)\n", t,
			    scsi_strerror(err), scsi_last);
			continue;
		}
		if ((data[0] & 0xe0) != 0)	/* LUN 0 not supported */
			continue;
		d->present = 1;
		d->type = data[0] & 0x1f;
		d->removable = (data[1] & 0x80) != 0;
		copy_trim(d->vendor, data + 8, 8);
		copy_trim(d->product, data + 16, 16);
		copy_trim(d->rev, data + 32, 4);
		found++;

		if (unit_ready(t) == SCSI_OK) {
			u8 cap[8];

			if (scsi_cmd(t, 0, rcap, cap, sizeof(cap), 1) == SCSI_OK) {
				d->blocks = ((u32)cap[0] << 24 | (u32)cap[1] << 16 |
				    (u32)cap[2] << 8 | cap[3]) + 1;
				d->bsize = (u32)cap[4] << 24 | (u32)cap[5] << 16 |
				    (u32)cap[6] << 8 | cap[7];
				d->ready = d->bsize == 512;
			}
		}
		if (verbose) {
			kprintf("scsi: target %u: %-8s %-16s %-4s %s%s", t,
			    d->vendor, d->product, d->rev, devtype(d->type),
			    d->removable ? " (removable)" : "");
			if (d->blocks)
				kprintf(", %u blocks x %u = %u MB%s", d->blocks,
				    d->bsize, d->blocks / (1048576 / d->bsize),
				    d->bsize != 512 ? " (unsupported size)" : "");
			else
				kprintf(", not ready");
			kprintf("\n");
		}
	}
	return found;
}

int
disk_read(const struct sdisk *d, u32 lba, u32 count, void *buf)
{
	u8 *p = buf;
	int err = SCSI_OK, tries;

	if (!d->ready)
		return SCSI_EIO;
	while (count && !err) {
		u32 n = count > SCSI_DMA_BLOCKS ? SCSI_DMA_BLOCKS : count;
		u8 cdb[10] = { 0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16),
		    (u8)(lba >> 8), (u8)lba, 0, (u8)(n >> 8), (u8)n, 0 };

		for (tries = 0; tries < 3; tries++)
			if ((err = scsi_cmd(d->target, d->lun, cdb, p, n * 512, 1))
			    != SCSI_ECHECK)
				break;
		lba += n;
		count -= n;
		p += n * 512;
	}
	return err;
}
