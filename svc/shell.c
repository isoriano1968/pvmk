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
#include "pvmk/display.h"
#include "pvmk/ffs.h"
#include "pvmk/gdb.h"
#include "pvmk/image.h"
#include "pvmk/keyboard.h"
#include "pvmk/kprintf.h"
#include "pvmk/layout.h"
#include "pvmk/log.h"
#include "pvmk/menu.h"
#include "pvmk/mk.h"
#include "pvmk/paula.h"
#include "pvmk/rdb.h"
#include "pvmk/scsi.h"
#include "pvmk/ufs.h"
#include "pvmk/vcpu.h"
#include "pvmk/vmmu.h"
#include "pvmk/z3660.h"
#include "pvmk/version.h"

#define LINE_MAX	128
#define HIST		8
#define MAXARGS		8
#define PROMPT		SORIX_NAME "> "

static char hist[HIST][LINE_MAX];
static u32 nhist;
static int autoboot_done;
static int screen_taken;		/* console on screen, guest stopped */

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
static void cmd_kick(int argc, char **argv);

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
	kprintf("# %s\n", !bootcap.conf_loaded ? "built-in defaults" :
	    bootcap.medium == BOOT_FLOPPY ? "from DF0:pvmk.conf" :
	    "from PVMK:pvmk.conf");
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
		    "takeover SORIX-ILOM-LDOM reads disks on the A3000 internal SCSI "
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
	/*
	 * The variant is the one formatted, in the partition's boot block
	 * (FFS goes by that too): a DOS\3 in the RDB may hold DOS\7.
	 */
	if (!part_block(p, 0, fs.hbuf) && fs.hbuf[0] == 'D' &&
	    fs.hbuf[1] == 'O' && fs.hbuf[2] == 'S' && fs.hbuf[3] <= 7)
		ffs_set_dostype(&fs, 0x444f5300 | fs.hbuf[3]);
	else
		ffs_set_dostype(&fs, p->dostype);
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
	u32 addr, len = 0x40, i, j, pa;
	int g = argc > 1 && streq(argv[1], "-g");	/* guest kernel view */

	if (g) {
		argc--;
		argv++;
	}
	if (argc < 2 || !need_num(argv[1], &addr, "address"))
		return;
	if (argc > 2 && !need_num(argv[2], &len, "length"))
		return;
	for (i = 0; i < len; i += 16) {
		u32 n = len - i < 16 ? len - i : 16;
		u8 b[16];

		for (j = 0; j < n; j++) {
			pa = addr + i + j;
			if (g && vmmu_translate(addr + i + j, 1, 0, &pa)) {
				kprintf("%08x: not mapped for the guest\n",
				    addr + i + j);
				return;
			}
			b[j] = *(volatile const u8 *)(uintptr_t)pa;
		}
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

/*
 * Receive len bytes from the serial port into p, as fast as they come: a
 * tight poll of SERDATR (Paula holds one byte), with the overrun bit
 * checked so a lost byte is reported, not stored. 10 s without a byte
 * is a timeout.
 */
static int
ser_receive(u8 *p, u32 len)
{
	u32 i = 0, idle = 0, s0, ms0, s, ms;
	u16 v;

	kprintf("load: send %u bytes now (10 s timeout)\n", len);
	while (!(REG16(SERDATR) & (1 << SERDATR_TSRE)))
		;
	time_now(&s0, &ms0);
	while (i < len) {
		v = REG16(SERDATR);
		if (v & (1 << SERDATR_RBF)) {
			REG16(CUSTOM_INTREQ) = 0x0800;	/* clear RBF */
			if (v & 0x8000) {		/* OVRUN */
				kprintf("load: serial overrun at byte %u of %u\n",
				    i, len);
				return -1;
			}
			p[i++] = (u8)v;
			idle = 0;
			continue;
		}
		if (++idle == 1)
			time_now(&s0, &ms0);	/* idle since now */
		else if (!(idle & 0xffff)) {
			time_now(&s, &ms);
			if ((s - s0) * 1000 + ms - ms0 >= 10000) {
				kprintf("load: timeout after %u of %u bytes\n",
				    i, len);
				return -1;
			}
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* AMIX kernels                                                        */

static struct amix_kernel amixk;
static u8 *amix_image;			/* relocated ELF file, or NULL */
static u32 amix_mem_cap;		/* boot -m: bytes for AMIX, 0 = all */

/*
 * Load and relocate an AMIX kernel from dev:path (default path
 * /stand/unix). The file goes just below the SORIX-ILOM-LDOM hole; the kernel
 * will be copied to the start of the largest fast RAM region.
 */
static int
amix_load(const char *arg)
{
	static struct ufs fs;
	struct partition *p = NULL;
	struct ufs_stat st;
	const char *path = "";
	u32 base, buf, top, i, size, s0, ms0, s1, ms1, ms;
	int err, ser = arg[0] == 's' && arg[1] == 'e' && arg[2] == 'r' &&
	    arg[3] == ':';

	amix_image = NULL;
	if (ser) {			/* ser:<size>: the file comes over serial */
		if (!need_num((char *)arg + 4, &size, "size"))
			return -1;
	} else {
		if (!(p = dev_path("load", arg, &path)) ||
		    ufs_open("load", p, &fs))
			return -1;
		if (!*path)
			path = AMIX_DEFAULT_PATH;
		if ((err = ufs_lookup(&fs, path, 1, &st))) {
			kprintf("load: %s:%s: %s\n", p->name, path,
			    ufs_strerror(err));
			return -1;
		}
		if ((st.mode & UFS_IFMT) != UFS_IFREG) {
			kprintf("load: %s:%s: not a regular file\n", p->name,
			    path);
			return -1;
		}
		size = st.size;
	}
	base = amix_bind_address();
	top = bootcap.hole_base;		/* the file: below the hole, */
	for (i = 0; i < bootcap.nmem; i++)	/* or at the top of the kernel's */
		if (base >= bootcap.mem[i].lower - 0x100 &&	/* region */
		    base < bootcap.mem[i].upper &&
		    !(top > base && top <= bootcap.mem[i].upper))
			top = bootcap.mem[i].upper;
	buf = (top - size - 0x10000) & ~0xffffu;
	if (!base || !size || buf <= base + size || buf > top) {
		kprintf("load: not enough fast RAM for a %u byte kernel\n",
		    size);
		return -1;
	}
	if (ser) {
		kprintf("load: serial, %u bytes, into %08x\n", size, buf);
		time_now(&s0, &ms0);
		if (ser_receive((u8 *)(uintptr_t)buf, size))
			return -1;
		kprintf("load: crc32 %08x\n", crc32((u8 *)(uintptr_t)buf,
		    size));
	} else {
		kprintf("load: %s:%s, %u bytes, into %08x\n", p->name, path,
		    size, buf);
		time_now(&s0, &ms0);
		if ((err = ufs_read_file(&fs, &st, 0, (void *)(uintptr_t)buf,
		    size))) {
			kprintf("load: read error: %s\n", ufs_strerror(err));
			return -1;
		}
	}
	time_now(&s1, &ms1);
	ms = (s1 - s0) * 1000 + ms1 - ms0;
	kprintf("load: read in %u ms (%u KB/s)\n", ms,
	    ms ? size / ms * 1000 / 1024 : 0);
	if (amix_relocate((u8 *)(uintptr_t)buf, size, base, &amixk))
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

/* MemHeader fields (exec/memory.h) */
#define MH_LOWER	20
#define MH_UPPER	24
#define MH_FREE		28

/*
 * Deprivileged AMIX start: the kernel runs as a guest of the virtual CPU.
 * Its memory list ends below SORIX-ILOM-LDOM's hole; the screen console stops
 * (AMIX owns chip RAM, where the display and the SCSI bounce buffer are).
 */
static void
amix_start_pv(struct amix_bootinfo *bi, u32 sp)
{
	u32 i, lo, up;

	for (i = 0; i < AMIX_NAUTO; i++) {
		u8 *mh = bi->memory[i];

		lo = *(u32 *)(mh + MH_LOWER);
		up = *(u32 *)(mh + MH_UPPER);
		if (lo <= bootcap.hole_base && bootcap.hole_base < up) {
			*(u32 *)(mh + MH_UPPER) = bootcap.hole_base;
			*(u32 *)(mh + MH_FREE) = bootcap.hole_base - lo;
			kprintf("boot: AMIX memory %08x-%08x (SORIX-ILOM-LDOM above)\n",
			    lo, bootcap.hole_base - 1);
		}
	}
	memcpy((void *)(uintptr_t)amixk.bd.tvaddr,
	    amix_image + amixk.bd.toffset, amixk.bd.tsize);
	memcpy((void *)(uintptr_t)amixk.bd.rvaddr,
	    amix_image + amixk.bd.roffset, amixk.bd.rsize);
	memcpy((void *)(uintptr_t)amixk.bd.dvaddr,
	    amix_image + amixk.bd.doffset, amixk.bd.dsize);
	if (amixk.cputype)			/* 68040/68060 port kernel */
		*(volatile u32 *)(uintptr_t)amixk.cputype = cpuinfo.model;
	amix_image = NULL;			/* overwritten from now on */
	kprintf("boot: PV patches: %d of 3 routines (copyin, copyout, "
	    "haltsys)\n", amix_pv_patch(&amixk));
	REG16(CUSTOM_INTENA) = 0x7fff;		/* quiet, as after a reset */
	REG16(CUSTOM_INTREQ) = 0x7fff;
	REG16(0xdff096) = 0x7dff;		/* DMACON: channels off, */
	REG16(0xdff096) = 0x8200;		/* master on, as Kickstart has it */
	cache_push_all(cpuinfo.model);
	display_off();
	screen_taken = 0;
	vcpu_prepare("amix", amixk.bd.entry, sp, AMIX_EXEC1);
	vcpu_spl_patching(amixk.bd.tvaddr, amixk.bd.tsize);
	guest.ctx.d[1] = (u32)(uintptr_t)bi;
	kprintf("boot: starting AMIX deprivileged at %08x (D0=3, D1=%p); "
	    "Ctrl-C on serial breaks in\n", amixk.bd.entry, bi);
	vcpu_resume();
}

/*
 * AMIX start (boot method 3). Native: the kernel gets the whole machine.
 * pv: it runs deprivileged under SORIX-ILOM-LDOM. bootinfo, the handoff
 * parameters and the initial stack live in the SCSI bounce buffer in chip
 * RAM, which nothing needs any more.
 */
static void
amix_start(int pv)
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
	for (i = 0; amix_mem_cap && i < bootcap.nmem && i < AMIX_NAUTO; i++) {
		u8 *mh = bi->memory[i];		/* boot -m: the kernel's region */
		u32 lo = *(u32 *)(mh + MH_LOWER), up = *(u32 *)(mh + MH_UPPER);

		if (amixk.bd.tvaddr >= (lo & ~0xffu) && amixk.bd.tvaddr < up &&
		    up - lo > amix_mem_cap) {
			*(u32 *)(mh + MH_UPPER) = lo + amix_mem_cap;
			*(u32 *)(mh + MH_FREE) = amix_mem_cap;
			kprintf("boot: AMIX memory %08x-%08x (boot -m %u)\n", lo,
			    lo + amix_mem_cap - 1, amix_mem_cap >> 20);
		}
	}
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

	/*
	 * 68060: the state SetPatch's 68060.library leaves, which AMIX's
	 * 040/060 port expects (its loader runs after SetPatch): FPU on
	 * (PCR DFP clear), superscalar dispatch on (ESS). A guest starts
	 * with this PCR too.
	 */
	if (cpuinfo.model == 60 && (cpuinfo.pcr & 3) != 1) {
		cpuinfo.pcr = (cpuinfo.pcr & ~3u) | 1;
		pcr_write(cpuinfo.pcr);
		kprintf("boot: 68060 PCR %08x: FPU and superscalar dispatch on, "
		    "as after SetPatch\n", cpuinfo.pcr);
	}
	if (pv)
		amix_start_pv(bi, h->sp);
	if (amixk.cputype >= amixk.bd.dvaddr &&
	    amixk.cputype + 4 <= amixk.bd.dvaddr + amixk.bd.dsize)
		*(u32 *)(amix_image + amixk.bd.doffset +
		    (amixk.cputype - amixk.bd.dvaddr)) = cpuinfo.model;
	vmmu_off();
	kprintf("boot: starting AMIX natively at %08x (D0=3, D1=%p): it gets "
	    "the whole machine, SORIX-ILOM-LDOM is gone until the next reset\n",
	    h->entry, bi);
	con_interactive(0);
	while (!(REG16(SERDATR) & (1 << SERDATR_TSRE)))
		;
	amix_enter(h);
}

/*
 * AMIX's console message buffer (putbuf, a ring of putbufsz bytes;
 * putbufwpos counts the bytes written), oldest first.
 */
static void
amix_log(void)
{
	u32 sz, wpos, i, n;
	const volatile u8 *buf;

	if (!amixk.putbuf || !amixk.putbufsz || !amixk.putbufwpos) {
		kprintf("guest log: no AMIX kernel with putbuf loaded\n");
		return;
	}
	sz = *(volatile u32 *)(uintptr_t)amixk.putbufsz;
	wpos = *(volatile u32 *)(uintptr_t)amixk.putbufwpos;
	buf = (const volatile u8 *)(uintptr_t)amixk.putbuf;
	if (!sz || sz > 0x10000) {
		kprintf("guest log: putbufsz %u?\n", sz);
		return;
	}
	n = wpos < sz ? wpos : sz;
	for (i = 0; i < n; i++) {
		u8 c = buf[(wpos - n + i) % sz];

		if (c)
			con_putc(c);
	}
	kprintf("\n[guest log: %u bytes written, buffer %u at %08x]\n", wpos,
	    sz, amixk.putbuf);
}

static void
cmd_load(int argc, char **argv)
{
	if (argc < 2) {
		kprintf("usage: load dev:[path] | ser:<bytes>   (AMIX kernel, "
		    "default %s)\n", AMIX_DEFAULT_PATH);
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

	if (!cpuinfo.mmu_ok) {
		kprintf("pvtest: guests need a working MMU (MMU self-test %s)\n",
		    mmu_result());
		return;
	}
	memcpy((void *)PVTEST_BASE, pvtest_image, size);
	icache_inval(cpuinfo.model);
	if (argc > 1 && streq(argv[argc - 1], "stop")) {
		vcpu_prepare("pvtest", PVTEST_BASE, PVTEST_BASE,
		    argc > 2 && streq(argv[1], "loop"));
		kprintf("pvtest: ready, stopped at %08x (resume, or gdb)\n",
		    PVTEST_BASE);
		return;
	}
	vcpu_start("pvtest", PVTEST_BASE, PVTEST_BASE,
	    argc > 1 && streq(argv[1], "loop"));
}

static void
cmd_gdb(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!guest.active) {
		kprintf("gdb: no guest (start one stopped: pvtest stop)\n");
		return;
	}
	gdb_main();
}

/*
 * The screen while a guest that owns it is stopped from the keyboard:
 * SORIX-ILOM-LDOM's console comes back on its chip RAM block, which the guest
 * may be using, so the block and the DMA enables are saved first and
 * given back on resume, with the guest's copper list (AMIX: the view in
 * ExecingCopList, or one just loaded in LoadedCopList; the copper program
 * address at +12, +8 for the other interlace field).
 */
static u8 disp_save[DISP_CHIP_SIZE];
static u16 dmacon_save;

static void
screen_take(void)
{
	if (screen_taken || !bootcap.disp_chip)
		return;
	memcpy(disp_save, (void *)(uintptr_t)bootcap.disp_chip, DISP_CHIP_SIZE);
	dmacon_save = REG16(0xdff002);			/* DMACONR */
	display_init(bootcap.disp_chip, bootcap.vblank == 50, bootcap.font);
	kbd_init((int)bootcap.conf.keymap);
	screen_taken = 1;
	if (guest.active)
		kprintf("SORIX-ILOM-LDOM console (guest %s stopped; resume to go "
		    "back)\n", guest.name);
	else
		kprintf("SORIX-ILOM-LDOM console (guest %s has ended)\n", guest.name);
}

/*
 * AmigaOS: the copper list on display is GfxBase->LOFlist (+0x32), found
 * through exec's library list (ExecBase +378) by name.
 */
static u32
amigaos_copper(void)
{
	volatile u32 *low = (volatile u32 *)(uintptr_t)4;
	u32 eb, n, name, i;

	__asm__("" : "+a"(low));
	eb = *low;
	if ((eb & 1) || eb >= 0x10000000)
		return 0;
	for (n = *(volatile u32 *)(uintptr_t)(eb + 378); n &&
	    *(volatile u32 *)(uintptr_t)n; n = *(volatile u32 *)(uintptr_t)n) {
		name = *(volatile u32 *)(uintptr_t)(n + 10);
		for (i = 0; name && "graphics.library"[i] &&
		    *(volatile char *)(uintptr_t)(name + i) == "graphics.library"[i];
		    i++)
			;
		if (name && !"graphics.library"[i] &&
		    !*(volatile char *)(uintptr_t)(name + i))
			return *(volatile u32 *)(uintptr_t)(n + 0x32);
	}
	return 0;
}

static void
screen_give(void)
{
	u32 cl = 0, cop = 0;

	if (!screen_taken)
		return;
	display_off();
	memcpy((void *)(uintptr_t)bootcap.disp_chip, disp_save, DISP_CHIP_SIZE);
	if (guest.reset_is_hw)
		cop = amigaos_copper();
	else {
		if (amixk.execingcoplist)
			cl = *(volatile u32 *)(uintptr_t)amixk.execingcoplist;
		if (!cl && amixk.loadedcoplist)
			cl = *(volatile u32 *)(uintptr_t)amixk.loadedcoplist;
		if (cl) {
			cop = *(volatile u32 *)(uintptr_t)(cl + 12);
			if (!cop)
				cop = *(volatile u32 *)(uintptr_t)(cl + 8);
		}
	}
	if (cop) {
		*(volatile u32 *)0xdff080 = cop;	/* COP1LC */
		REG16(0xdff088) = 0;			/* COPJMP1 */
	} else
		kprintf("resume: guest copper list not found; its screen comes "
		    "back when it loads a new one\n");
	REG16(0xdff096) = (u16)(0x8000 | (dmacon_save & 0x07ff));
	screen_taken = 0;
}

/*
 * Clean shutdown of the guest: AMIX gets SIGPWR for init (see
 * amix_pv_patch), runs the powerfail entry of /etc/inittab (shutdown) and
 * halts back into this shell.
 */
static void
cmd_shutdown(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!guest.active) {
		kprintf("shutdown: no guest\n");
		return;
	}
	if (!streq(guest.name, "amix") || !amixk.shutdown_call) {
		kprintf("shutdown: %s has no shutdown request (AMIX deprivileged "
		    "with its PV patches has); use kill\n", guest.name);
		return;
	}
	kprintf("shutdown: SIGPWR to AMIX's init (the powerfail entry of "
	    "/etc/inittab runs shutdown); it halts back to this shell\n");
	vcpu_inject(amixk.shutdown_call);
	screen_give();
	vcpu_resume();
}

/* Hard stop: the guest is discarded; SORIX-ILOM-LDOM takes the machine back. */
static void
cmd_kill(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!guest.active) {
		kprintf("kill: no guest\n");
		return;
	}
	guest.active = 0;
	guest.inject_pc = 0;
	vmmu_off();
	REG16(CUSTOM_INTENA) = 0x7fff;
	REG16(CUSTOM_INTREQ) = 0x7fff;
	REG16(0xdff096) = 0x7dff;		/* DMA channels off */
	if (!screen_taken && bootcap.disp_chip)
		display_init(bootcap.disp_chip, bootcap.vblank == 50, bootcap.font);
	screen_taken = 0;			/* nothing to give back now */
	kbd_init((int)bootcap.conf.keymap);
	scsi_init();
	kprintf("kill: %s discarded%s\n", guest.name, streq(guest.name, "amix") ?
	    "; its file systems were not unmounted, the next boot checks them" :
	    "");
}

static void
cmd_resume(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (guest.active)
		screen_give();
	vcpu_resume();
}

static void
cmd_guest(int argc, char **argv)
{
	if (argc > 1 && streq(argv[1], "log")) {
		amix_log();
		return;
	}
	if (argc > 1 && streq(argv[1], "stats")) {
		vcpu_stats();
		return;
	}
	vcpu_show();
}

static void
cmd_pt(int argc, char **argv)
{
	u32 va;

	if (argc < 2 || !need_num(argv[1], &va, "address"))
		return;
	vmmu_dump(va, !(argc > 2 && streq(argv[2], "u")));
}

static void
cmd_watch(int argc, char **argv)
{
	u32 va = 0;

	if (argc > 1 && !need_num(argv[1], &va, "address"))
		return;
	vmmu_watch(va);
	kprintf(va ? "watch: guest writes to %08x are logged (guest)\n" :
	    "watch: off\n", va);
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
 * SORIX-ILOM-LDOM's boot floppy or PVMK partition can only decline its boot;
 * strap then boots the next bootable device in its priority order. Returns
 * the name of that device, or NULL.
 */
static const char *
next_boot_device(void)
{
	const struct bc_bootnode *best = NULL;
	u32 i;

	for (i = 0; i < bootcap.nbootnodes; i++) {
		const struct bc_bootnode *b = &bootcap.bootnodes[i];

		if (name_eq(b->name, "DF0") || name_eq(b->name, "PVMK") ||
		    b->pri == -128)
			continue;		/* SORIX-ILOM-LDOM's own boot media */
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

/* the last deprivileged boot, for a guest reboot and the fallback */
static char last_target[40];
static int last_nondefault;
static u32 last_start;			/* seconds since power-on */

/*
 * Does the loaded AMIX kernel fit this CPU? The 68040/68060 port
 * (amigaux.org) has a cputype variable its loader sets; the stock kernel
 * is for the 68020/68030 PMMU.
 */
static int
kernel_fits(void)
{
	if (cpuinfo.model >= 40 && !amixk.cputype) {
		kprintf("boot: this is a stock 68020/68030 kernel; the CPU is a "
		    "680%u\n", cpuinfo.model);
		return 0;
	}
	if (cpuinfo.model < 40 && amixk.cputype) {
		kprintf("boot: this is a 68040/68060 kernel (cputype); the CPU is "
		    "a 680%u\n", cpuinfo.model);
		return 0;
	}
	return 1;
}

/*
 * Boot target (dev[:path]): an AmigaOS partition (through Kickstart) or an
 * AMIX kernel, deprivileged (pv) or native. Returns only if it did not
 * boot: -1, or BOOT_ENOTFIT for a kernel that does not fit the CPU
 * (force boots it anyway).
 */
int
boot_target(const char *what, int pv, int force)
{
	struct partition *p;
	char dev[32];
	static char withcolon[34];
	u32 n = 0, sec, ms;

	if (!what || !*what) {
		kprintf("usage: boot [-pv|-native] [-f] <dev[:path]>   (see part)\n");
		return -1;
	}
	while (what[n] && what[n] != ':' && n < sizeof(dev) - 1) {
		dev[n] = what[n];
		n++;
	}
	dev[n] = 0;
	if (streq(dev, "ser") && what[n] == ':')
		goto amix;			/* a kernel sent over serial */
	if (!(p = part_find(dev))) {
		kprintf("boot: no partition %s (see part)\n", dev);
		return -1;
	}
	if (!what[n] && (p->dostype >> 8) == 0x554e49) {
		memcpy(withcolon, p->name, n);	/* "boot unix_root" */
		withcolon[n] = ':';
		withcolon[n + 1] = 0;
		what = withcolon;
	}
	if (dostype_is_amigaos(p->dostype) && pv) {
		kprintf("boot: AmigaOS deprivileged; Kickstart chooses the boot "
		    "device by its priorities (%s requested)\n", p->name);
		cmd_kick(0, NULL);
		return -1;
	}
	if (dostype_is_amigaos(p->dostype)) {
		/*
		 * Kickstart boots AmigaOS partitions after the floppy declines.
		 * (Not boot-block partitions such as AMIX's UNIX_Boot: its boot1
		 * does not start after a declined floppy boot; AMIX is loaded by
		 * SORIX-ILOM-LDOM itself from its root file system, M2.)
		 */
		const char *next = next_boot_device();

		if (!next || !name_eq(next, p->name)) {
			kprintf("boot: %s: Kickstart boots %s first after the floppy; "
			    "only that one can be selected from here\n", p->name,
			    next ? next : "(no bootable device)");
			return -1;
		}
		reboot_to_kickstart(p->name, "AmigaOS");
		return -1;
	}
	if ((p->dostype >> 8) != 0x554e49) {
		kprintf("boot: %s: no loader for this partition type\n", p->name);
		return -1;
	}
amix:
	if (pv && !cpuinfo.mmu_ok) {
		kprintf("boot: -pv needs a working MMU (MMU self-test %s)\n",
		    mmu_result());
		return -1;
	}
	if (amix_load(what))
		return -1;
	if (!kernel_fits() && !force) {
		kprintf("boot: not started (boot -f, or confirm in the menu)\n");
		return BOOT_ENOTFIT;
	}
	if (pv) {
		for (n = 0; what[n] && n < sizeof(last_target) - 1; n++)
			last_target[n] = what[n];
		last_target[n] = 0;
		last_nondefault = !menu_is_default(what);
		time_now(&sec, &ms);
		last_start = sec;
	}
	amix_start(pv);
	return -1;
}

static void
cmd_boot(int argc, char **argv)
{
	int pv = 0, force = 0, i;

	amix_mem_cap = 0;
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (streq(argv[i], "-m") && i + 1 < argc) {
			if (!need_num(argv[++i], &amix_mem_cap, "megabytes"))
				return;
			amix_mem_cap <<= 20;
		} else if (streq(argv[i], "-pv"))
			pv = 1;
		else if (streq(argv[i], "-native"))
			pv = 0;
		else if (streq(argv[i], "-f"))
			force = 1;
		else {
			kprintf("boot: unknown option %s\n", argv[i]);
			return;
		}
	}
	boot_target(i < argc ? argv[i] : bootcap.conf.def, pv, force);
}

/* once-only next boot: nextboot [-pv|-native] dev[:path] | label, or -c */
static void
cmd_nextboot(int argc, char **argv)
{
	u32 mode = ENTRY_AUTO;
	int i;

	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (streq(argv[i], "-pv"))
			mode = ENTRY_PV;
		else if (streq(argv[i], "-native"))
			mode = ENTRY_NATIVE;
		else if (streq(argv[i], "-c")) {
			nextboot_set(sstack_top(), "", 0);
			kprintf("nextboot: cleared\n");
			return;
		}
	}
	if (i >= argc) {
		kprintf("usage: nextboot [-pv|-native] <dev:path|label>, "
		    "nextboot -c\n");
		return;
	}
	nextboot_set(sstack_top(), argv[i], mode);
	kprintf("nextboot: %s once, at the next start of the boot menu\n",
	    argv[i]);
}

int
shell_getc(u32 timeout_ms)
{
	return timeout_ms ? getc_timeout(timeout_ms, 0) : getc_wait();
}

const char *
amigaos_next(void)
{
	return next_boot_device();
}

void
shell_reboot(void)
{
	cmd_reset(0, NULL);
	for (;;)
		;
}

struct kern_list {
	struct ufs *fs;
	const char *part;
	void (*cb)(void *arg, const char *path, u32 size);
	void *arg;
};

static void
kern_entry(void *arg, const char *name, const struct ufs_stat *st)
{
	struct kern_list *k = arg;
	char path[64];
	u32 n = 0, i;

	if ((st->mode & UFS_IFMT) != UFS_IFREG || st->size < 65536 ||
	    name[0] != 'u' || name[1] != 'n' || name[2] != 'i' || name[3] != 'x')
		return;
	for (i = 0; k->part[i] && n < sizeof(path) - 1; i++)
		path[n++] = k->part[i];
	for (i = 0; ":/stand/"[i] && n < sizeof(path) - 1; i++)
		path[n++] = ":/stand/"[i];
	for (i = 0; name[i] && n < sizeof(path) - 1; i++)
		path[n++] = name[i];
	path[n] = 0;
	k->cb(k->arg, path, st->size);
}

/* Kernels in /stand of an AMIX root (regular files unix*, 64 KB or more) */
int
amix_kernels(const char *part, void (*cb)(void *arg, const char *path,
    u32 size), void *arg)
{
	struct partition *p = part_find(part);
	struct ufs fs;
	struct ufs_stat st;
	struct kern_list k = { &fs, part, cb, arg };

	if (!p || ufs_open("menu", p, &fs) ||
	    ufs_lookup(&fs, "/stand", 1, &st) != UFS_OK)
		return -1;
	k.part = p->name;
	return ufs_list_dir(&fs, &st, kern_entry, &k) == UFS_OK ? 0 : -1;
}

static void
cmd_menu(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	menu_run(NULL);
}

/*
 * AmigaOS as a guest (M8, exploration): Kickstart from the ROM, entered at
 * its reset entry (ROM + 4) deprivileged, with interrupts, DMA and the
 * screen off as after a reset; RESET resets the chips instead of
 * meaning a reboot.
 */
static void
cmd_kick(int argc, char **argv)
{
	u32 entry;

	(void)argc;
	(void)argv;
	if (!cpuinfo.mmu_ok) {
		kprintf("kick: needs a working MMU\n");
		return;
	}
	entry = *(volatile u32 *)(uintptr_t)(bootcap.kick_base + 4);
	kprintf("kick: Kickstart %u.%u at %08x, entry %08x, deprivileged\n",
	    bootcap.kick_ver, bootcap.kick_rev, bootcap.kick_base, entry);
	REG16(CUSTOM_INTENA) = 0x7fff;
	REG16(CUSTOM_INTREQ) = 0x7fff;
	REG16(0xdff096) = 0x7fff;
	display_off();
	screen_taken = 0;
	vcpu_prepare("amigaos", entry, 0x400, 0);
	guest.reset_is_hw = 1;
	vcpu_resume();
}

static void
cmd_reset(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	kprintf("reset: rebooting into Kickstart\n");
	vmmu_off();
	while (!(REG16(SERDATR) & (1 << SERDATR_TSRE)))
		;
	cold_reboot();
}

/* ------------------------------------------------------------------ */
/* update: a new boot volume (pvmk.adf) onto the PVMK partition        */

#define ADF_BLOCKS	1760

static u32
bb_checksum(const u8 *bb)		/* boot block: sum with carry, inverted */
{
	u32 sum = 0, i, v;

	for (i = 0; i < 1024; i += 4) {
		v = i == 4 ? 0 : PEEK32(bb, i);
		if (sum + v < sum)
			sum++;
		sum += v;
	}
	return ~sum;
}

/*
 * update ser:<bytes> [partition]: receive a boot volume as built for the
 * floppy (pvmk.adf, 1760 blocks) over serial, check it, point its boot
 * block at the partition (default PVMK: the image's absolute first block,
 * medium hard disk, checksum), write it and read it back. A reset then
 * boots it.
 */
static void
cmd_update(int argc, char **argv)
{
	const char *name = argc > 2 ? argv[2] : "PVMK";
	u32 size, buf, back, start, isize, i;
	struct partition *p;
	u8 *pv;

	if (argc < 2 || argv[1][0] != 's' || argv[1][1] != 'e' ||
	    argv[1][2] != 'r' || argv[1][3] != ':') {
		kprintf("usage: update ser:<bytes> [partition]   (pvmk.adf, "
		    "default partition PVMK)\n");
		return;
	}
	if (!need_num(argv[1] + 4, &size, "size"))
		return;
	if (guest.active) {
		kprintf("update: a guest is active; shut it down or kill it "
		    "first\n");
		return;
	}
	if (!(p = part_find(name)) || !p->native) {
		kprintf("update: no partition %s with a native driver (see "
		    "part)\n", name);
		return;
	}
	if (size != ADF_BLOCKS * 512 || p->nblocks < ADF_BLOCKS) {
		kprintf("update: need a %u byte boot volume and a partition of "
		    "at least %u blocks (%s has %u)\n", ADF_BLOCKS * 512,
		    ADF_BLOCKS, p->name, p->nblocks);
		return;
	}
	buf = (bootcap.hole_base - size - 0x10000) & ~0xffffu;
	back = (buf - size - 0x10000) & ~0xffffu;
	if (ser_receive((u8 *)(uintptr_t)buf, size))
		return;
	pv = (u8 *)(uintptr_t)buf + PVBB_OFFSET;
	start = PEEK32(pv, PVBB_O_START);
	isize = PEEK32(pv, PVBB_O_SIZE);
	kprintf("update: crc32 %08x\n", crc32((u8 *)(uintptr_t)buf, size));
	if (PEEK32(buf, 0) != 0x444f5301 || PEEK32(pv, PVBB_O_MAGIC) !=
	    PVBB_MAGIC || start != 2 || isize > size - 1024 ||
	    crc32((u8 *)(uintptr_t)buf + 1024, isize) != PEEK32(pv, PVBB_O_CRC)) {
		kprintf("update: not a SORIX-ILOM-LDOM boot volume (or its image "
		    "CRC is wrong); nothing written\n");
		return;
	}
	PEEK32(pv, PVBB_O_START) = p->start + 2;	/* absolute on the disk */
	PEEK16(pv, PVBB_O_MEDIUM) = BOOT_HARDDISK;
	PEEK32(buf, 4) = bb_checksum((u8 *)(uintptr_t)buf);
	kprintf("update: writing %u blocks to %s (%s unit %u, blocks %u-%u)\n",
	    ADF_BLOCKS, p->name, p->device, p->unit, p->start,
	    p->start + ADF_BLOCKS - 1);
	if (part_write(p, 0, ADF_BLOCKS, (void *)(uintptr_t)buf) ||
	    part_read(p, 0, ADF_BLOCKS, (void *)(uintptr_t)back)) {
		kprintf("update: I/O error\n");
		return;
	}
	for (i = 0; i < size; i++)
		if (((u8 *)(uintptr_t)buf)[i] != ((u8 *)(uintptr_t)back)[i]) {
			kprintf("update: VERIFY FAILED at byte %u\n", i);
			return;
		}
	kprintf("update: %s written and verified; reset (without a floppy) "
	    "boots it\n", p->name);
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
	{ "md",    "[-g] <addr> [len]", "memory dump (-g: guest kernel virtual)", cmd_md },
	{ "mw",    "[.b|.w|.l] <a> <v>", "memory write (mw.b, mw.w, mw.l)", cmd_mw },
	{ "regs",  "",                  "registers of the last exception", cmd_regs },
	{ "dmesg", "[-p]",              "boot log (-p: previous boot)", cmd_dmesg },
	{ "sload", "<addr> <len>",      "receive raw bytes over serial", cmd_sload },
	{ "go",    "<addr>",            "call code at addr, print D0", cmd_go },
	{ "load",  "dev:[path]|ser:<n>", "load and relocate an AMIX kernel (ser: n bytes over serial)", cmd_load },
	{ "boot",  "[-pv] [-f] [-m MB] <dev[:path]>", "AmigaOS partition, or AMIX kernel (-pv: deprivileged, -f: any CPU, -m: AMIX memory)", cmd_boot },
	{ "nextboot", "[-pv] <target>|-c", "boot target once, at the next boot menu", cmd_nextboot },
	{ "menu",  "",                  "the boot menu", cmd_menu },
	{ "pvtest", "[loop] [stop]",    "run the virtual CPU test guest", cmd_pvtest },
	{ "gdb",   "",                  "serve GDB on the serial port (guest)", cmd_gdb },
	{ "guest", "[log|stats]",       "stopped guest: state, AMIX messages, profile", cmd_guest },
	{ "resume", "",                 "continue the stopped guest", cmd_resume },
	{ "shutdown", "",               "shut the guest down cleanly (AMIX)", cmd_shutdown },
	{ "kill",  "",                  "discard the guest (hard stop)", cmd_kill },
	{ "kick",  "",                  "AmigaOS from the Kickstart ROM, deprivileged (M8, experimental)", cmd_kick },
	{ "watch", "[addr]",            "log guest writes to addr (shadow MMU)", cmd_watch },
	{ "pt",    "<addr> [u]",        "guest page table walk (u: user view)", cmd_pt },
	{ "reset", "",                  "reboot into Kickstart", cmd_reset },
	{ "update", "ser:<n> [part]",   "write a boot volume (pvmk.adf) sent over serial to PVMK", cmd_update },
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
	if (bootcap.conf.menu) {
		menu_start();			/* returns for the shell */
		return;
	}
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

	if (gdb_active)
		gdb_main();		/* the guest stopped while GDB is attached */
	if (guest.reboot && last_target[0]) {
		u32 sec, ms;

		guest.reboot = 0;
		time_now(&sec, &ms);
		scsi_init();			/* after the guest's driver */
		if (last_nondefault && sec - last_start < bootcap.conf.fallback) {
			kprintf("boot: %s rebooted after %u s: falling back to the "
			    "default\n", last_target, sec - last_start);
			menu_run(NULL);
		} else {
			kprintf("boot: guest reboot\n");
			menu_run(last_target);
		}
	}
	guest.reboot = 0;
	if (guest.kbd_break) {
		guest.kbd_break = 0;
		screen_take();
	}
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
