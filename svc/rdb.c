/*
 * RDB partition tables (devices/hardblocks.h): RDSK in one of the first 16
 * blocks, then a chain of PART blocks; each block's first SummedLongs
 * longs sum to zero.
 */
#include "pvmk/kprintf.h"
#include "pvmk/rdb.h"
#include "pvmk/scsi.h"

#define RDB_LOCATION_LIMIT	16
#define END			0xffffffff

/* RDSK and PART field offsets */
#define RDB_SUMMED	4
#define RDB_PARTLIST	28
#define PB_NEXT		16
#define PB_FLAGS	20
#define PB_NAME		36
#define DE_BASE		128
#define DE_TABLESIZE	(DE_BASE + 0)
#define DE_SIZEBLOCK	(DE_BASE + 4)
#define DE_SURFACES	(DE_BASE + 12)
#define DE_SECPERBLK	(DE_BASE + 16)
#define DE_BPT		(DE_BASE + 20)
#define DE_RESERVED	(DE_BASE + 24)
#define DE_LOWCYL	(DE_BASE + 36)
#define DE_HIGHCYL	(DE_BASE + 40)
#define DE_BOOTPRI	(DE_BASE + 60)
#define DE_DOSTYPE	(DE_BASE + 64)
#define DE_BOOTBLOCKS	(DE_BASE + 76)

/* FFS root block */
#define T_HEADER	2
#define ST_ROOT		1
#define O_NAME		432
#define O_SECTYPE	508

struct partition parts[MAXPARTS];
u32 nparts;

static u8 blk[512];

static u32
rd32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static int
sum_ok(const u8 *b)
{
	u32 n = rd32(b + RDB_SUMMED), sum = 0, i;

	if (n < 2 || n > 128)
		return 0;
	for (i = 0; i < n; i++)
		sum += rd32(b + i * 4);
	return sum == 0;
}

void
dostype_str(u32 t, char *buf)
{
	int i;

	for (i = 0; i < 3; i++) {
		int c = (int)(t >> (24 - 8 * i)) & 0xff;

		buf[i] = (char)(c >= 0x20 && c < 0x7f ? c : '?');
	}
	buf[3] = '\\';
	buf[4] = (char)('0' + (t & 0xff) % 10);
	buf[5] = 0;
	if ((t & 0xff) >= 10) {		/* e.g. PFS\3 vs. raw values */
		buf[4] = "0123456789abcdef"[(t >> 4) & 0xf];
		buf[5] = "0123456789abcdef"[t & 0xf];
		buf[6] = 0;
	}
}

int
dostype_is_amigaos(u32 t)
{
	switch (t >> 8) {
	case 0x444f53:		/* DOS: OFS/FFS variants */
	case 0x504653:		/* PFS */
	case 0x534653:		/* SFS */
	case 0x504453:		/* PDS (PFS variant) */
		return 1;
	}
	return 0;
}

const char *
dostype_kind(u32 t)
{
	if (dostype_is_amigaos(t))
		return "AmigaOS";
	if ((t >> 8) == 0x554e49)	/* UNI: AMIX */
		return "AMIX";
	if (t == 0x50564d00)		/* PVM\0: this microkernel */
		return "SORIX-LDOM";
	return "-";
}

int
part_read(const struct partition *p, u32 b, u32 count, void *buf)
{
	if (b + count > p->nblocks || p->target < 0 ||
	    p->target >= SCSI_MAXTARGET)
		return SCSI_EIO;
	return disk_read(&sdisks[(u32)p->target], p->start + b, count, buf);
}

/* Volume name from the FFS/OFS root block, if this is a DOS\x partition. */
static void
read_volume(struct partition *p, rdb_read_fn rd, void *ctx)
{
	u32 root = (p->nblocks + 1) / 2, sum = 0, i, n;

	p->volume[0] = 0;
	if ((p->dostype >> 8) != 0x444f53 || rd(ctx, p->start + root, blk))
		return;
	for (i = 0; i < 512; i += 4)
		sum += rd32(blk + i);
	if (sum || rd32(blk) != T_HEADER || rd32(blk + O_SECTYPE) != ST_ROOT)
		return;
	n = blk[O_NAME] < 30 ? blk[O_NAME] : 30;
	for (i = 0; i < n; i++)
		p->volume[i] = (char)blk[O_NAME + 1 + i];
	p->volume[n] = 0;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

int
rdb_parse(rdb_read_fn rd, void *ctx, const char *device, u32 unit, int target)
{
	u32 b, guard = 0, first = nparts, i;
	int found = 0, rdsk = -1;

	for (b = 0; b < RDB_LOCATION_LIMIT; b++)
		if (!rd(ctx, b, blk) && rd32(blk) == 0x5244534b && sum_ok(blk)) {
			rdsk = (int)b;
			break;
		}
	if (rdsk < 0)
		return -1;
	for (b = rd32(blk + RDB_PARTLIST); b != END && guard++ < 64;
	    b = rd32(blk + PB_NEXT)) {
		struct partition q, *p = NULL;
		u32 n, cylblocks, sizeblock;

		if (rd(ctx, b, blk) || rd32(blk) != 0x50415254 || !sum_ok(blk)) {
			kprintf("rdb: %s unit %u: bad PART block %u\n", device, unit,
			    b);
			break;
		}
		memset(&q, 0, sizeof(q));
		n = blk[PB_NAME] < 31 ? blk[PB_NAME] : 31;
		for (i = 0; i < n; i++)
			q.name[i] = (char)blk[PB_NAME + 1 + i];
		for (i = 0; device[i] && i < sizeof(q.device) - 1; i++)
			q.device[i] = device[i];
		q.unit = unit;
		q.target = (s8)target;
		q.flags = rd32(blk + PB_FLAGS);
		/* geometry in blocks of SizeBlock longs; convert to 512 bytes */
		sizeblock = rd32(blk + DE_SIZEBLOCK) * 4 / 512;
		if (!sizeblock)
			sizeblock = 1;
		cylblocks = rd32(blk + DE_SURFACES) * rd32(blk + DE_BPT) *
		    sizeblock;
		q.start = rd32(blk + DE_LOWCYL) * cylblocks;
		q.nblocks = (rd32(blk + DE_HIGHCYL) - rd32(blk + DE_LOWCYL) + 1) *
		    cylblocks;
		q.bootpri = (s32)rd32(blk + DE_BOOTPRI);
		q.dostype = rd32(blk + DE_TABLESIZE) >= 16 ?
		    rd32(blk + DE_DOSTYPE) : 0x444f5300;
		q.bootblocks = rd32(blk + DE_TABLESIZE) >= 19 ?
		    rd32(blk + DE_BOOTBLOCKS) : 0;

		for (i = 0; i < nparts; i++)
			if (parts[i].unit == unit && str_eq(parts[i].name, q.name) &&
			    str_eq(parts[i].device, q.device))
				p = &parts[i];
		if (!p) {
			if (nparts == MAXPARTS)
				break;
			p = &parts[nparts++];
		}
		*p = q;
		found++;
	}
	/* volume names last: read_volume() reuses the block buffer */
	for (i = 0; i < nparts; i++)
		if (parts[i].unit == unit && str_eq(parts[i].device, device) &&
		    (i >= first || parts[i].target == target))
			read_volume(&parts[i], rd, ctx);
	return found;
}

static int
native_read(void *ctx, u32 b, void *buf)
{
	return disk_read(ctx, b, 1, buf) ? -1 : 0;
}

/*
 * Native scan of the A3000 internal SCSI. Kickstart's scsi.device numbers
 * its units by target ID, so partitions stage0 already found through
 * scsi.device are updated in place and become readable after takeover.
 */
int
rdb_scan(int verbose)
{
	u32 t;

	for (t = 0; t < SCSI_MAXTARGET; t++)
		if (sdisks[t].present && sdisks[t].ready && sdisks[t].type == 0 &&
		    rdb_parse(native_read, &sdisks[t], "scsi.device", t, (int)t) < 0
		    && verbose)
			kprintf("rdb: target %u: no RDB\n", t);
	if (verbose)
		kprintf("rdb: %u partition(s)\n", nparts);
	return (int)nparts;
}

void
part_show(void)
{
	u32 i;

	if (!nparts) {
		kprintf("no partitions (scsi: rescan)\n");
		return;
	}
	kprintf("  %-8s %-18s %4s %-7s %7s %4s  %-10s %s\n", "name", "device",
	    "unit", "dostype", "size", "pri", "kind", "volume");
	for (i = 0; i < nparts; i++) {
		const struct partition *p = &parts[i];
		char dt[8];

		dostype_str(p->dostype, dt);
		kprintf("  %-8s %-18s %4u %-7s %5uMB %4d  %-10s %s%s\n", p->name,
		    p->device, p->unit, dt, p->nblocks / 2048, p->bootpri,
		    dostype_kind(p->dostype), p->volume,
		    p->flags & PBFF_BOOTABLE ? "" : " (not bootable)");
	}
}

static int
upper(int c)
{
	return c >= 'a' && c <= 'z' ? c - 32 : c;
}

struct partition *
part_find(const char *name)
{
	u32 i;

	for (i = 0; i < nparts; i++) {
		const char *a = parts[i].name, *b = name;

		while (*a && upper((u8)*a) == upper((u8)*b))
			a++, b++;
		if (!*a && (!*b || (*b == ':' && !b[1])))
			return &parts[i];
	}
	return NULL;
}
