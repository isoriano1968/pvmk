/*
 * mkhdf — build or list an RDB hard disk image (Amiga "RigidDiskBlock"
 * partitioning, as in AmigaDOS devices/hardblocks.h).
 *
 *   mkhdf -o disk.hdf [--heads H] [--secs S] -p SPEC [-p SPEC]...
 *   mkhdf --list disk.hdf
 *
 * SPEC is NAME followed by comma-separated options:
 *   image=FILE      partition contents (e.g. a volume from mkadf --blocks)
 *   size=N[K|M]     partition size if there is no image (or larger than it)
 *   dostype=0xHEX   default: DOS\1, or the image's first four bytes
 *   pri=N           boot priority; makes the partition bootable
 *   bootblocks=N    boot from the partition's first N blocks (DE_BOOTBLOCKS)
 *
 * Cylinders 0-1 hold the RDB; partitions follow in order, each rounded up
 * to whole cylinders (default geometry: 1 head, 32 sectors = 16 KB).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BSIZE		512
#define MAXPART		16
#define RDB_CYLS	2
#define HOST_ID		7

#define PBFF_BOOTABLE	1

struct part {
	char name[32];
	const char *image;
	uint32_t size;			/* bytes */
	uint32_t dostype;
	int32_t pri;
	int bootable;
	uint32_t bootblocks;
	uint32_t lowcyl, highcyl;
};

static void
die(const char *msg, const char *arg)
{
	fprintf(stderr, "mkhdf: ");
	fprintf(stderr, msg, arg);
	fprintf(stderr, "\n");
	exit(1);
}

static uint32_t
rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | p[2] << 8 | p[3];
}

static void
wr32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

/* RDB blocks: the first SummedLongs longs, checksum included, sum to 0. */
static void
rdb_sum(uint8_t *b)
{
	uint32_t n = rd32(b + 4), sum = 0, i;

	wr32(b + 8, 0);
	for (i = 0; i < n; i++)
		sum += rd32(b + i * 4);
	wr32(b + 8, -sum);
}

static int
rdb_ok(const uint8_t *b)
{
	uint32_t n = rd32(b + 4), sum = 0, i;

	if (n < 2 || n > 128)
		return 0;
	for (i = 0; i < n; i++)
		sum += rd32(b + i * 4);
	return sum == 0;
}

static uint32_t
parse_size(const char *s)
{
	char *end;
	unsigned long v = strtoul(s, &end, 0);

	if (*end == 'K' || *end == 'k')
		v *= 1024;
	else if (*end == 'M' || *end == 'm')
		v *= 1024 * 1024;
	else if (*end)
		die("bad size '%s'", s);
	return (uint32_t)v;
}

static long
file_size(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (!f)
		die("cannot open %s", path);
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return n;
}

static void
parse_part(struct part *p, char *spec)
{
	char *opt = strchr(spec, ',');

	memset(p, 0, sizeof(*p));
	p->dostype = 0x444f5301;
	if (opt)
		*opt++ = 0;
	if (!*spec || strlen(spec) > 30)
		die("bad partition name '%s'", spec);
	strcpy(p->name, spec);
	while (opt && *opt) {
		char *next = strchr(opt, ','), *eq;

		if (next)
			*next++ = 0;
		eq = strchr(opt, '=');
		if (eq)
			*eq++ = 0;
		if (!strcmp(opt, "image") && eq)
			p->image = eq;
		else if (!strcmp(opt, "size") && eq)
			p->size = parse_size(eq);
		else if (!strcmp(opt, "dostype") && eq)
			p->dostype = (uint32_t)strtoul(eq, NULL, 0);
		else if (!strcmp(opt, "pri") && eq) {
			p->pri = (int32_t)strtol(eq, NULL, 0);
			p->bootable = 1;
		} else if (!strcmp(opt, "bootblocks") && eq)
			p->bootblocks = (uint32_t)strtoul(eq, NULL, 0);
		else
			die("bad partition option '%s'", opt);
		opt = next;
	}
	if (p->image) {
		long n = file_size(p->image);

		if ((uint32_t)n > p->size)
			p->size = (uint32_t)n;
	}
	if (!p->size)
		die("partition %s needs image= or size=", p->name);
}

static int
build(const char *out, struct part *parts, int nparts, uint32_t heads,
    uint32_t secs)
{
	uint32_t cylbytes = heads * secs * BSIZE, cyl = RDB_CYLS, cyls, i;
	uint8_t *disk;
	size_t total;
	FILE *f;
	int k;

	for (k = 0; k < nparts; k++) {
		parts[k].lowcyl = cyl;
		cyl += (parts[k].size + cylbytes - 1) / cylbytes;
		parts[k].highcyl = cyl - 1;
	}
	cyls = cyl;
	if ((uint32_t)nparts + 1 > heads * secs * RDB_CYLS)
		die("%s", "too many partitions for the RDB area");
	total = (size_t)cyls * cylbytes;
	disk = calloc(1, total);
	if (!disk)
		die("%s", "out of memory");

	/* RigidDiskBlock */
	memcpy(disk, "RDSK", 4);
	wr32(disk + 4, 64);			/* SummedLongs */
	wr32(disk + 12, HOST_ID);
	wr32(disk + 16, BSIZE);
	wr32(disk + 20, 0);			/* Flags */
	wr32(disk + 24, 0xffffffff);		/* BadBlockList */
	wr32(disk + 28, nparts ? 1 : 0xffffffff);	/* PartitionList */
	for (i = 32; i < 60; i += 4)		/* FileSysHeader, DriveInit, ... */
		wr32(disk + i, 0xffffffff);
	wr32(disk + 64, cyls);
	wr32(disk + 68, secs);
	wr32(disk + 72, heads);
	wr32(disk + 76, 1);			/* Interleave */
	wr32(disk + 80, cyls);			/* Park */
	wr32(disk + 96, cyls);			/* WritePreComp */
	wr32(disk + 100, cyls);			/* ReducedWrite */
	wr32(disk + 104, 3);			/* StepRate */
	wr32(disk + 128, 0);			/* RDBBlocksLo */
	wr32(disk + 132, heads * secs * RDB_CYLS - 1);	/* RDBBlocksHi */
	wr32(disk + 136, RDB_CYLS);		/* LoCylinder */
	wr32(disk + 140, cyls - 1);		/* HiCylinder */
	wr32(disk + 144, heads * secs);		/* CylBlocks */
	wr32(disk + 152, (uint32_t)nparts);	/* HighRDSKBlock */
	memcpy(disk + 160, "SORIX   ", 8);	/* DiskVendor */
	memcpy(disk + 168, "TEST DISK       ", 16);
	memcpy(disk + 184, "0.1 ", 4);
	rdb_sum(disk);
	/* the RDB layout above uses rdb_ fields at their hardblocks.h offsets:
	 * Cylinders 64, Sectors 68, Heads 72, ... CylBlocks 144 */

	for (k = 0; k < nparts; k++) {
		uint8_t *pb = disk + (size_t)(k + 1) * BSIZE;
		struct part *p = &parts[k];
		size_t n = strlen(p->name);

		memcpy(pb, "PART", 4);
		wr32(pb + 4, 64);
		wr32(pb + 12, HOST_ID);
		wr32(pb + 16, k + 1 < nparts ? (uint32_t)k + 2 : 0xffffffff);
		wr32(pb + 20, p->bootable ? PBFF_BOOTABLE : 0);
		pb[36] = (uint8_t)n;			/* DriveName, BSTR */
		memcpy(pb + 37, p->name, n);
		wr32(pb + 128, p->bootblocks ? 19 : 16);	/* TableSize */
		wr32(pb + 132, BSIZE / 4);		/* SizeBlock (longs) */
		wr32(pb + 140, heads);			/* Surfaces */
		wr32(pb + 144, 1);			/* SectorPerBlock */
		wr32(pb + 148, secs);			/* BlocksPerTrack */
		wr32(pb + 152, 2);			/* Reserved */
		wr32(pb + 164, p->lowcyl);
		wr32(pb + 168, p->highcyl);
		wr32(pb + 172, 30);			/* NumBuffers */
		wr32(pb + 180, 0x00ffffff);		/* MaxTransfer */
		wr32(pb + 184, 0x7ffffffe);		/* Mask */
		wr32(pb + 188, (uint32_t)p->pri);	/* BootPri */
		wr32(pb + 192, p->dostype);
		wr32(pb + 204, p->bootblocks);		/* BootBlocks */
		rdb_sum(pb);

		if (p->image) {
			FILE *in = fopen(p->image, "rb");
			size_t off = (size_t)p->lowcyl * cylbytes;
			long sz = file_size(p->image);

			if (!in || fread(disk + off, 1, (size_t)sz, in) != (size_t)sz)
				die("cannot read %s", p->image);
			fclose(in);
		}
	}

	if (!(f = fopen(out, "wb")) || fwrite(disk, 1, total, f) != total)
		die("cannot write %s", out);
	fclose(f);
	printf("mkhdf: %s: %u cylinders x %u heads x %u sectors, %zu KB, "
	    "%d partition(s)\n", out, cyls, heads, secs, total / 1024, nparts);
	return 0;
}

static void
dostype_str(uint32_t t, char *buf)
{
	int i;

	for (i = 0; i < 3; i++) {
		int c = (int)(t >> (24 - 8 * i)) & 0xff;

		buf[i] = (char)(c >= 0x20 && c < 0x7f ? c : '?');
	}
	sprintf(buf + 3, "\\%u", t & 0xff);
}

static int
list(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t b[BSIZE];
	uint32_t blk, cylblocks, guard = 0;
	int found = -1, i;

	if (!f)
		die("cannot open %s", path);
	for (i = 0; i < 16 && found < 0; i++)
		if (fseek(f, (long)i * BSIZE, SEEK_SET) == 0 &&
		    fread(b, 1, BSIZE, f) == BSIZE && !memcmp(b, "RDSK", 4) &&
		    rdb_ok(b))
			found = i;
	if (found < 0)
		die("%s: no valid RDSK block in blocks 0-15", path);
	cylblocks = rd32(b + 144);
	printf("%s: RDSK at block %d, %u cylinders x %u heads x %u sectors\n",
	    path, found, rd32(b + 64), rd32(b + 72), rd32(b + 68));
	printf("  %-12s %-8s %8s %8s %8s %4s %s\n", "name", "dostype", "lowcyl",
	    "highcyl", "size KB", "pri", "flags");
	for (blk = rd32(b + 28); blk != 0xffffffff && guard++ < 64;
	    blk = rd32(b + 16)) {
		char name[32], dt[16];
		uint32_t lo, hi;

		if (fseek(f, (long)blk * BSIZE, SEEK_SET) ||
		    fread(b, 1, BSIZE, f) != BSIZE || memcmp(b, "PART", 4) ||
		    !rdb_ok(b))
			die("%s: bad PART block", path);
		memcpy(name, b + 37, b[36] < 31 ? b[36] : 31);
		name[b[36] < 31 ? b[36] : 31] = 0;
		lo = rd32(b + 164);
		hi = rd32(b + 168);
		dostype_str(rd32(b + 192), dt);
		printf("  %-12s %-8s %8u %8u %8u %4d %s\n", name, dt, lo, hi,
		    (hi - lo + 1) * cylblocks / 2, (int32_t)rd32(b + 188),
		    rd32(b + 20) & PBFF_BOOTABLE ? "bootable" : "");
	}
	fclose(f);
	return 0;
}

int
main(int argc, char **argv)
{
	struct part parts[MAXPART];
	const char *out = NULL;
	uint32_t heads = 1, secs = 32;
	int i, nparts = 0;

	for (i = 1; i < argc; i++) {
		if (i + 1 >= argc)
			goto usage;
		if (!strcmp(argv[i], "--list"))
			return list(argv[i + 1]);
		else if (!strcmp(argv[i], "-o"))
			out = argv[++i];
		else if (!strcmp(argv[i], "--heads"))
			heads = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--secs"))
			secs = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-p") && nparts < MAXPART)
			parse_part(&parts[nparts++], argv[++i]);
		else
			goto usage;
	}
	if (!out || !nparts || !heads || !secs)
		goto usage;
	return build(out, parts, nparts, heads, secs);
usage:
	fprintf(stderr, "usage: mkhdf -o disk.hdf [--heads H] [--secs S] "
	    "-p NAME,image=FILE|size=N[,dostype=X][,pri=N][,bootblocks=N]...\n"
	    "       mkhdf --list disk.hdf\n");
	return 2;
}
