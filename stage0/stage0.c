/*
 * pvmk stage0 — runs with Kickstart still alive.
 *
 * Capture what Kickstart knows about the machine, reserve the microkernel
 * hole at the top of fast RAM, start the boot log there, read pvmk.conf and
 * the directory from the boot floppy, then take the machine over and
 * continue in stage1 from the hole.
 *
 * Holding the left mouse button at boot skips all of this and continues
 * booting AmigaOS.
 */
#include "pvmk/amiga.h"
#include "pvmk/bootcap.h"
#include "pvmk/bootreq.h"
#include "pvmk/conf.h"
#include "pvmk/display.h"
#include "pvmk/ffs.h"
#include "pvmk/image.h"
#include "pvmk/kprintf.h"
#include "pvmk/layout.h"
#include "pvmk/log.h"
#include "pvmk/mk.h"
#include "pvmk/paula.h"
#include "pvmk/rdb.h"
#include "pvmk/scsi.h"
#include "pvmk/version.h"

#define LVO_AllocAbs		(-204)
#define LVO_CloseLibrary	(-414)
#define LVO_OpenLibrary		(-552)
#define LVO_CacheControl	(-648)
#define LVO_FindConfigDev	(-72)	/* expansion.library */
#define MEMF_REVERSE		(1 << 18)
#define CACRF_EnableD		0x00000100
#define CACRF_DBE		0x00001000
#define CACRF_WriteAllocate	0x00002000
#define CACRF_CopyBack		0x80000000
#define CIAA_PRA		0xbfe001	/* bit 6: /FIR0, left mouse button */

#define FLOPPY_ROOT		880

/* graphics.library: OpenFont(), struct TextFont */
#define LVO_OpenFont		(-72)
#define LVO_CloseFont		(-78)
#define TF_YSIZE		20
#define TF_XSIZE		24
#define TF_LOCHAR		32
#define TF_HICHAR		33
#define TF_CHARDATA		34
#define TF_MODULO		38
#define TF_CHARLOC		40

struct takeover_args {
	u32 old_base, new_base, len, reloff, nrel, cpu, sp, entry, arg;
};

u32 lib_call(void *base, s32 lvo, u32 d0, u32 d1, u32 a0, u32 a1);
u32 sorix_guest(void *sysbase);		/* libcall.S: 'SORX' under SORIX-ILOM-LDOM */
void takeover(const struct takeover_args *ta, void *sysbase)
    __attribute__((noreturn));
int stage0_main(void *sysbase, void *ioreq, u32 medium, u32 baud, u8 *base,
    u32 start);

struct bootcap bootcap;
static struct takeover_args ta;
static char confbuf[2048];

struct floppy {				/* the boot volume, floppy or partition */
	void *sysbase;
	u8 *io;
	u32 off;			/* its first block on the device */
};

static int
floppy_read(void *ctx, u32 blk, void *buf)
{
	struct floppy *f = ctx;

	PEEK16(f->io, IO_COMMAND) = CMD_READ;
	PEEK32(f->io, IO_DATA) = (u32)(uintptr_t)buf;
	PEEK32(f->io, IO_LENGTH) = FFS_BSIZE;
	PEEK32(f->io, IO_OFFSET) = (f->off + blk) * FFS_BSIZE;
	lib_call(f->sysbase, LVO_DoIO, 0, 0, 0, (u32)(uintptr_t)f->io);
	return PEEK8(f->io, IO_ERROR) ? -1 : 0;
}

static void
capture(void *sysbase, u32 medium)
{
	struct bootcap *bc = &bootcap;
	u8 *mh = (u8 *)PEEK32(sysbase, EB_MEMLIST);
	void *exp;
	u32 cd = 0, romend = 0x01000000;

	bc->medium = medium;
	bc->exec_ver = PEEK16(sysbase, EB_VERSION);
	bc->exec_rev = PEEK16(sysbase, EB_REVISION);
	bc->softver = PEEK16(sysbase, EB_SOFTVER);
	bc->attn = PEEK16(sysbase, EB_ATTNFLAGS);
	bc->vblank = PEEK8(sysbase, EB_VBLANKFREQ);
	bc->eclock = PEEK32(sysbase, EB_ECLOCKFREQ);
	bc->hsync_hz = bc->vblank == 50 ? 15625 : 15734;

	bc->kick_size = PEEK32(romend, -0x14);
	bc->kick_base = romend - bc->kick_size;
	bc->kick_ver = PEEK16(bc->kick_base, 12);
	bc->kick_rev = PEEK16(bc->kick_base, 14);

	for (; PEEK32(mh, MH_SUCC) && bc->nmem < BC_MAXMEM;
	    mh = (u8 *)PEEK32(mh, MH_SUCC)) {
		struct memregion *m = &bc->mem[bc->nmem++];

		m->attr = PEEK16(mh, MH_ATTRIBUTES);
		m->lower = PEEK32(mh, MH_LOWER);
		m->upper = PEEK32(mh, MH_UPPER);
		memcpy(bc->memhdr[bc->nmem - 1], mh, sizeof(bc->memhdr[0]));
	}

	exp = (void *)lib_call(sysbase, LVO_OpenLibrary, 0, 0, 0,
	    (u32)(uintptr_t)"expansion.library");
	if (exp) {
		while (bc->ncd < BC_MAXCD &&
		    (cd = lib_call(exp, LVO_FindConfigDev, (u32)-1, (u32)-1, cd,
		    0)) != 0)
			memcpy(bc->configdev[bc->ncd++], (void *)cd,
			    CONFIGDEV_SIZE);
		lib_call(sysbase, LVO_CloseLibrary, 0, 0, 0, (u32)(uintptr_t)exp);
	}
}

static void
show_capture(void)
{
	struct bootcap *bc = &bootcap;
	u32 i;

	kprintf("stage0: exec %u.%u (SoftVer %u), Kickstart ROM %u.%u "
	    "%08x-%08x\n", bc->exec_ver, bc->exec_rev, bc->softver,
	    bc->kick_ver, bc->kick_rev, bc->kick_base,
	    bc->kick_base + bc->kick_size - 1);
	kprintf("stage0: %s %uHz, E clock %u Hz, AttnFlags %04x\n",
	    bc->vblank == 50 ? "PAL" : "NTSC", bc->vblank, bc->eclock,
	    bc->attn);
	for (i = 0; i < bc->nmem; i++)
		kprintf("stage0: mem %08x-%08x %6uK %s (attributes %04x)\n",
		    bc->mem[i].lower, bc->mem[i].upper - 1,
		    (bc->mem[i].upper - bc->mem[i].lower) >> 10,
		    bc->mem[i].attr & MEMF_CHIP ? "chip" :
		    bc->mem[i].attr & MEMF_FAST ? "fast" : "other",
		    bc->mem[i].attr);
	kprintf("stage0: %u expansion board(s)\n", bc->ncd);
	for (i = 0; i < bc->ncd; i++) {
		const u8 *c = bc->configdev[i];

		kprintf("stage0: board %u: manufacturer %u product %u at "
		    "%08x size %uK\n", i, PEEK16(c, 20), PEEK8(c, 17),
		    PEEK32(c, 32), PEEK32(c, 36) >> 10);
	}
}

/* Is a inside an autoconfigured board (whose memory a reset takes away)? */
static int
on_board(u32 a)
{
	u32 i, b;

	for (i = 0; i < bootcap.ncd; i++) {
		b = PEEK32(bootcap.configdev[i], 32);	/* cd_BoardAddr */
		if (a >= b && a - b < PEEK32(bootcap.configdev[i], 36))
			return 1;
	}
	return 0;
}

/*
 * Reserve HOLE_SIZE at the top of the largest fast RAM region, avoiding the
 * running image; preferably not Zorro RAM, which goes away when the boards
 * are reset for an AmigaOS guest's autoconfig. Falls back to a reverse
 * allocation if that range is taken.
 */
static int
reserve_hole(void *sysbase, u8 *base, u32 alloc)
{
	struct bootcap *bc = &bootcap;
	u32 i, top, hb, r;
	int best = -1, pass;

	for (pass = 0; pass < 2 && best < 0; pass++)	/* 0: not on a board */
		for (i = 0; i < bc->nmem; i++)
			if ((bc->mem[i].attr & MEMF_FAST) &&
			    (pass || !on_board(bc->mem[i].lower)) &&
			    (best < 0 || bc->mem[i].upper - bc->mem[i].lower >
			    bc->mem[best].upper - bc->mem[best].lower))
				best = (int)i;
	if (best < 0) {
		kprintf("stage0: no fast RAM found\n");
		return -1;
	}
	top = bc->mem[best].upper & ~0xfffu;
	hb = top - HOLE_SIZE;
	if (hb >= bc->mem[best].lower &&
	    (hb >= (u32)(uintptr_t)base + alloc || top <= (u32)(uintptr_t)base) &&
	    lib_call(sysbase, LVO_AllocAbs, HOLE_SIZE, 0, 0, hb) == hb) {
		bc->hole_base = bc->hole_alloc = hb;
		bc->hole_top = top;
		bc->hole_alloc_size = HOLE_SIZE;
		return 0;
	}
	r = lib_call(sysbase, LVO_AllocMem, HOLE_SIZE + 4096,
	    MEMF_FAST | MEMF_REVERSE, 0, 0);
	if (!r) {
		kprintf("stage0: cannot reserve %uK for the microkernel\n",
		    HOLE_SIZE >> 10);
		return -1;
	}
	bc->hole_alloc = r;
	bc->hole_alloc_size = HOLE_SIZE + 4096;
	bc->hole_base = (r + 4095) & ~0xfffu;
	bc->hole_top = bc->hole_base + HOLE_SIZE;
	kprintf("stage0: top of fast RAM busy, using %08x (previous-boot log "
	    "may not be found)\n", bc->hole_base);
	return 0;
}

/*
 * Copy Kickstart's ROM font topaz 8 into bootcap.font as one byte per
 * raster line, ' '..DEL. Characters the font lacks get its "undefined"
 * glyph (the entry after HiChar).
 */
static void
capture_font(void *sysbase)
{
	struct {
		u32 name;
		u16 ysize;
		u8 style, flags;
	} __attribute__((packed)) ta = { 0, 0, 0, 0 };
	u8 *gfx, *tf, *data, *loc;
	u32 lo, hi, mod, c, r, i;

	gfx = (u8 *)lib_call(sysbase, LVO_OpenLibrary, 0, 0, 0,
	    (u32)(uintptr_t)"graphics.library");
	if (!gfx)
		return;
	ta.name = (u32)(uintptr_t)"topaz.font";
	ta.ysize = 8;
	tf = (u8 *)lib_call(gfx, LVO_OpenFont, 0, 0, (u32)(uintptr_t)&ta, 0);
	if (tf && PEEK16(tf, TF_YSIZE) == 8 && PEEK16(tf, TF_XSIZE) == 8 &&
	    PEEK8(tf, TF_LOCHAR) <= 32 && PEEK8(tf, TF_HICHAR) >= 126) {
		lo = PEEK8(tf, TF_LOCHAR);
		hi = PEEK8(tf, TF_HICHAR);
		data = (u8 *)PEEK32(tf, TF_CHARDATA);
		mod = PEEK16(tf, TF_MODULO);
		loc = (u8 *)PEEK32(tf, TF_CHARLOC);
		for (c = FONT_FIRST; c < FONT_FIRST + FONT_CHARS; c++) {
			u32 idx = c <= hi ? c - lo : hi - lo + 1;
			u32 off = PEEK16(loc, idx * 4), w = PEEK16(loc, idx * 4 + 2);

			for (r = 0; r < 8; r++) {
				u8 g = 0;

				for (i = 0; i < w && i < 8; i++) {
					u32 b = off + i;

					if (data[r * mod + (b >> 3)] & (0x80 >> (b & 7)))
						g |= (u8)(0x80 >> i);
				}
				bootcap.font[(c - FONT_FIRST) * 8 + r] = g;
			}
		}
		bootcap.font_ok = 1;
	}
	if (tf)
		lib_call(gfx, LVO_CloseFont, 0, 0, 0, (u32)(uintptr_t)tf);
	lib_call(sysbase, LVO_CloseLibrary, 0, 0, 0, (u32)(uintptr_t)gfx);
}

/* expansion.library: ExpansionBase->MountList of struct BootNode */
#define EB_MOUNTLIST		0x4a
#define BN_DEVICENODE		16
#define DN_NAME			40	/* BSTR */

static void
bootnode_name(u8 *bn, char *buf, u32 size)
{
	u8 *dn = (u8 *)PEEK32(bn, BN_DEVICENODE);
	u8 *name = dn ? (u8 *)(PEEK32(dn, DN_NAME) << 2) : NULL;
	u32 n = name ? name[0] : 0, i;

	for (i = 0; i < n && i < size - 1; i++)
		buf[i] = (char)name[1 + i];
	buf[i] = 0;
}

static int
name_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if ((*a | 0x20) != (*b | 0x20))
			return 0;
	return *a == *b;
}

/* priority of a recorded boot node, -129 if there is none of that name */
static int
bootnode_pri(const char *name)
{
	u32 i;

	for (i = 0; i < bootcap.nbootnodes; i++)
		if (name_eq(bootcap.bootnodes[i].name, name))
			return bootcap.bootnodes[i].pri;
	return -129;
}

/*
 * Record and list Kickstart's boot nodes (want == NULL), or check that a
 * boot node of that name exists (returns 0 if found or not asked).
 */
static int
bootnodes(void *sysbase, const char *want)
{
	u8 *exp, *bn, *found = NULL;
	char buf[32];

	exp = (u8 *)lib_call(sysbase, LVO_OpenLibrary, 0, 0, 0,
	    (u32)(uintptr_t)"expansion.library");
	if (!exp)
		return -1;
	for (bn = (u8 *)PEEK32(exp, EB_MOUNTLIST); PEEK32(bn, 0);
	    bn = (u8 *)PEEK32(bn, 0)) {
		bootnode_name(bn, buf, sizeof(buf));
		if (want && name_eq(buf, want))
			found = bn;
		else if (!want) {
			kprintf("stage0: boot node %s pri %d\n", buf,
			    (int)(s8)PEEK8(bn, 9));
			if (bootcap.nbootnodes < BC_MAXBOOTNODES) {
				struct bc_bootnode *b =
				    &bootcap.bootnodes[bootcap.nbootnodes++];

				memcpy(b->name, buf, sizeof(b->name));
				b->name[sizeof(b->name) - 1] = 0;
				b->pri = (s8)PEEK8(bn, 9);
			}
		}
	}
	lib_call(sysbase, LVO_CloseLibrary, 0, 0, 0, (u32)(uintptr_t)exp);
	return want && !found ? -1 : 0;
}

/*
 * Partitions through Kickstart's own disk drivers, so any controller
 * Kickstart can boot from is listed (A3000 SCSI, accelerator or Zorro
 * controllers, emulated HDFs, IDE...). Drivers and units come from the boot
 * nodes' FileSysStartupMsg; units 0-7 of each driver are tried as well, to
 * find disks without mounted partitions (AMIX-only disks, for example).
 */
#define LVO_OpenDevice		(-444)
#define LVO_CloseDevice		(-450)
#define LVO_CreateIORequest	(-654)
#define LVO_DeleteIORequest	(-660)
#define LVO_CreateMsgPort	(-666)
#define LVO_DeleteMsgPort	(-672)
#define DN_STARTUP		28	/* BPTR to struct FileSysStartupMsg */
#define FSSM_UNIT		0
#define FSSM_DEVICE		4	/* BSTR */
#define TD_READ64		24
#define IOSTD_SIZE		48

struct romdisk {
	void *sysbase;
	u8 *io;
	u8 *buf;			/* chip RAM: reachable by any DMA */
};

static int
rom_read(void *ctx, u32 blk, void *buf)
{
	struct romdisk *r = ctx;

	PEEK16(r->io, IO_COMMAND) = blk >= 0x800000 ? TD_READ64 : CMD_READ;
	PEEK32(r->io, IO_ACTUAL) = blk >> 23;		/* TD_READ64: high 32 */
	PEEK32(r->io, IO_OFFSET) = blk << 9;
	PEEK32(r->io, IO_LENGTH) = 512;
	PEEK32(r->io, IO_DATA) = (u32)(uintptr_t)r->buf;
	lib_call(r->sysbase, LVO_DoIO, 0, 0, 0, (u32)(uintptr_t)r->io);
	if (PEEK8(r->io, IO_ERROR))
		return -1;
	memcpy(buf, r->buf, 512);
	return 0;
}

struct devunit {
	char name[24];
	u32 unit;
};

/* trackdisk (floppies) and carddisk (PCMCIA) have no RDB */
static int
strcmp_trackdisk(const char *name)
{
	const char *a = "trackdisk.device", *b = "carddisk.device", *n;

	for (n = name; *a && *n == *a; n++, a++)
		;
	if (!*a && !*n)
		return 0;
	for (n = name; *b && *n == *b; n++, b++)
		;
	return *b || *n;
}

static u32
add_devunit(struct devunit *du, u32 n, const char *name, u32 unit)
{
	u32 i, k;

	for (i = 0; i < n; i++) {
		for (k = 0; name[k] && name[k] == du[i].name[k]; k++)
			;
		if (!name[k] && !du[i].name[k] && du[i].unit == unit)
			return n;
	}
	if (n == 32)
		return n;
	for (k = 0; name[k] && k < sizeof(du[n].name) - 1; k++)
		du[n].name[k] = name[k];
	du[n].name[k] = 0;
	du[n].unit = unit;
	return n + 1;
}

static void
rom_scan(void *sysbase)
{
	static struct devunit du[32];
	struct romdisk r = { sysbase, NULL, (u8 *)bootcap.scsi_chip };
	u8 *exp, *bn, *port;
	u32 n = 0, i, ndev, u;

	if (!r.buf)
		return;
	exp = (u8 *)lib_call(sysbase, LVO_OpenLibrary, 0, 0, 0,
	    (u32)(uintptr_t)"expansion.library");
	if (!exp)
		return;
	for (bn = (u8 *)PEEK32(exp, EB_MOUNTLIST); PEEK32(bn, 0);
	    bn = (u8 *)PEEK32(bn, 0)) {
		u8 *dn = (u8 *)PEEK32(bn, BN_DEVICENODE), *fssm, *bs;
		char name[24];
		u32 len;

		if (!dn || PEEK32(dn, DN_STARTUP) < 0x100)
			continue;
		fssm = (u8 *)(PEEK32(dn, DN_STARTUP) << 2);
		bs = (u8 *)(PEEK32(fssm, FSSM_DEVICE) << 2);
		if (!bs)
			continue;
		for (len = 0; len < bs[0] && len < sizeof(name) - 1 &&
		    bs[1 + len]; len++)
			name[len] = (char)bs[1 + len];
		name[len] = 0;
		if (!len || !strcmp_trackdisk(name))
			continue;
		n = add_devunit(du, n, name, PEEK32(fssm, FSSM_UNIT));
	}
	lib_call(sysbase, LVO_CloseLibrary, 0, 0, 0, (u32)(uintptr_t)exp);

	ndev = n;			/* also probe units 0-7 of each driver */
	for (i = 0; i < ndev; i++)
		for (u = 0; u < 8; u++)
			n = add_devunit(du, n, du[i].name, u);

	port = (u8 *)lib_call(sysbase, LVO_CreateMsgPort, 0, 0, 0, 0);
	r.io = port ? (u8 *)lib_call(sysbase, LVO_CreateIORequest, IOSTD_SIZE, 0,
	    (u32)(uintptr_t)port, 0) : NULL;
	for (i = 0; r.io && i < n; i++) {
		int found;

		if (lib_call(sysbase, LVO_OpenDevice, du[i].unit, 0,
		    (u32)(uintptr_t)du[i].name, (u32)(uintptr_t)r.io) & 0xff)
			continue;
		found = rdb_parse(rom_read, &r, du[i].name, du[i].unit, NATIVE_NONE);
		kprintf("stage0: %s unit %u: %s", du[i].name, du[i].unit,
		    found < 0 ? "no RDB\n" : "");
		if (found >= 0)
			kprintf("%d partition(s)\n", found);
		lib_call(sysbase, LVO_CloseDevice, 0, 0, 0, (u32)(uintptr_t)r.io);
	}
	if (r.io)
		lib_call(sysbase, LVO_DeleteIORequest, 0, 0, (u32)(uintptr_t)r.io, 0);
	if (port)
		lib_call(sysbase, LVO_DeleteMsgPort, 0, 0, (u32)(uintptr_t)port, 0);
}

/* stage0/alertpatch.S */
extern u8 ap_start[], ap_old[], ap_end[];
#define LVO_SetFunction		(-420)

/*
 * Strap reports a declining boot block with a recoverable alert
 * (AN_BootError) that waits for a mouse click. Filter exactly that one
 * alert, once; the filter then restores the original Alert(). Its few
 * bytes stay allocated for the AmigaOS session.
 */
static void
silence_boot_alert(void *sysbase)
{
	u32 size = (u32)(ap_end - ap_start);
	u8 *m = (u8 *)lib_call(sysbase, LVO_AllocMem, size, MEMF_PUBLIC, 0, 0);

	if (!m)
		return;
	memcpy(m, ap_start, size);
	if (bootcap.exec_ver >= 37)
		lib_call(sysbase, LVO_CacheClearU, 0, 0, 0, 0);
	/* SetFunction(library a1, offset a0, new d0) returns the old vector */
	PEEK32(m, ap_old - ap_start) = lib_call(sysbase, LVO_SetFunction,
	    (u32)(uintptr_t)m, 0, (u32)LVO_Alert, (u32)(uintptr_t)sysbase);
}

/*
 * Hold a colour for about three seconds, so a boot that ends here can be
 * told apart without a serial terminal.
 */
static void
hold_colour(u16 c)
{
	volatile u8 *vpos = (volatile u8 *)VHPOSR;
	int n;

	for (n = 0; n < 300; n++) {		/* line 16 and 272: 2 per frame */
		while (*vpos != 0x10)		/* the Copper may set it too */
			REG16(COLOR00) = c;
		while (*vpos == 0x10)
			REG16(COLOR00) = c;
	}
}

/*
 * Booting another device: strap tries the boot nodes in priority order and,
 * when a boot block declines (returns an error), goes on with the next one
 * in its own order (reordering the list at this point has no effect). So
 * the floppy declines once and strap boots the next bootable device, which
 * the shell has checked is the one requested; nothing is left in memory.
 */
static void
motor_off(void *sysbase, void *ioreq)
{
	if (bootcap.medium != BOOT_FLOPPY)
		return;
	PEEK16(ioreq, IO_COMMAND) = TD_MOTOR;
	PEEK32(ioreq, IO_LENGTH) = 0;
	lib_call(sysbase, LVO_DoIO, 0, 0, 0, (u32)(uintptr_t)ioreq);
}

static void
decline_boot(void *sysbase, void *ioreq, const char *name)
{
	motor_off(sysbase, ioreq);
	if (bootcap.scsi_chip)
		lib_call(sysbase, LVO_FreeMem, SCSI_DMA_BLOCKS * 512, 0, 0,
		    bootcap.scsi_chip);
	lib_call(sysbase, LVO_FreeMem, bootcap.hole_alloc_size, 0, 0,
	    bootcap.hole_alloc);
	silence_boot_alert(sysbase);
	kprintf("stage0: declining to boot from the %s; strap continues "
	    "with the next boot device (%s is requested)\n",
	    bootcap.medium == BOOT_FLOPPY ? "floppy" : "PVMK partition", name);
	while (!(REG16(SERDATR) & (1 << SERDATR_TSRE)))
		;
}

struct dirwalk {
	const char *prefix;
	struct {
		u32 blk;		/* directory header block */
		u32 file;		/* its index in bootcap.files */
	} dirs[8];
	u32 ndirs;
};

static void
add_file(void *arg, const char *name, u32 size, int is_dir, u32 blk)
{
	struct dirwalk *w = arg;
	struct bc_file *f;
	u32 n = 0;
	const char *p;

	if (bootcap.nfiles >= BC_MAXFILES)
		return;
	f = &bootcap.files[bootcap.nfiles++];
	for (p = w->prefix; *p && n < sizeof(f->path) - 1; p++)
		f->path[n++] = *p;
	for (p = name; *p && n < sizeof(f->path) - 1; p++)
		f->path[n++] = *p;
	f->path[n] = 0;
	f->size = is_dir ? 0 : size;
	f->is_dir = (u8)is_dir;
	if (is_dir && w->ndirs < 8) {
		w->dirs[w->ndirs].blk = blk;
		w->dirs[w->ndirs++].file = bootcap.nfiles - 1;
	}
}

static void
read_bootvol(void *sysbase, void *ioreq)
{
	struct floppy fl = { sysbase, ioreq, bootcap.boot_part };
	const char *vol = bootcap.medium == BOOT_FLOPPY ? "DF0" : "PVMK";
	struct ffs fs;
	struct dirwalk w = { .prefix = "" };
	char prefix[sizeof(bootcap.files[0].path) + 1];
	u32 blk, size, i, n, first_sub;
	int err, is_dir;

	fs.read = floppy_read;
	fs.ctx = &fl;
	fs.root = FLOPPY_ROOT;
	ffs_set_dostype(&fs, 0x444f5301);	/* our floppy: DOS\1 */

	err = ffs_lookup(&fs, "pvmk.conf", &blk, &is_dir);
	if (!err && !is_dir)
		err = ffs_read_file(&fs, blk, confbuf, sizeof(confbuf) - 1, &size);
	if (err) {
		kprintf("stage0: conf: %s:pvmk.conf: %s, using defaults\n", vol,
		    ffs_strerror(err));
	} else {
		confbuf[size] = 0;
		conf_parse(&bootcap.conf, confbuf, size);
		bootcap.conf_loaded = 1;
		kprintf("stage0: conf: %s:pvmk.conf loaded (%u bytes)\n", vol, size);
	}

	err = ffs_list_dir(&fs, FLOPPY_ROOT, add_file, &w);
	first_sub = w.ndirs;
	/* one level of subdirectories, entries prefixed with "dir/" */
	for (i = 0; !err && i < first_sub; i++) {
		const char *p = bootcap.files[w.dirs[i].file].path;

		for (n = 0; *p && n < sizeof(prefix) - 2; )
			prefix[n++] = *p++;
		prefix[n++] = '/';
		prefix[n] = 0;
		w.prefix = prefix;
		err = ffs_list_dir(&fs, w.dirs[i].blk, add_file, &w);
	}
	if (err)
		kprintf("stage0: %s: directory: %s\n", vol, ffs_strerror(err));
	kprintf("stage0: %s: %u entries cached\n", bootcap.medium == BOOT_FLOPPY ?
	    "floppy" : "PVMK", bootcap.nfiles);
	if (bootcap.medium == BOOT_FLOPPY) {
		motor_off(sysbase, ioreq);
		kprintf("stage0: floppy motor off\n");
	}
}

int
stage0_main(void *sysbase, void *ioreq, u32 medium, u32 baud, u8 *base,
    u32 start)
{
	u32 bss_end = PEEK32(base, PVMK_O_BSSEND);
	u32 nrel = PEEK32(base, PVMK_O_RELCNT);
	u32 len = bss_end + nrel * 4;

	paula_init(PEEK32(sysbase, EB_ECLOCKFREQ) * 5, baud);
	REG16(COLOR00) = 0x0f0;		/* green: stage0 running */

	if (!(*(volatile u8 *)CIAA_PRA & 0x40)) {
		kprintf("\n" SORIX_NAME ": left mouse button held, "
		    "continuing AmigaOS boot\n");
		return 0;
	}

	/*
	 * Booted by an AmigaOS that itself runs as a guest of SORIX-ILOM-LDOM (the
	 * AmigaOS personality): not again; the floppy is declined and
	 * Kickstart boots from the next device.
	 */
	if (sorix_guest(sysbase) == 0x534f5258) {
		kprintf("\n" SORIX_NAME ": this AmigaOS runs under SORIX-ILOM-LDOM; "
		    "declining the floppy, it boots from the next device\n");
		bootcap.medium = medium;
		motor_off(sysbase, ioreq);
		silence_boot_alert(sysbase);
		hold_colour(0x80f);		/* violet: declined, a guest */
		return BOOTBLOCK_DECLINE;
	}

	conf_defaults(&bootcap.conf);
	capture(sysbase, medium);
	bootcap.boot_part = start - 2;		/* the image is volume block 2 */
	if (reserve_hole(sysbase, base, len))
		return 0;	/* fail safe: let AmigaOS boot */
	time_init(bootcap.hsync_hz);
	log_init(bootcap.hole_top - LOG_AREA_SIZE, LOG_AREA_SIZE);

	kprintf("\n" SORIX_NAME " " PVMK_VERSION " - stage0 (boot %u)\n",
	    log_bootcount());
	if (log_prev_valid())
		kprintf("stage0: previous boot log found (dmesg -p)\n");
	kprintf("stage0: boot medium %s, image at %p, %u bytes + %u relocs\n",
	    medium == BOOT_FLOPPY ? "floppy" :
	    medium == BOOT_HARDDISK ? "hard disk" : "unknown",
	    base, bss_end, nrel);
	show_capture();
	bootnodes(sysbase, NULL);
	bootcap.scsi_chip = lib_call(sysbase, LVO_AllocMem, SCSI_DMA_BLOCKS * 512,
	    MEMF_CHIP | MEMF_PUBLIC, 0, 0);

	{
		char name[32];

		if (bootreq_take(bootcap.hole_top - LOG_AREA_SIZE, name) ==
		    BOOTREQ_AMIGAOS) {
			kprintf("stage0: boot request: AmigaOS from %s\n", name);
			if (bootnodes(sysbase, name) == 0) {
				/*
				 * A PVMK boot partition before the target in
				 * strap's order boots a SORIX-ILOM-LDOM next:
				 * leave the request for it to decline too.
				 */
				if (medium == BOOT_FLOPPY &&
				    bootnode_pri("PVMK") >= bootnode_pri(name)) {
					bootreq_set(bootcap.hole_top - LOG_AREA_SIZE,
					    BOOTREQ_AMIGAOS, name);
					kprintf("stage0: the PVMK partition boots "
					    "first; it declines as well\n");
				}
				decline_boot(sysbase, ioreq, name);
				hold_colour(0x088);	/* teal: declined, request */
				return BOOTBLOCK_DECLINE;
			}
			kprintf("stage0: cannot boot %s, staying in "
			    SORIX_NAME "\n", name);
		}
	}
	rom_scan(sysbase);
	kprintf("stage0: hole %08x-%08x (%uK)\n", bootcap.hole_base,
	    bootcap.hole_top - 1, HOLE_SIZE >> 10);

	read_bootvol(sysbase, ioreq);
	if (bootcap.conf.display) {
		capture_font(sysbase);
		bootcap.disp_chip = bootcap.font_ok ? lib_call(sysbase,
		    LVO_AllocMem, DISP_CHIP_SIZE, MEMF_CHIP | MEMF_PUBLIC, 0, 0) : 0;
		if (bootcap.disp_chip)
			kprintf("stage0: display: topaz 8 font, %u bytes chip RAM at "
			    "%08x\n", DISP_CHIP_SIZE, bootcap.disp_chip);
		else
			kprintf("stage0: display: unavailable (%s)\n",
			    bootcap.font_ok ? "no chip RAM" : "no 8x8 font");
	}
	if (bootcap.conf.baud != baud) {
		kprintf("stage0: serial: switching to %u baud\n",
		    bootcap.conf.baud);
		paula_init(bootcap.eclock * 5, bootcap.conf.baud);
	}

	if (len + SSTACK_SIZE + LOG_AREA_SIZE > HOLE_SIZE) {
		kprintf("stage0: image too large for the hole (%u bytes)\n", len);
		return 0;
	}
	bootcap.image_base = bootcap.hole_base;
	bootcap.image_size = len;

	ta.old_base = (u32)(uintptr_t)base;
	ta.new_base = bootcap.hole_base;
	ta.len = (len + 3) & ~3u;
	ta.reloff = bss_end;
	ta.nrel = nrel;
	ta.cpu = bootcap.attn & ((1 << AFB_68040) | (1 << AFB_68060)) ?
	    (bootcap.attn & (1 << AFB_68060) ? 60 : 40) : 30;
	ta.sp = bootcap.hole_top - LOG_AREA_SIZE;
	ta.entry = (u32)(uintptr_t)stage1_main;
	ta.arg = (u32)(uintptr_t)&bootcap;

	kprintf("stage0: takeover: moving image to %08x, goodbye Kickstart\n",
	    ta.new_base);
	if (bootcap.exec_ver >= 37)
		lib_call(sysbase, LVO_CacheControl, 0, CACRF_EnableD | CACRF_DBE |
		    CACRF_WriteAllocate | CACRF_CopyBack, 0, 0);
	takeover(&ta, sysbase);
}
