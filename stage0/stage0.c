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
#include "pvmk/conf.h"
#include "pvmk/display.h"
#include "pvmk/ffs.h"
#include "pvmk/image.h"
#include "pvmk/kprintf.h"
#include "pvmk/layout.h"
#include "pvmk/log.h"
#include "pvmk/mk.h"
#include "pvmk/paula.h"
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
void takeover(const struct takeover_args *ta, void *sysbase)
    __attribute__((noreturn));
int stage0_main(void *sysbase, void *ioreq, u32 medium, u32 baud, u8 *base);

struct bootcap bootcap;
static struct takeover_args ta;
static char confbuf[2048];

struct floppy {
	void *sysbase;
	u8 *io;
};

static int
floppy_read(void *ctx, u32 blk, void *buf)
{
	struct floppy *f = ctx;

	PEEK16(f->io, IO_COMMAND) = CMD_READ;
	PEEK32(f->io, IO_DATA) = (u32)(uintptr_t)buf;
	PEEK32(f->io, IO_LENGTH) = FFS_BSIZE;
	PEEK32(f->io, IO_OFFSET) = blk * FFS_BSIZE;
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
		kprintf("stage0: mem %08x-%08x %6uK %s\n", bc->mem[i].lower,
		    bc->mem[i].upper - 1,
		    (bc->mem[i].upper - bc->mem[i].lower) >> 10,
		    bc->mem[i].attr & MEMF_CHIP ? "chip" :
		    bc->mem[i].attr & MEMF_FAST ? "fast" : "other");
	kprintf("stage0: %u expansion board(s)\n", bc->ncd);
	for (i = 0; i < bc->ncd; i++) {
		const u8 *c = bc->configdev[i];

		kprintf("stage0: board %u: manufacturer %u product %u at "
		    "%08x size %uK\n", i, PEEK16(c, 20), PEEK8(c, 17),
		    PEEK32(c, 32), PEEK32(c, 36) >> 10);
	}
}

/*
 * Reserve HOLE_SIZE at the top of the largest fast RAM region, avoiding the
 * running image. Falls back to a reverse allocation if that range is taken.
 */
static int
reserve_hole(void *sysbase, u8 *base, u32 alloc)
{
	struct bootcap *bc = &bootcap;
	u32 best = 0, i, top, hb, r;

	for (i = 0; i < bc->nmem; i++)
		if ((bc->mem[i].attr & MEMF_FAST) &&
		    bc->mem[i].upper - bc->mem[i].lower >
		    bc->mem[best].upper - bc->mem[best].lower)
			best = i;
	if (!(bc->mem[best].attr & MEMF_FAST)) {
		kprintf("stage0: no fast RAM found\n");
		return -1;
	}
	top = bc->mem[best].upper & ~0xfffu;
	hb = top - HOLE_SIZE;
	if (hb >= bc->mem[best].lower &&
	    (hb >= (u32)(uintptr_t)base + alloc || top <= (u32)(uintptr_t)base) &&
	    lib_call(sysbase, LVO_AllocAbs, HOLE_SIZE, 0, 0, hb) == hb) {
		bc->hole_base = hb;
		bc->hole_top = top;
		return 0;
	}
	r = lib_call(sysbase, LVO_AllocMem, HOLE_SIZE + 4096,
	    MEMF_FAST | MEMF_REVERSE, 0, 0);
	if (!r) {
		kprintf("stage0: cannot reserve %uK for the microkernel\n",
		    HOLE_SIZE >> 10);
		return -1;
	}
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
read_floppy(void *sysbase, void *ioreq)
{
	struct floppy fl = { sysbase, ioreq };
	struct ffs fs;
	struct dirwalk w = { .prefix = "" };
	char prefix[sizeof(bootcap.files[0].path) + 1];
	u32 blk, size, i, n, first_sub;
	int err, is_dir;

	fs.read = floppy_read;
	fs.ctx = &fl;
	fs.root = FLOPPY_ROOT;

	err = ffs_lookup(&fs, "pvmk.conf", &blk, &is_dir);
	if (!err && !is_dir)
		err = ffs_read_file(&fs, blk, confbuf, sizeof(confbuf) - 1, &size);
	if (err) {
		kprintf("stage0: conf: DF0:pvmk.conf: %s, using defaults\n",
		    ffs_strerror(err));
	} else {
		confbuf[size] = 0;
		conf_parse(&bootcap.conf, confbuf, size);
		bootcap.conf_loaded = 1;
		kprintf("stage0: conf: DF0:pvmk.conf loaded (%u bytes)\n", size);
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
		kprintf("stage0: floppy directory: %s\n", ffs_strerror(err));
	kprintf("stage0: floppy: %u entries cached\n", bootcap.nfiles);

	PEEK16(ioreq, IO_COMMAND) = TD_MOTOR;
	PEEK32(ioreq, IO_LENGTH) = 0;
	lib_call(sysbase, LVO_DoIO, 0, 0, 0, (u32)(uintptr_t)ioreq);
	kprintf("stage0: floppy motor off\n");
}

int
stage0_main(void *sysbase, void *ioreq, u32 medium, u32 baud, u8 *base)
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

	conf_defaults(&bootcap.conf);
	capture(sysbase, medium);
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
	kprintf("stage0: hole %08x-%08x (%uK)\n", bootcap.hole_base,
	    bootcap.hole_top - 1, HOLE_SIZE >> 10);

	if (medium == BOOT_FLOPPY)
		read_floppy(sysbase, ioreq);
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
