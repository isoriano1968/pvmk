/*
 * Boot menu: see include/pvmk/menu.h.
 */
#include "pvmk/bootcap.h"
#include "pvmk/bootreq.h"
#include "pvmk/kprintf.h"
#include "pvmk/log.h"
#include "pvmk/menu.h"
#include "pvmk/mk.h"
#include "pvmk/rdb.h"
#include "pvmk/version.h"

#define MAXENT		(CONF_MAXENT + 6)
#define MAXKERN		16

struct mentry {
	char label[24];
	char target[40];		/* dev[:path] */
	u8 mode;			/* ENTRY_* */
	u8 amigaos;
};

static struct mentry ents[MAXENT];
static u32 nents;
static int amix_pv = -1;		/* mode of ENTRY_AUTO entries */
static u8 pre_mode = ENTRY_AUTO;	/* mode of a preselected target */
static const char *pre_label = "this";	/* and its label */

static int
lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int
ieq(const char *a, const char *b)
{
	while (*a && lower(*a) == lower(*b)) {
		a++;
		b++;
	}
	return lower(*a) == lower(*b);
}

static void
copy(char *dst, u32 cap, const char *src)
{
	u32 i;

	for (i = 0; i < cap - 1 && src[i]; i++)
		dst[i] = src[i];
	dst[i] = 0;
}

static struct partition *
target_part(const char *target)
{
	char dev[32];
	u32 n = 0;

	while (target[n] && target[n] != ':' && n < sizeof(dev) - 1) {
		dev[n] = target[n];
		n++;
	}
	dev[n] = 0;
	return part_find(dev);
}

static void
add(const char *label, const char *target, u8 mode)
{
	struct partition *p = target_part(target);
	struct mentry *e;

	if (nents >= MAXENT)
		return;
	e = &ents[nents++];
	copy(e->label, sizeof(e->label), label);
	copy(e->target, sizeof(e->target), target);
	e->mode = mode;
	e->amigaos = p && dostype_is_amigaos(p->dostype);
}

static void
build(void)
{
	const struct conf *c = &bootcap.conf;
	const char *next;
	char t[40];
	u32 i, n;

	if (amix_pv < 0)
		amix_pv = cpuinfo.mmu_ok;
	nents = 0;
	for (i = 0; i < c->nentries; i++)
		add(c->entry[i].label, c->entry[i].target, c->entry[i].mode);
	if (nents)
		return;
	for (i = 0; i < nparts; i++) {		/* AMIX roots (UFS) */
		if (parts[i].dostype != 0x554e4901 || !parts[i].native)
			continue;
		for (n = 0; parts[i].name[n] && n < 24; n++)
			t[n] = parts[i].name[n];
		copy(t + n, sizeof(t) - n, ":/stand/unix");
		add("AMIX", t, ENTRY_AUTO);
	}
	next = amigaos_next();
	if (next) {
		struct partition *p = part_find(next);

		if (p && dostype_is_amigaos(p->dostype))
			add("AmigaOS", next, ENTRY_AUTO);
	}
}

/*
 * Entry index for what: an entry label or target, or a device alone
 * ("unix_root", "dh0:"), which means that device's first entry.
 */
static int
find(const char *what)
{
	struct partition *p;
	u32 i, n;

	if (!what || !*what)
		return -1;
	for (i = 0; i < nents; i++)
		if (ieq(ents[i].label, what) || ieq(ents[i].target, what))
			return (int)i;
	for (n = 0; what[n] && what[n] != ':'; n++)
		;
	if (what[n] && what[n + 1])
		return -1;			/* a path: not an entry */
	p = target_part(what);
	for (i = 0; p && i < nents; i++)
		if (target_part(ents[i].target) == p)
			return (int)i;
	return -1;
}

int
menu_is_default(const char *target)
{
	int d;

	if (!bootcap.conf.def[0])
		return 0;
	build();
	d = find(bootcap.conf.def);
	return d >= 0 ? ieq(ents[d].target, target) : ieq(bootcap.conf.def,
	    target);
}

static int
entry_pv(const struct mentry *e)
{
	if (e->amigaos)
		return 0;
	return e->mode == ENTRY_PV ? 1 : e->mode == ENTRY_NATIVE ? 0 : amix_pv;
}

static void
draw(int def)
{
	u32 i;

	kprintf("\n " SORIX_NAME " %s boot menu (680%u, %s)\n\n", PVMK_VERSION,
	    cpuinfo.model, cpuinfo.mmu_ok ? "MMU ok" : "no MMU: native only");
	for (i = 0; i < nents; i++)
		kprintf("  %u  %-9s %-34s %s%s\n", i + 1, ents[i].label,
		    ents[i].target, ents[i].amigaos ? "" : entry_pv(&ents[i]) ?
		    "deprivileged" : "native", (int)i == def ? "  [default]" : "");
	kprintf("  k  choose a kernel on an AMIX root\n");
	kprintf("  m  AMIX mode: %s (m toggles)\n", amix_pv ? "deprivileged" :
	    "native");
	kprintf("  s  " SORIX_NAME " shell\n");
	kprintf("  r  reboot\n\n");
}

/* Boot entry e; on a kernel that does not fit the CPU, ask first. */
static void
boot_entry(const struct mentry *e)
{
	int pv = entry_pv(e), c;

	if (pv && !cpuinfo.mmu_ok) {
		kprintf("menu: no working MMU, booting natively\n");
		pv = 0;
	}
	kprintf("menu: booting %s %s%s\n", e->label, e->target,
	    e->amigaos ? "" : pv ? " (deprivileged)" : " (native)");
	if (boot_target(e->target, pv, 0) != BOOT_ENOTFIT)
		return;
	kprintf("Boot it anyway (y/n)? ");
	c = shell_getc(0);
	kprintf("%c\n", c >= 0x20 && c < 0x7f ? c : ' ');
	if (lower(c) == 'y')
		boot_target(e->target, pv, 1);
}

/* ------------------------------------------------------------------ */
/* Kernel chooser                                                      */

struct kern {
	char path[48];
	u32 size;
};

static struct kern kerns[MAXKERN];
static u32 nkerns;

static void
kern_add(void *arg, const char *path, u32 size)
{
	(void)arg;
	if (nkerns >= MAXKERN)
		return;
	copy(kerns[nkerns].path, sizeof(kerns[0].path), path);
	kerns[nkerns++].size = size;
}

/* read a line (serial or keyboard); 0 if cancelled with Esc */
static int
read_path(char *buf, u32 cap)
{
	u32 n = 0;
	int c;

	for (;;) {
		c = shell_getc(0);
		if (c == 0x1b)
			return 0;
		if (c == '\r' || c == '\n') {
			buf[n] = 0;
			kprintf("\n");
			return n > 0;
		}
		if ((c == 8 || c == 0x7f) && n) {
			n--;
			kprintf("\b \b");
		} else if (c >= 0x20 && c < 0x7f && n < cap - 1) {
			buf[n++] = (char)c;
			kprintf("%c", c);
		}
	}
}

static void
choose_kernel(void)
{
	struct mentry e;
	u32 i;
	int c;

	nkerns = 0;
	for (i = 0; i < nparts; i++)
		if (parts[i].dostype == 0x554e4901 && parts[i].native)
			amix_kernels(parts[i].name, kern_add, NULL);
	kprintf("\n Kernels in /stand on the AMIX roots:\n\n");
	for (i = 0; i < nkerns; i++)
		kprintf("  %c  %-40s %8u bytes\n", i < 9 ? '1' + (int)i :
		    'a' + (int)i - 9, kerns[i].path, kerns[i].size);
	if (!nkerns)
		kprintf("  (none found)\n");
	kprintf("  p  type a path (dev:path)\n  b  back\n\nKernel: ");
	for (;;) {
		c = lower(shell_getc(0));
		if (c == 'b' || c == 0x1b) {
			kprintf("\n");
			return;
		}
		e.mode = ENTRY_AUTO;
		e.amigaos = 0;
		copy(e.label, sizeof(e.label), "AMIX");
		if (c == 'p') {
			kprintf("p\nPath: ");
			if (!read_path(e.target, sizeof(e.target)))
				return;
			boot_entry(&e);
			return;
		}
		i = c >= '1' && c <= '9' ? (u32)(c - '1') :
		    c >= 'a' && c <= 'g' ? (u32)(c - 'a' + 9) : MAXKERN;
		if (i < nkerns) {
			kprintf("%c\n", c);
			copy(e.target, sizeof(e.target), kerns[i].path);
			boot_entry(&e);
			return;
		}
	}
}

/* ------------------------------------------------------------------ */

void
menu_run(const char *preselect)
{
	int def, old = con_interactive(1), c;
	u32 t;

	build();
	def = pre_mode == ENTRY_AUTO ?
	    find(preselect ? preselect : bootcap.conf.def) : -1;
	if (def < 0 && preselect && *preselect) {
		add(pre_label, preselect, pre_mode);	/* chosen before, nextboot */
		def = (int)nents - 1;
	} else if (def < 0 && !preselect && bootcap.conf.def[0]) {
		add("default", bootcap.conf.def, ENTRY_AUTO);
		def = (int)nents - 1;
	}
	pre_mode = ENTRY_AUTO;
	pre_label = "this";
	draw(def);
	c = -1;
	if (def >= 0 && bootcap.conf.timeout) {
		kprintf("Booting %u in", def + 1);
		for (t = bootcap.conf.timeout; t > 0; t--) {
			kprintf(" %u", t);
			if ((c = shell_getc(1000)) >= 0)
				break;
		}
		kprintf("\n");
		if (c < 0) {
			boot_entry(&ents[def]);
			draw(def);
		}
	}
	for (;;) {
		if (c < 0) {
			kprintf("Select: ");
			c = shell_getc(0);
			kprintf("%c\n", c >= 0x20 && c < 0x7f ? c : ' ');
		}
		c = lower(c);
		if ((c == '\r' || c == '\n') && def >= 0)
			boot_entry(&ents[def]);
		else if (c >= '1' && c <= '9' && (u32)(c - '1') < nents)
			boot_entry(&ents[c - '1']);
		else if (c == 'k')
			choose_kernel();
		else if (c == 'm')
			amix_pv = !amix_pv && cpuinfo.mmu_ok;
		else if (c == 's' || c == 0x1b)
			break;
		else if (c == 'r')
			shell_reboot();
		else {
			c = -1;
			continue;
		}
		c = -1;
		draw(def);
	}
	con_interactive(old);
}

void
menu_start(void)
{
	char t[40];
	u32 mode;

	if (nextboot_take(sstack_top(), t, &mode)) {
		kprintf("menu: nextboot %s (once)\n", t);
		pre_mode = (u8)mode;
		pre_label = "nextboot";
		menu_run(t);
		return;
	}
	menu_run(NULL);
}
