/*
 * Boot monitor shell. Input from the serial port or the Amiga keyboard;
 * output to the console (serial and screen).
 *
 * Numbers are hexadecimal unless prefixed with '#' (decimal); a "0x"
 * prefix is accepted.
 */
#include "pvmk/amiga.h"
#include "pvmk/amix.h"
#include "pvmk/bootcap.h"
#include "pvmk/bootreq.h"
#include "pvmk/conf.h"
#include "pvmk/ffs.h"
#include "pvmk/keyboard.h"
#include "pvmk/kprintf.h"
#include "pvmk/layout.h"
#include "pvmk/log.h"
#include "pvmk/mk.h"
#include "pvmk/paula.h"
#include "pvmk/rdb.h"
#include "pvmk/scsi.h"
#include "pvmk/ufs.h"
#include "pvmk/vcpu.h"
#include "pvmk/z3660.h"
#include "pvmk/version.h"

#define LINE_MAX	128
#define HIST		8
#define MAXARGS		8
#define PROMPT		SORIX_NAME "> "

static char hist[HIST][LINE_MAX];
static u32 nhist;
static int autoboot_done;

/* ------------------------------------------------------------------ */
/* Input                                                               */

/* A character from the serial port or, unless serial_only, the keyboard. */
static int
getc_any(int serial_only)
{
	int c = paula_getc();

	if (c < 0 && !serial_only)
		c = kbd_getc();
	return c;
}

static int
getc_wait(void)
{
	int c;
	u32 s, ms;

	while ((c = getc_any(0)) < 0)
		time_now(&s, &ms);	/* keeps the TOD wrap count current */
	return c;
}

/* Wait up to ms milliseconds for a character; -1 on timeout. */
static int
getc_timeout(u32 timeout_ms, int serial_only)
{
	u32 s0, ms0, s, ms;
	int c;

	time_now(&s0, &ms0);
	for (;;) {
		if ((c = getc_any(serial_only)) >= 0)
			return c;
		time_now(&s, &ms);
		if ((s - s0) * 1000 + ms - ms0 >= timeout_ms)
			return -1;
	}
}

static void
erase_line(u32 n)
{
	while (n--)
		kprintf("\b \b");
}

static u32
read_line(char *buf)
{
	u32 n = 0, h = nhist;
	int c, esc = 0;

	for (;;) {
		c = getc_wait();
		if (esc == 1) {
			esc = c == '[' ? 2 : 0;
			continue;
		}
		if (esc == 2) {
			esc = 0;
			if ((c == 'A' && h > 0 && h + HIST > nhist) ||
			    (c == 'B' && h < nhist)) {
				h += c == 'A' ? (u32)-1 : 1;
				erase_line(n);
				n = 0;
				if (h < nhist) {
					const char *p = hist[h % HIST];

					while (*p)
						buf[n++] = *p++;
				}
				buf[n] = 0;
				kprintf("%s", buf);
			}
			continue;
		}
		switch (c) {
		case 0x1b:
			esc = 1;
			break;
		case '\r':
		case '\n':
			buf[n] = 0;
			kprintf("\n");
			return n;
		case 0x08:
		case 0x7f:
			if (n) {
				n--;
				erase_line(1);
			}
			break;
		case 0x15:	/* Ctrl-U */
			erase_line(n);
			n = 0;
			break;
		case 0x03:	/* Ctrl-C */
			kprintf("^C\n");
			buf[0] = 0;
			return 0;
		default:
			if (c >= 0x20 && c < 0x7f && n < LINE_MAX - 1) {
				buf[n++] = (char)c;
				con_putc(c);
			}
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */

static int
streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int
parse_num(const char *s, u32 *out)
{
	u32 v = 0, base = 16;

	if (*s == '#') {
		base = 10;
		s++;
	} else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	if (!*s)
		return 0;
	for (; *s; s++) {
		u32 d;

		if (*s >= '0' && *s <= '9')
			d = (u32)(*s - '0');
		else if (base == 16 && *s >= 'a' && *s <= 'f')
			d = (u32)(*s - 'a' + 10);
		else if (base == 16 && *s >= 'A' && *s <= 'F')
			d = (u32)(*s - 'A' + 10);
		else
			return 0;
		v = v * base + d;
	}
	*out = v;
	return 1;
}

static int
need_num(const char *s, u32 *out, const char *what)
{
	if (s && parse_num(s, out))
		return 1;
	kprintf("bad or missing %s\n", what);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */

struct cmd {
	const char *name;
	const char *args;
	const char *help;
	void (*fn)(int argc, char **argv);
};

static void cmd_help(int argc, char **argv);
static void cmd_reset(int argc, char **argv);

static void
cmd_ver(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	kprintf(SORIX_NAME " " PVMK_VERSION ", boot %u, built " __DATE__
	    " " __TIME__ "\n", log_bootcount());
	kprintf("Kickstart %u.%u, exec %u.%u\n", bootcap.kick_ver,
	    bootcap.kick_rev, bootcap.exec_ver, bootcap.exec_rev);
}

static void
cmd_cpu(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	cpu_show();
}

static void
cmd_mem(int argc, char **argv)
{
	u32 i, st = sstack_top();

	(void)argc;
	(void)argv;
	for (i = 0; i < bootcap.nmem; i++)
		kprintf("  %08x-%08x %6uK %s\n", bootcap.mem[i].lower,
		    bootcap.mem[i].upper - 1,
		    (bootcap.mem[i].upper - bootcap.mem[i].lower) >> 10,
		    bootcap.mem[i].attr & MEMF_CHIP ? "chip RAM" : "fast RAM");
	kprintf("  %08x-%08x %6uK Kickstart ROM\n", bootcap.kick_base,
	    bootcap.kick_base + bootcap.kick_size - 1, bootcap.kick_size >> 10);
	kprintf("hole %08x-%08x (%uK):\n", bootcap.hole_base,
	    bootcap.hole_top - 1, HOLE_SIZE >> 10);
	kprintf("  %08x-%08x %6uK image\n", bootcap.image_base,
	    bootcap.image_base + bootcap.image_size - 1,
	    (bootcap.image_size + 1023) >> 10);
	kprintf("  %08x-%08x %6uK free\n", bootcap.image_base +
	    bootcap.image_size, st - SSTACK_SIZE - 1,
	    (st - SSTACK_SIZE - bootcap.image_base - bootcap.image_size) >> 10);
	kprintf("  %08x-%08x %6uK supervisor stack\n", st - SSTACK_SIZE, st - 1,
	    SSTACK_SIZE >> 10);
	kprintf("  %08x-%08x %6uK boot log\n", st, bootcap.hole_top - 1,
	    LOG_AREA_SIZE >> 10);
}

static void
cmd_conf(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	kprintf("# %s\n", bootcap.conf_loaded ? "from DF0:pvmk.conf" :
	    "built-in defaults");
	conf_show(&bootcap.conf);
}

static int
part_block(void *ctx, u32 blk, void *buf)
{
	return part_read(ctx, blk, 1, buf) ? -1 : 0;
}

static void
ls_entry(void *arg, const char *name, u32 size, int is_dir, u32 blk)
{
	(void)arg;
	(void)blk;
	if (is_dir)
		kprintf("  %-32s   (dir)\n", name);
	else
		kprintf("  %-32s %7u\n", name, size);
}

/* Split "dev:path" and find a partition a native driver can read. */
static struct partition *
dev_path(const char *cmd, const char *arg, const char **path)
{
	char dev[32];
	struct partition *p;
	u32 n = 0;

	*path = arg;
	while (**path && **path != ':' && n < sizeof(dev) - 1)
		dev[n++] = *(*path)++;
	dev[n] = 0;
	if (**path != ':') {
		kprintf("%s: expected dev:path, e.g. dh0:s or unix_root:/stand\n",
		    cmd);
		return NULL;
	}
	(*path)++;
	if (!(p = part_find(dev))) {
		kprintf("%s: no partition %s (see part)\n", cmd, dev);
		return NULL;
	}
	if (p->native == NATIVE_NONE) {
		kprintf("%s: %s is on %s unit %u, which has no native driver; after "
		    "takeover SORIX-LDOM reads disks on the A3000 internal SCSI "
		    "and the Z3660\n", cmd, p->name, p->device, p->unit);
		return NULL;
	}
	return p;
}

static int
ufs_block(void *ctx, u32 blk, u32 count, void *buf)
{
	return part_read(ctx, blk, count, buf) ? -1 : 0;
}

/* Mount the UFS on an AMIX (UNI\x) partition. */
static int
ufs_open(const char *cmd, struct partition *p, struct ufs *fs)
{
	int err;

	if ((p->dostype >> 8) != 0x554e49) {
		kprintf("%s: %s is not an AMIX partition\n", cmd, p->name);
		return -1;
	}
	fs->read = ufs_block;
	fs->ctx = p;
	if ((err = ufs_mount(fs))) {
		kprintf("%s: %s: %s\n", cmd, p->name, ufs_strerror(err));
		return -1;
	}
	return 0;
}

static void
mode_str(u32 mode, char *s)
{
	static const char types[] = "?pc?d?b?-?l?s???";
	const char *rwx = "rwxrwxrwx";
	int i;

	s[0] = types[(mode >> 12) & 0xf];
	for (i = 0; i < 9; i++)
		s[1 + i] = mode & (0400 >> i) ? rwx[i] : '-';
	s[10] = 0;
}

struct ufs_ls {
	struct ufs *fs;
};

static void
ufs_ls_entry(void *arg, const char *name, const struct ufs_stat *st)
{
	struct ufs_ls *l = arg;
	char m[11], target[64];

	mode_str(st->mode, m);
	kprintf("  %s %8u  %s", m, st->size, name);
	if ((st->mode & UFS_IFMT) == UFS_IFLNK &&
	    ufs_readlink(l->fs, st, target, sizeof(target)) == UFS_OK)
		kprintf(" -> %s", target);
	kprintf("\n");
}

/* ls dev:path on an AmigaDOS (OFS/FFS) or AMIX (UFS) partition */
static void
ls_part(const char *arg)
{
	static struct ffs fs;
	static struct ufs ufs;
	const char *path;
	struct partition *p;
	u32 blk;
	int err, is_dir;

	if (!(p = dev_path("ls", arg, &path)))
		return;
	if ((p->dostype >> 8) == 0x554e49) {
		struct ufs_stat st;
		struct ufs_ls l = { &ufs };

		if (ufs_open("ls", p, &ufs))
			return;
		err = ufs_lookup(&ufs, path, 1, &st);
		if (!err && (st.mode & UFS_IFMT) == UFS_IFDIR)
			err = ufs_list_dir(&ufs, &st, ufs_ls_entry, &l);
		else if (!err)
			ufs_ls_entry(&l, path, &st);
		if (err)
			kprintf("ls: %s: %s\n", arg, ufs_strerror(err));
		return;
	}
	if ((p->dostype >> 8) != 0x444f53) {
		kprintf("ls: %s: no reader for this partition type\n", p->name);
		return;
	}
	fs.read = part_block;
	fs.ctx = p;
	fs.root = (p->nblocks + 1) / 2;
	err = ffs_lookup(&fs, path, &blk, &is_dir);
	if (!err && is_dir)
		err = ffs_list_dir(&fs, blk, ls_entry, NULL);
	else if (!err)
		kprintf("  %s\n", path);
	if (err)
		kprintf("ls: %s: %s\n", arg, ffs_strerror(err));
}

/* cat dev:path: print a file from an AMIX (UFS) partition */
static void
cmd_cat(int argc, char **argv)
{
	static struct ufs fs;
	static char chunk[1024];
	struct partition *p;
	struct ufs_stat st;
	const char *path;
	u32 off;
	int err;

	if (argc < 2) {
		kprintf("usage: cat dev:path   (AMIX UFS partitions)\n");
		return;
	}
	if (!(p = dev_path("cat", argv[1], &path)) || ufs_open("cat", p, &fs))
		return;
	if ((err = ufs_lookup(&fs, path, 1, &st))) {
		kprintf("cat: %s: %s\n", argv[1], ufs_strerror(err));
		return;
	}
	if ((st.mode & UFS_IFMT) != UFS_IFREG) {
		kprintf("cat: %s: not a regular file\n", argv[1]);
		return;
	}
	for (off = 0; off < st.size; off += sizeof(chunk)) {
		u32 n = st.size - off < sizeof(chunk) ? st.size - off :
		    sizeof(chunk), i;

		if ((err = ufs_read_file(&fs, &st, off, chunk, n))) {
			kprintf("\ncat: %s: %s\n", argv[1], ufs_strerror(err));
			return;
		}
		for (i = 0; i < n; i++)
			if (chunk[i] == '\n' || chunk[i] == '\t' ||
			    (chunk[i] >= 0x20 && chunk[i] < 0x7f))
				con_putc(chunk[i]);
			else
				con_putc('.');
	}
	if (st.size && chunk[(st.size - 1) % sizeof(chunk)] != '\n')
		kprintf("\n");
}

static void
cmd_ls(int argc, char **argv)
{
	u32 i;

	if (argc > 1 && !(argv[1][0] == 'd' && argv[1][1] == 'f' &&
	    argv[1][2] == '0')) {
		ls_part(argv[1]);
		return;
	}
	if (!bootcap.nfiles) {
		kprintf("no boot floppy directory cached\n");
		return;
	}
	for (i = 0; i < bootcap.nfiles; i++) {
		if (bootcap.files[i].is_dir)
			kprintf("  %-32s   (dir)\n", bootcap.files[i].path);
		else
			kprintf("  %-32s %7u\n", bootcap.files[i].path,
			    bootcap.files[i].size);
	}
}

static void
cmd_md(int argc, char **argv)
{
	u32 addr, len = 0x40, i, j;

	if (!need_num(argv[1], &addr, "address"))
		return;
	if (argc > 2 && !need_num(argv[2], &len, "length"))
		return;
	for (i = 0; i < len; i += 16) {
		volatile const u8 *p = (volatile const u8 *)(uintptr_t)(addr + i);
		u32 n = len - i < 16 ? len - i : 16;
		u8 b[16];

		for (j = 0; j < n; j++)
			b[j] = p[j];
		kprintf("%08x:", addr + i);
		for (j = 0; j < 16; j++) {
			if (j < n)
				kprintf(" %02x", b[j]);
			else
				kprintf("   ");
		}
		kprintf("  ");
		for (j = 0; j < n; j++)
			con_putc(b[j] >= 0x20 && b[j] < 0x7f ? b[j] : '.');
		kprintf("\n");
	}
}

static void
cmd_mw(int argc, char **argv)
{
	u32 addr, val, size = 4;
	const char *n = argv[0];

	(void)argc;
	if (n[2] == '.')
		size = n[3] == 'b' ? 1 : n[3] == 'w' ? 2 : 4;
	if (!need_num(argv[1], &addr, "address") ||
	    !need_num(argv[2], &val, "value"))
		return;
	if (size == 1)
		*(volatile u8 *)(uintptr_t)addr = (u8)val;
	else if (size == 2)
		*(volatile u16 *)(uintptr_t)addr = (u16)val;
	else
		*(volatile u32 *)(uintptr_t)addr = val;
}

static void
cmd_regs(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	exc_show_last();
}

static void
cmd_dmesg(int argc, char **argv)
{
	int prev = argc > 1 && streq(argv[1], "-p");

	log_dump(prev, con_putc);	/* serial and screen, not re-logged */
}

static void
cmd_sload(int argc, char **argv)
{
	u32 addr, len, i;
	u8 *p;

	(void)argc;
	if (!need_num(argv[1], &addr, "address") ||
	    !need_num(argv[2], &len, "length"))
		return;
	p = (u8 *)(uintptr_t)addr;
	kprintf("sload: send %u bytes now (10 s timeout)\n", len);
	for (i = 0; i < len; i++) {
		int c = getc_timeout(10000, 1);	/* binary: serial only */

		if (c < 0) {
			kprintf("sload: timeout after %u of %u bytes\n", i, len);
			return;
		}
		p[i] = (u8)c;
	}
	kprintf("sload: %u bytes at %08x, crc32 %08x\n", len, addr,
	    crc32(p, len));
}

static void
cmd_go(int argc, char **argv)
{
	u32 addr;

	(void)argc;
	if (!need_num(argv[1], &addr, "address"))
		return;
	icache_inval(cpuinfo.model);
	kprintf("go: calling %08x\n", addr);
	kprintf("go: returned %08x\n", (u32)call_addr(addr));
}

/* ------------------------------------------------------------------ */
/* AMIX kernels                                                        */

static struct amix_kernel amixk;
static u8 *amix_image;			/* relocated ELF file, or NULL */

/*
 * Load and relocate an AMIX kernel from dev:path (default path
 * /stand/unix). The file goes just below the SORIX-LDOM hole; the kernel
 * will be copied to the start of the largest fast RAM region.
 */
static int
amix_load(const char *arg)
{
	static struct ufs fs;
	struct partition *p;
	struct ufs_stat st;
	const char *path;
	u32 base, buf, s0, ms0, s1, ms1, ms;
	int err;

	amix_image = NULL;
	if (!(p = dev_path("load", arg, &path)) || ufs_open("load", p, &fs))
		return -1;
	if (!*path)
		path = AMIX_DEFAULT_PATH;
	if ((err = ufs_lookup(&fs, path, 1, &st))) {
		kprintf("load: %s:%s: %s\n", p->name, path, ufs_strerror(err));
		return -1;
	}
	if ((st.mode & UFS_IFMT) != UFS_IFREG) {
		kprintf("load: %s:%s: not a regular file\n", p->name, path);
		return -1;
	}
	base = amix_bind_address();
	buf = (bootcap.hole_base - st.size - 0x10000) & ~0xffffu;
	if (!base || buf <= base + st.size) {
		kprintf("load: not enough fast RAM for a %u byte kernel\n",
		    st.size);
		return -1;
	}
	kprintf("load: %s:%s, %u bytes, into %08x\n", p->name, path, st.size,
	    buf);
	time_now(&s0, &ms0);
	if ((err = ufs_read_file(&fs, &st, 0, (void *)(uintptr_t)buf,
	    st.size))) {
		kprintf("load: read error: %s\n", ufs_strerror(err));
		return -1;
	}
	time_now(&s1, &ms1);
	ms = (s1 - s0) * 1000 + ms1 - ms0;
	kprintf("load: read in %u ms (%u KB/s)\n", ms,
	    ms ? st.size / ms * 1000 / 1024 : 0);
	if (amix_relocate((u8 *)(uintptr_t)buf, st.size, base, &amixk))
		return -1;
	if (amixk.end > buf) {
		kprintf("load: kernel (to %08x) would overlap its image at "
		    "%08x\n", amixk.end, buf);
		return -1;
	}
	kprintf("load: ELF type %u, %u relocations, entry %08x\n", amixk.type,
	    amixk.nrel, amixk.bd.entry);
	kprintf("load:   .text   %08x %7u bytes\n", amixk.bd.tvaddr,
	    amixk.bd.tsize);
	if (amixk.bd.rsize)
		kprintf("load:   .rodata %08x %7u bytes\n", amixk.bd.rvaddr,
		    amixk.bd.rsize);
	if (amixk.bd.dsize)
		kprintf("load:   .data   %08x %7u bytes\n", amixk.bd.dvaddr,
		    amixk.bd.dsize);
	kprintf("load:   end     %08x\n", amixk.end);
	amix_image = (u8 *)(uintptr_t)buf;
	return 0;
}

/*
 * Native AMIX start (boot method 3): the kernel gets the whole machine.
 * bootinfo, the handoff parameters and the initial stack live in the
 * SCSI bounce buffer in chip RAM, which nothing needs any more.
 */
static void
amix_start(void)
{
	u8 *chip = (u8 *)(uintptr_t)bootcap.scsi_chip;
	struct amix_bootinfo *bi = (struct amix_bootinfo *)chip;
	struct amix_handoff *h = (struct amix_handoff *)(chip + sizeof(*bi));
	u32 i;

	if (!amix_image || !chip) {
		kprintf("boot: no kernel loaded\n");
		return;
	}
	memset(bi, 0, sizeof(*bi));
	for (i = 0; i < bootcap.ncd && i < AMIX_NAUTO; i++)
		memcpy(bi->autocon[i], bootcap.configdev[i], sizeof(bi->autocon[0]));
	for (i = 0; i < bootcap.nmem && i < AMIX_NAUTO; i++)
		memcpy(bi->memory[i], bootcap.memhdr[i], sizeof(bi->memory[0]));
	bi->bootdata = amixk.bd;

	h->sp = bootcap.scsi_chip + SCSI_DMA_BLOCKS * 512 - 16;
	h->entry = amixk.bd.entry;
	h->d0 = AMIX_EXEC1;
	h->d1 = (u32)(uintptr_t)bi;
	h->tbuf = (u32)(uintptr_t)(amix_image + amixk.bd.toffset);
	h->tvaddr = amixk.bd.tvaddr;
	h->tsize = amixk.bd.tsize;
	h->rbuf = (u32)(uintptr_t)(amix_image + amixk.bd.roffset);
	h->rvaddr = amixk.bd.rvaddr;
	h->rsize = amixk.bd.rsize;
	h->dbuf = (u32)(uintptr_t)(amix_image + amixk.bd.doffset);
	h->dvaddr = amixk.bd.dvaddr;
	h->dsize = amixk.bd.dsize;
	h->cpu = cpuinfo.model;

	kprintf("boot: starting AMIX natively at %08x (D0=3, D1=%p): it gets "
	    "the whole machine, SORIX-LDOM is gone until the next reset\n",
	    h->entry, bi);
	con_interactive(0);
	while (!(REG16(SERDATR) & (1 << SERDATR_TSRE)))
		;
	amix_enter(h);
}

static void
cmd_load(int argc, char **argv)
{
	if (argc < 2) {
		kprintf("usage: load dev:[path]   (AMIX kernel, default %s)\n",
		    AMIX_DEFAULT_PATH);
		return;
	}
	amix_load(argv[1]);
}

/* ------------------------------------------------------------------ */
/* Guests                                                              */

#define PVTEST_BASE	0x60000
extern const u8 pvtest_image[], pvtest_image_end[];

static void
cmd_pvtest(int argc, char **argv)
{
	u32 size = (u32)(pvtest_image_end - pvtest_image);

	memcpy((void *)PVTEST_BASE, pvtest_image, size);
	icache_inval(cpuinfo.model);
	vcpu_start("pvtest", PVTEST_BASE, PVTEST_BASE,
	    argc > 1 && streq(argv[1], "loop"));
}

static void
cmd_resume(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	vcpu_resume();
}

static void
cmd_guest(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	vcpu_show();
}

static void
cmd_scsi(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (scsi_init() != SCSI_OK)
		kprintf("scsi: no A3000 internal SCSI controller\n");
	else if (!disk_scan(1))
		kprintf("scsi: no devices on the A3000 internal SCSI\n");
	if (z3660_init() == 0 && !z3660_scan(1))
		kprintf("z3660: no units\n");
	rdb_scan(1);
}

static void
cmd_part(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	part_show();
}

static int
name_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if ((*a | 0x20) != (*b | 0x20))
			return 0;
	return *a == *b;
}

/*
 * The floppy can only decline its boot; strap then boots the next bootable
 * device in its priority order. Returns the name of that device, or NULL.
 */
static const char *
next_boot_device(void)
{
	const struct bc_bootnode *best = NULL;
	u32 i;

	for (i = 0; i < bootcap.nbootnodes; i++) {
		const struct bc_bootnode *b = &bootcap.bootnodes[i];

		if (name_eq(b->name, "DF0") || b->pri == -128)
			continue;
		if (!best || b->pri > best->pri)
			best = b;
	}
	return best ? best->name : NULL;
}

/* Reboot; the floppy declines and Kickstart (strap) boots dev. */
static void
reboot_to_kickstart(const char *dev, const char *what)
{
	kprintf("boot: rebooting; %s will start from %s\n", what, dev);
	bootreq_set(sstack_top(), BOOTREQ_AMIGAOS, dev);
	cmd_reset(0, NULL);
}

static void
cmd_boot(int argc, char **argv)
{
	const char *what = argc > 1 ? argv[1] : bootcap.conf.def;
	struct partition *p;

	if (!*what) {
		kprintf("usage: boot <partition>   (see part)\n");
		return;
	}
	{
		char dev[32];
		u32 n = 0;

		while (what[n] && what[n] != ':' && n < sizeof(dev) - 1) {
			dev[n] = what[n];
			n++;
		}
		dev[n] = 0;
		if (!(p = part_find(dev))) {
			kprintf("boot: no partition %s (see part)\n", dev);
			return;
		}
		if (!what[n] && (p->dostype >> 8) == 0x554e49) {
			static char withcolon[34];	/* "boot unix_root" */

			memcpy(withcolon, p->name, n);
			withcolon[n] = ':';
			withcolon[n + 1] = 0;
			what = withcolon;
		}
	}
	if (dostype_is_amigaos(p->dostype)) {
		/*
		 * Kickstart boots AmigaOS partitions after the floppy declines.
		 * (Not boot-block partitions such as AMIX's UNIX_Boot: its boot1
		 * does not start after a declined floppy boot; AMIX is loaded by
		 * SORIX-LDOM itself from its root file system, M2.)
		 */
		const char *next = next_boot_device();

		if (!next || !name_eq(next, p->name)) {
			kprintf("boot: %s: Kickstart boots %s first after the floppy; "
			    "only that one can be selected from here\n", p->name,
			    next ? next : "(no bootable device)");
			return;
		}
		reboot_to_kickstart(p->name, "AmigaOS");
	} else if ((p->dostype >> 8) == 0x554e49) {
		if (amix_load(what) == 0)
			amix_start();
	}
	else
		kprintf("boot: %s: no loader for this partition type\n", p->name);
}

static void
cmd_reset(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	kprintf("reset: rebooting into Kickstart\n");
	while (!(REG16(SERDATR) & (1 << SERDATR_TSRE)))
		;
	cold_reboot();
}

static const struct cmd cmds[] = {
	{ "help",  "",                  "this list", cmd_help },
	{ "ver",   "",                  "version and boot count", cmd_ver },
	{ "cpu",   "",                  "CPU, FPU, MMU, VBR, CACR", cmd_cpu },
	{ "mem",   "",                  "memory map and hole layout", cmd_mem },
	{ "conf",  "",                  "configuration in effect", cmd_conf },
	{ "ls",    "[dev:path]",        "floppy, AmigaDOS or AMIX (UFS) directory", cmd_ls },
	{ "cat",   "dev:path",          "print a file (AMIX UFS)", cmd_cat },
	{ "scsi",  "",                  "rescan disks (A3000 SCSI, Z3660)", cmd_scsi },
	{ "part",  "",                  "list partitions", cmd_part },
	{ "md",    "<addr> [len]",      "memory dump", cmd_md },
	{ "mw",    "[.b|.w|.l] <a> <v>", "memory write (mw.b, mw.w, mw.l)", cmd_mw },
	{ "regs",  "",                  "registers of the last exception", cmd_regs },
	{ "dmesg", "[-p]",              "boot log (-p: previous boot)", cmd_dmesg },
	{ "sload", "<addr> <len>",      "receive raw bytes over serial", cmd_sload },
	{ "go",    "<addr>",            "call code at addr, print D0", cmd_go },
	{ "load",  "dev:[path]",        "load and relocate an AMIX kernel", cmd_load },
	{ "boot",  "<dev[:path]>",      "AmigaOS partition, or AMIX kernel", cmd_boot },
	{ "pvtest", "[loop]",           "run the virtual CPU test guest", cmd_pvtest },
	{ "guest", "",                  "state of the stopped guest", cmd_guest },
	{ "resume", "",                 "continue the stopped guest", cmd_resume },
	{ "reset", "",                  "reboot into Kickstart", cmd_reset },
	{ NULL, NULL, NULL, NULL },
};

static void
cmd_help(int argc, char **argv)
{
	const struct cmd *c;

	(void)argc;
	(void)argv;
	for (c = cmds; c->name; c++)
		kprintf("  %-6s %-18s %s\n", c->name, c->args, c->help);
	kprintf("Numbers are hex; prefix '#' for decimal. Up/down: history.\n");
}

static void
run(char *line)
{
	char *argv[MAXARGS + 1];
	int argc = 0;
	const struct cmd *c;
	char *p = line;

	while (*p && argc < MAXARGS) {
		while (*p == ' ' || *p == '\t')
			*p++ = 0;
		if (!*p)
			break;
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}
	*p = 0;
	if (!argc)
		return;
	argv[argc] = NULL;
	for (c = cmds; c->name; c++) {
		const char *a = argv[0], *b = c->name;

		/* "mw.b" etc. match "mw" */
		while (*b && *a == *b)
			a++, b++;
		if (!*b && (!*a || (*a == '.' && streq(c->name, "mw")))) {
			c->fn(argc, argv);
			return;
		}
	}
	kprintf("%s: unknown command (try help)\n", argv[0]);
}

static void
autoboot(void)
{
	u32 t = bootcap.conf.timeout, i;

	if (autoboot_done)
		return;
	autoboot_done = 1;
	if (!bootcap.conf.def[0])
		return;
	kprintf("autoboot: %s in %u s, press any key for the shell\n",
	    bootcap.conf.def, t);
	for (i = 0; i < t; i++)
		if (getc_timeout(1000, 0) >= 0) {
			kprintf("autoboot: cancelled\n");
			return;
		}
	cmd_boot(1, NULL);
}

void
shell_main(void)
{
	char line[LINE_MAX];

	autoboot();
	kprintf("type 'help' for commands\n");
	con_interactive(1);
	for (;;) {
		kprintf(PROMPT);
		if (!read_line(line))
			continue;
		memcpy(hist[nhist++ % HIST], line, LINE_MAX);
		run(line);
	}
}
