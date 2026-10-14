/*
 * Boot monitor shell. Input from the serial port or the Amiga keyboard;
 * output to the console (serial and screen).
 *
 * Numbers are hexadecimal unless prefixed with '#' (decimal); a "0x"
 * prefix is accepted.
 */
#include "pvmk/amiga.h"
#include "pvmk/bootcap.h"
#include "pvmk/conf.h"
#include "pvmk/keyboard.h"
#include "pvmk/kprintf.h"
#include "pvmk/layout.h"
#include "pvmk/log.h"
#include "pvmk/mk.h"
#include "pvmk/paula.h"
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

static void
cmd_ls(int argc, char **argv)
{
	u32 i;

	(void)argc;
	(void)argv;
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

static void
cmd_boot(int argc, char **argv)
{
	const char *what = argc > 1 ? argv[1] : bootcap.conf.def;

	kprintf("boot: '%s': personality loaders arrive in M2\n",
	    *what ? what : "(no default)");
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
	{ "ls",    "",                  "boot floppy directory", cmd_ls },
	{ "md",    "<addr> [len]",      "memory dump", cmd_md },
	{ "mw",    "[.b|.w|.l] <a> <v>", "memory write (mw.b, mw.w, mw.l)", cmd_mw },
	{ "regs",  "",                  "registers of the last exception", cmd_regs },
	{ "dmesg", "[-p]",              "boot log (-p: previous boot)", cmd_dmesg },
	{ "sload", "<addr> <len>",      "receive raw bytes over serial", cmd_sload },
	{ "go",    "<addr>",            "call code at addr, print D0", cmd_go },
	{ "boot",  "[personality]",     "boot a personality (M2)", cmd_boot },
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
