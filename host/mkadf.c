/*
 * mkadf — build and verify FFS ("DOS\1") volumes: the pvmk boot floppy
 * (880 KB DD ADF) and partition images for host/mkhdf.
 *
 *   mkadf -o pvmk.adf -b bootblock.bin -i pvmk.bin [-n PVMK] [--baud N]
 *         [-f dest=src]...
 *   mkadf -o dh0.img --blocks N [-n NAME] [-f dest=src]...
 *   mkadf --verify image [-q]
 *
 * Layout: the boot block (blocks 0-1) carries the PVBB parameter area; the
 * file "pvmk" is stored contiguously from block 2 so the boot block can load
 * it with one CMD_READ (FFS data blocks have no headers). The root block is
 * at (blocks + 1) / 2 (880 on a floppy), followed by the bitmap block(s),
 * then all other files and directories. Without -b the boot block is a
 * plain "DOS\1" with no code. The result is a normal AmigaDOS volume.
 * SOURCE_DATE_EPOCH, if set, fixes the timestamps.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pvmk/image.h"

#define BSIZE		512
#define ADF_BLOCKS	1760		/* 80 cyl * 2 heads * 11 sectors */
#define BM_BITS		(127 * 32)	/* blocks covered by one bitmap block */
#define MAX_BM		25		/* bm_pages[] in the root block */
#define HT_SIZE		72
#define MAXNAME		30

#define T_HEADER	2
#define T_LIST		16
#define ST_ROOT		1
#define ST_USERDIR	2
#define ST_FILE		(-3)

/* Offsets shared by root, directory and file header blocks. */
#define O_TYPE		0
#define O_KEY		4
#define O_HIGHSEQ	8
#define O_HTSIZE	12
#define O_FIRSTDATA	16
#define O_CHKSUM	20
#define O_TABLE		24
#define O_BMFLAG	312
#define O_BMPAGES	316
#define O_PROTECT	320
#define O_BYTESIZE	324
#define O_DAYS		420
#define O_NAME		432
#define O_VDAYS		472
#define O_CDAYS		484
#define O_HASHCHAIN	496
#define O_PARENT	500
#define O_EXTENSION	504
#define O_SECTYPE	508

static uint8_t *disk;
static uint8_t *used, *seen;		/* per block: built / found in use */
static uint32_t nblocks, root_blk, bm_count;
static int verbose = 1;

static void
die(const char *fmt, const char *arg)
{
	fprintf(stderr, "mkadf: ");
	fprintf(stderr, fmt, arg);
	fprintf(stderr, "\n");
	exit(1);
}

static uint8_t *
blk(uint32_t b)
{
	return disk + (size_t)b * BSIZE;
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

static uint32_t
crc32(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffff;
	int k;

	while (n--) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = c & 1 ? (c >> 1) ^ 0xedb88320 : c >> 1;
	}
	return ~c;
}

/* Boot block: sum with end-around carry; stored value is the complement. */
static uint32_t
bootsum(const uint8_t *bb)
{
	uint32_t sum = 0, d, n;
	int i;

	for (i = 0; i < 256; i++) {
		d = i == 1 ? 0 : rd32(bb + i * 4);
		n = sum + d;
		if (n < sum)
			n++;
		sum = n;
	}
	return ~sum;
}

/* Header/data-structure blocks: all longs including the checksum sum to 0. */
static uint32_t
blocksum(const uint8_t *b, int chk_off)
{
	uint32_t sum = 0;
	int i;

	for (i = 0; i < BSIZE; i += 4)
		if (i != chk_off)
			sum += rd32(b + i);
	return -sum;
}

static int
name_hash(const char *name)
{
	uint32_t h = (uint32_t)strlen(name);

	for (; *name; name++)
		h = (h * 13 + (uint32_t)toupper((unsigned char)*name)) & 0x7ff;
	return (int)(h % HT_SIZE);
}

static int
name_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
			return 0;
	return *a == *b;
}

static void
wr_name(uint8_t *b, const char *name)
{
	size_t n = strlen(name);

	b[O_NAME] = (uint8_t)n;
	memcpy(b + O_NAME + 1, name, n);
}

/* ------------------------------------------------------------------ */
/* Building                                                            */

struct node {
	char name[MAXNAME + 1];
	int is_dir;
	uint32_t block;
	struct node *parent, *children, *next;
	uint8_t *data;
	size_t size;
	uint32_t fixed_start;		/* data blocks preassigned (pvmk) */
	uint32_t *dblocks, *eblocks;
	uint32_t ndata, next_blocks;
	uint32_t hash_chain;
};

static uint32_t cursor;
static uint32_t days, mins, ticks;

static uint32_t
alloc_block(void)
{
	uint32_t i, b;

	for (i = 0; i < nblocks; i++) {
		b = cursor + i;
		if (b >= nblocks)
			b = 2 + (b - nblocks) % (nblocks - 2);
		if (!used[b]) {
			used[b] = 1;
			cursor = b + 1;
			return b;
		}
	}
	die("%s", "disk full");
	return 0;
}

static uint8_t *
read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;

	if (!f)
		die("cannot open %s", path);
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	buf = malloc(n > 0 ? (size_t)n : 1);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n)
		die("cannot read %s", path);
	fclose(f);
	*size = (size_t)n;
	return buf;
}

static struct node *
child(struct node *dir, const char *name, int is_dir)
{
	struct node *n;

	if (strlen(name) == 0 || strlen(name) > MAXNAME || strpbrk(name, ":/"))
		die("bad file name component '%s'", name);
	for (n = dir->children; n; n = n->next)
		if (name_eq(n->name, name)) {
			if (n->is_dir != is_dir || !is_dir)
				die("duplicate name '%s'", name);
			return n;
		}
	n = calloc(1, sizeof(*n));
	strcpy(n->name, name);
	n->is_dir = is_dir;
	n->parent = dir;
	n->next = dir->children;
	dir->children = n;
	return n;
}

static struct node *
add_path(struct node *root, const char *path)
{
	char buf[256], *p, *slash;
	struct node *dir = root;

	if (strlen(path) >= sizeof(buf))
		die("path too long: %s", path);
	strcpy(buf, path);
	for (p = buf; (slash = strchr(p, '/')); p = slash + 1) {
		*slash = 0;
		dir = child(dir, p, 1);
	}
	return child(dir, p, 0);
}

static void
alloc_tree(struct node *dir)
{
	struct node *n;
	uint32_t i;

	for (n = dir->children; n; n = n->next) {
		n->block = alloc_block();
		if (n->is_dir)
			continue;
		n->ndata = (uint32_t)((n->size + BSIZE - 1) / BSIZE);
		n->dblocks = calloc(n->ndata + 1, sizeof(uint32_t));
		for (i = 0; i < n->ndata; i++)
			n->dblocks[i] = n->fixed_start ? n->fixed_start + i :
			    alloc_block();
		n->next_blocks = n->ndata > HT_SIZE ?
		    (n->ndata - 1) / HT_SIZE : 0;
		n->eblocks = calloc(n->next_blocks + 1, sizeof(uint32_t));
		for (i = 0; i < n->next_blocks; i++)
			n->eblocks[i] = alloc_block();
	}
	for (n = dir->children; n; n = n->next)
		if (n->is_dir)
			alloc_tree(n);
}

static void
write_date(uint8_t *b, int off)
{
	wr32(b + off, days);
	wr32(b + off + 4, mins);
	wr32(b + off + 8, ticks);
}

/* Fill dir's hash table; chains sorted by block number. */
static void
write_hash_table(uint8_t *b, struct node *dir)
{
	struct node *n, *m;
	int h;

	for (h = 0; h < HT_SIZE; h++) {
		uint32_t prev = 0;

		for (;;) {
			struct node *best = NULL;

			for (m = dir->children; m; m = m->next)
				if (name_hash(m->name) == h && m->block > prev &&
				    (!best || m->block < best->block))
					best = m;
			if (!best)
				break;
			if (!prev)
				wr32(b + O_TABLE + h * 4, best->block);
			else
				for (n = dir->children; n; n = n->next)
					if (n->block == prev)
						n->hash_chain = best->block;
			prev = best->block;
		}
	}
}

static void
write_tree(struct node *dir)
{
	struct node *n;
	uint32_t i, k;

	for (n = dir->children; n; n = n->next) {
		uint8_t *b = blk(n->block);
		uint32_t parent = dir->parent ? dir->block : root_blk;

		wr32(b + O_TYPE, T_HEADER);
		wr32(b + O_KEY, n->block);
		write_date(b, O_DAYS);
		wr_name(b, n->name);
		wr32(b + O_HASHCHAIN, n->hash_chain);
		wr32(b + O_PARENT, parent);
		if (n->is_dir) {
			wr32(b + O_SECTYPE, ST_USERDIR);
			write_hash_table(b, n);	/* sets the children's chains */
			continue;		/* checksummed after recursion */
		}
		wr32(b + O_SECTYPE, (uint32_t)ST_FILE);
		wr32(b + O_HIGHSEQ, n->ndata < HT_SIZE ? n->ndata : HT_SIZE);
		wr32(b + O_FIRSTDATA, n->ndata ? n->dblocks[0] : 0);
		wr32(b + O_BYTESIZE, (uint32_t)n->size);
		wr32(b + O_EXTENSION, n->next_blocks ? n->eblocks[0] : 0);
		for (i = 0; i < n->ndata && i < HT_SIZE; i++)
			wr32(b + O_TABLE + (HT_SIZE - 1 - i) * 4, n->dblocks[i]);
		for (k = 0; k < n->next_blocks; k++) {
			uint8_t *e = blk(n->eblocks[k]);
			uint32_t first = (k + 1) * HT_SIZE, cnt = n->ndata - first;

			if (cnt > HT_SIZE)
				cnt = HT_SIZE;
			wr32(e + O_TYPE, T_LIST);
			wr32(e + O_KEY, n->eblocks[k]);
			wr32(e + O_HIGHSEQ, cnt);
			for (i = 0; i < cnt; i++)
				wr32(e + O_TABLE + (HT_SIZE - 1 - i) * 4,
				    n->dblocks[first + i]);
			wr32(e + O_PARENT, n->block);
			wr32(e + O_EXTENSION, k + 1 < n->next_blocks ?
			    n->eblocks[k + 1] : 0);
			wr32(e + O_SECTYPE, (uint32_t)ST_FILE);
			wr32(e + O_CHKSUM, blocksum(e, O_CHKSUM));
		}
		for (i = 0; i < n->ndata; i++) {
			size_t off = (size_t)i * BSIZE, len = n->size - off;

			memcpy(blk(n->dblocks[i]), n->data + off,
			    len > BSIZE ? BSIZE : len);
		}
		wr32(b + O_CHKSUM, blocksum(b, O_CHKSUM));
	}
	for (n = dir->children; n; n = n->next)
		if (n->is_dir) {
			write_tree(n);
			wr32(blk(n->block) + O_CHKSUM,
			    blocksum(blk(n->block), O_CHKSUM));
		}
}

static void
set_date(void)
{
	const char *sde = getenv("SOURCE_DATE_EPOCH");
	long long t = sde ? atoll(sde) : (long long)time(NULL);
	long long s = t - 252460800LL;	/* 1978-01-01 */

	if (s < 0)
		s = 0;
	days = (uint32_t)(s / 86400);
	mins = (uint32_t)(s % 86400 / 60);
	ticks = (uint32_t)(s % 60 * 50);
}

/* Size the volume: root block in the middle, bitmap blocks after it. */
static void
geometry(uint32_t n)
{
	if (n < 8)
		die("%s", "volume too small");
	nblocks = n;
	root_blk = (n + 1) / 2;
	bm_count = (n - 2 + BM_BITS - 1) / BM_BITS;
	if (bm_count > MAX_BM)
		die("%s", "volume too large (bitmap extension blocks not supported)");
	disk = calloc(n, BSIZE);
	used = calloc(n, 1);
	seen = calloc(n, 1);
	if (!disk || !used || !seen)
		die("%s", "out of memory");
	cursor = root_blk + 1 + bm_count;
}

static int
build(const char *out, const char *bootpath, const char *imgpath,
    const char *volname, uint32_t baud, int nfiles, char **files)
{
	struct node root = { .is_dir = 1, .block = root_blk };
	struct node *pvmk = NULL;
	uint8_t *bb, *rb, *bm;
	size_t bbsize;
	uint32_t b, i, alloc = 0;
	FILE *f;
	int k;

	set_date();
	used[0] = used[1] = used[root_blk] = 1;
	for (b = 0; b < bm_count; b++)
		used[root_blk + 1 + b] = 1;

	if (strlen(volname) > MAXNAME)
		die("volume name too long: %s", volname);

	if (imgpath) {
		pvmk = add_path(&root, "pvmk");
		pvmk->data = read_file(imgpath, &pvmk->size);
		pvmk->fixed_start = 2;
		if (2 + (pvmk->size + BSIZE - 1) / BSIZE > root_blk)
			die("%s does not fit below the root block", imgpath);
		for (b = 2; b < 2 + (pvmk->size + BSIZE - 1) / BSIZE; b++)
			used[b] = 1;
	}
	for (k = 0; k < nfiles; k++) {
		char *eq = strchr(files[k], '=');
		struct node *n;

		if (!eq)
			die("-f expects dest=src, got '%s'", files[k]);
		*eq = 0;
		n = add_path(&root, files[k]);
		n->data = read_file(eq + 1, &n->size);
	}

	alloc_tree(&root);
	rb = blk(root_blk);
	write_hash_table(rb, &root);	/* before write_tree: sets chains */
	write_tree(&root);

	/* Root block */
	wr32(rb + O_TYPE, T_HEADER);
	wr32(rb + O_HTSIZE, HT_SIZE);
	wr32(rb + O_BMFLAG, 0xffffffff);
	for (b = 0; b < bm_count; b++)
		wr32(rb + O_BMPAGES + b * 4, root_blk + 1 + b);
	write_date(rb, O_DAYS);
	write_date(rb, O_VDAYS);
	write_date(rb, O_CDAYS);
	wr_name(rb, volname);
	wr32(rb + O_SECTYPE, ST_ROOT);
	wr32(rb + O_CHKSUM, blocksum(rb, O_CHKSUM));

	/* Bitmap: bit set = free; covers blocks 2..nblocks-1. */
	for (b = 2; b < nblocks; b++)
		if (!used[b]) {
			uint32_t idx = b - 2, j = idx % BM_BITS;
			uint32_t off = 4 + j / 32 * 4;

			bm = blk(root_blk + 1 + idx / BM_BITS);
			wr32(bm + off, rd32(bm + off) | 1u << (j % 32));
		}
	for (b = 0; b < bm_count; b++) {
		bm = blk(root_blk + 1 + b);
		wr32(bm, blocksum(bm, 0));
	}

	/* Boot block + PVBB */
	if (bootpath) {
		bb = read_file(bootpath, &bbsize);
		if (bbsize != 2 * BSIZE)
			die("boot block %s must be exactly 1024 bytes", bootpath);
		if (memcmp(bb, "DOS\1", 4) != 0)
			die("boot block %s does not start with DOS\\1", bootpath);
		memcpy(disk, bb, bbsize);
	} else {
		memcpy(disk, "DOS\1", 4);
		wr32(disk + 8, root_blk);
	}
	if (pvmk) {
		uint8_t *p = disk + PVBB_OFFSET;
		uint32_t nblk = (uint32_t)((pvmk->size + BSIZE - 1) / BSIZE);

		if (rd32(p + PVBB_O_MAGIC) != PVBB_MAGIC)
			die("%s", "PVBB area not found in boot block");
		if (pvmk->size >= PVMK_HDR_SIZE &&
		    rd32(pvmk->data + PVMK_O_MAGIC) == PVMK_MAGIC)
			alloc = rd32(pvmk->data + PVMK_O_ALLOC);
		if (alloc < nblk * BSIZE)
			alloc = nblk * BSIZE;
		p[PVBB_O_MEDIUM] = 0;
		p[PVBB_O_MEDIUM + 1] = BOOT_FLOPPY;
		wr32(p + PVBB_O_START, 2);
		wr32(p + PVBB_O_NBLOCKS, nblk);
		wr32(p + PVBB_O_SIZE, (uint32_t)pvmk->size);
		wr32(p + PVBB_O_ALLOC, alloc);
		wr32(p + PVBB_O_CRC, crc32(pvmk->data, pvmk->size));
		wr32(p + PVBB_O_ENTRY, 0);
		wr32(p + PVBB_O_BAUD, baud);
	}
	wr32(disk + 4, bootsum(disk));

	if (!(f = fopen(out, "wb")) ||
	    fwrite(disk, BSIZE, nblocks, f) != nblocks)
		die("cannot write %s", out);
	fclose(f);
	for (i = 0, b = 0; b < nblocks; b++)
		i += used[b];
	printf("mkadf: %s: volume %s, %u/%u blocks used", out, volname, i,
	    nblocks);
	if (pvmk)
		printf(", pvmk %zu bytes at block 2", pvmk->size);
	printf("\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Verifying                                                           */

static int errors;
static uint32_t pvmk_first, pvmk_n, pvmk_size;
static int pvmk_contig = -1;

static void
err(const char *fmt, ...)
{
	va_list ap;

	errors++;
	fprintf(stderr, "mkadf: error: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static int
claim(uint32_t b, const char *what, const char *path)
{
	if (b < 2 || b >= nblocks) {
		err("%s: %s block %u out of range", path, what, b);
		return 0;
	}
	if (seen[b])
		err("%s: %s block %u used twice", path, what, b);
	seen[b] = 1;
	return 1;
}

static void
get_name(const uint8_t *b, char *name)
{
	unsigned n = b[O_NAME];

	if (n > MAXNAME)
		n = MAXNAME;
	memcpy(name, b + O_NAME + 1, n);
	name[n] = 0;
}

static void walk_dir(uint32_t dirblk, const char *path, int depth);

static void
check_file(uint32_t hb, const char *path, int top)
{
	const uint8_t *h = blk(hb);
	uint32_t size = rd32(h + O_BYTESIZE), want, got = 0, prev = 0, eb;
	uint32_t i, cnt, first = 0;
	int contig = 1;
	const uint8_t *t = h;

	want = (size + BSIZE - 1) / BSIZE;
	for (;;) {
		cnt = rd32(t + O_HIGHSEQ);
		if (cnt > HT_SIZE) {
			err("%s: bad high_seq %u", path, cnt);
			break;
		}
		for (i = 0; i < cnt; i++) {
			uint32_t d = rd32(t + O_TABLE + (HT_SIZE - 1 - i) * 4);

			if (!claim(d, "data", path))
				continue;
			if (got == 0)
				first = d;
			else if (d != prev + 1)
				contig = 0;
			prev = d;
			got++;
		}
		eb = rd32(t + O_EXTENSION);
		if (!eb)
			break;
		if (!claim(eb, "extension", path))
			break;
		t = blk(eb);
		if (rd32(t + O_TYPE) != T_LIST || blocksum(t, O_CHKSUM) !=
		    rd32(t + O_CHKSUM) || rd32(t + O_PARENT) != hb)
			err("%s: bad extension block %u", path, eb);
	}
	if (got != want)
		err("%s: %u data blocks for %u bytes (expected %u)", path, got,
		    size, want);
	if (want && rd32(h + O_FIRSTDATA) != first)
		err("%s: first_data mismatch", path);
	if (verbose)
		printf("  %-28s %7u bytes  %4u blocks%s\n", path, size, got,
		    got > 1 ? (contig ? "  contiguous" : "  fragmented") : "");
	if (top && strcmp(path, "pvmk") == 0) {
		pvmk_first = first;
		pvmk_n = got;
		pvmk_size = size;
		pvmk_contig = contig;
	}
}

static void
walk_dir(uint32_t dirblk, const char *path, int depth)
{
	const uint8_t *d = blk(dirblk);
	int h;

	if (depth > 16) {
		err("%s: directory nesting too deep", path);
		return;
	}
	for (h = 0; h < HT_SIZE; h++) {
		uint32_t b = rd32(d + O_TABLE + h * 4);
		int guard = 0;

		while (b && guard++ < (int)nblocks) {
			const uint8_t *e;
			char name[MAXNAME + 1], sub[512];
			int32_t st;

			if (!claim(b, "header", path))
				break;
			e = blk(b);
			get_name(e, name);
			snprintf(sub, sizeof(sub), "%s%s%s", path, *path ? "/" : "",
			    name);
			if (rd32(e + O_TYPE) != T_HEADER)
				err("%s: header %u has type %u", sub, b,
				    rd32(e + O_TYPE));
			if (blocksum(e, O_CHKSUM) != rd32(e + O_CHKSUM))
				err("%s: header %u checksum bad", sub, b);
			if (rd32(e + O_KEY) != b)
				err("%s: header_key %u != block %u", sub,
				    rd32(e + O_KEY), b);
			if (rd32(e + O_PARENT) != dirblk)
				err("%s: parent %u != %u", sub,
				    rd32(e + O_PARENT), dirblk);
			if (name_hash(name) != h)
				err("%s: in hash bucket %d, expected %d", sub, h,
				    name_hash(name));
			st = (int32_t)rd32(e + O_SECTYPE);
			if (st == ST_USERDIR) {
				if (verbose)
					printf("  %-28s     (dir)\n", sub);
				walk_dir(b, sub, depth + 1);
			} else if (st == ST_FILE)
				check_file(b, sub, depth == 0);
			else
				err("%s: unknown sec_type %d", sub, st);
			b = rd32(e + O_HASHCHAIN);
		}
	}
}

static int
verify(const char *path)
{
	FILE *f = fopen(path, "rb");
	const uint8_t *rb, *bm, *p;
	char vol[MAXNAME + 1];
	uint32_t b, k, nfree = 0, bmblk[MAX_BM];
	long size;
	int have_pvbb;

	if (!f)
		die("cannot open %s", path);
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	if (size <= 0 || size % BSIZE)
		die("%s: size is not a multiple of 512 bytes", path);
	geometry((uint32_t)(size / BSIZE));
	if (fread(disk, BSIZE, nblocks, f) != nblocks)
		die("cannot read %s", path);
	fclose(f);

	if (memcmp(disk, "DOS", 3) != 0)
		err("boot block: no DOS signature");
	else if (disk[3] != 1)
		err("boot block: DOS\\%d, expected DOS\\1 (FFS)", disk[3]);
	if (bootsum(disk) != rd32(disk + 4))
		err("boot block: checksum bad");
	if (rd32(disk + 8) != root_blk)
		err("boot block: root block %u, expected %u", rd32(disk + 8),
		    root_blk);

	rb = blk(root_blk);
	if (rd32(rb + O_TYPE) != T_HEADER || (int32_t)rd32(rb + O_SECTYPE) !=
	    ST_ROOT || rd32(rb + O_HTSIZE) != HT_SIZE)
		err("root block: bad type/sec_type/ht_size");
	if (blocksum(rb, O_CHKSUM) != rd32(rb + O_CHKSUM))
		err("root block: checksum bad");
	if (rd32(rb + O_BMFLAG) != 0xffffffff)
		err("root block: bitmap not marked valid");
	get_name(rb, vol);
	seen[root_blk] = 1;
	if (verbose)
		printf("%s: volume %s, %u blocks\n", path, vol, nblocks);

	for (k = 0; k < bm_count; k++) {
		b = rd32(rb + O_BMPAGES + k * 4);
		if (b < 2 || b >= nblocks) {
			err("root block: bitmap block %u out of range", b);
			return 1;
		}
		seen[b] = 1;
		bmblk[k] = b;
		if (blocksum(blk(b), 0) != rd32(blk(b)))
			err("bitmap block %u: checksum bad", b);
	}

	walk_dir(root_blk, "", 0);

	for (b = 2; b < nblocks; b++) {
		uint32_t idx = b - 2, j = idx % BM_BITS;
		int isfree;

		bm = blk(bmblk[idx / BM_BITS]);
		isfree = (rd32(bm + 4 + j / 32 * 4) >> (j % 32)) & 1;

		nfree += isfree;
		if (seen[b] && isfree)
			err("block %u in use but marked free", b);
		else if (!seen[b] && !isfree)
			fprintf(stderr, "mkadf: warning: block %u allocated but "
			    "unused\n", b);
	}

	p = disk + PVBB_OFFSET;
	have_pvbb = rd32(p + PVBB_O_MAGIC) == PVBB_MAGIC;
	if (!have_pvbb) {
		if (verbose)
			printf("no PVBB area (plain volume, not a pvmk boot disk)\n");
	} else {
		uint32_t start = rd32(p + PVBB_O_START);
		uint32_t n = rd32(p + PVBB_O_NBLOCKS);
		uint32_t size = rd32(p + PVBB_O_SIZE);
		uint32_t crc = rd32(p + PVBB_O_CRC);

		if (start < 2 || start + n > nblocks || size > n * BSIZE ||
		    n * BSIZE - size >= BSIZE)
			err("PVBB: bad range start %u n %u size %u", start, n,
			    size);
		else if (crc32(blk(start), size) != crc)
			err("PVBB: image CRC mismatch (pvmk moved or changed? "
			    "rebuild or run pvinstall)");
		if (pvmk_contig < 0)
			err("PVBB present but no file 'pvmk' in the root");
		else if (!pvmk_contig || pvmk_first != start ||
		    pvmk_n != n || pvmk_size != size)
			err("PVBB does not match file pvmk (blocks %u+%u, size %u)",
			    pvmk_first, pvmk_n, pvmk_size);
		if (rd32(p + PVBB_O_ALLOC) < n * BSIZE)
			err("PVBB: alloc size smaller than the read");
		if (rd32(blk(start) + PVMK_O_MAGIC) != PVMK_MAGIC)
			err("pvmk image: PVMK header magic missing");
		if (verbose)
			printf("PVBB: medium %u, blocks %u+%u, %u bytes, alloc %u, "
			    "crc %08x, baud %u\n",
			    rd32(p + PVBB_O_VERSION) & 0xffff, start, n, size,
			    rd32(p + PVBB_O_ALLOC), crc, rd32(p + PVBB_O_BAUD));
	}
	if (verbose)
		printf("%u blocks free\n", nfree);
	if (errors) {
		fprintf(stderr, "mkadf: %s: %d error(s)\n", path, errors);
		return 1;
	}
	printf("mkadf: %s: OK\n", path);
	return 0;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: mkadf -o out.adf [-b bootblock.bin] [-i pvmk.bin] [-n VOLUME]\n"
	    "             [--blocks N] [--baud N] [-f dest=src]...\n"
	    "       mkadf --verify image [-q]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	const char *out = NULL, *boot = NULL, *img = NULL, *vol = "PVMK";
	const char *check = NULL;
	char **files = calloc((size_t)argc, sizeof(char *));
	uint32_t baud = 115200, blocks = ADF_BLOCKS;
	int i, nfiles = 0;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "-q"))
			verbose = 0;
		else if (!v)
			usage();
		else if (!strcmp(a, "-o"))
			out = argv[++i];
		else if (!strcmp(a, "-b"))
			boot = argv[++i];
		else if (!strcmp(a, "-i"))
			img = argv[++i];
		else if (!strcmp(a, "-n"))
			vol = argv[++i];
		else if (!strcmp(a, "--blocks"))
			blocks = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(a, "--baud"))
			baud = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(a, "-f"))
			files[nfiles++] = argv[++i];
		else if (!strcmp(a, "--verify"))
			check = argv[++i];
		else
			usage();
	}
	if (check)
		return verify(check);
	if (!out || (img && !boot))
		usage();
	geometry(blocks);
	return build(out, boot, img, vol, baud, nfiles, files);
}
