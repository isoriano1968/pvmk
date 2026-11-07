/*
 * What stage0 captures from Kickstart before takeover. Lives in the image's
 * bss, so it moves with the image into the hole.
 */
#ifndef PVMK_BOOTCAP_H
#define PVMK_BOOTCAP_H

#include "pvmk/types.h"

#define BC_MAXMEM	16
#define BC_MAXCD	16		/* NAUTO in AMIX bootinfo.h */
#define BC_MAXFILES	48
#define CONFIGDEV_SIZE	0x44		/* struct ConfigDev */
#define BC_MAXBOOTNODES	16

struct memregion {
	u32 lower, upper;		/* [lower, upper) */
	u16 attr;			/* MEMF_* */
};

struct bc_file {
	char path[40];
	u32 size;
	u8 is_dir;
};

struct bc_bootnode {
	char name[24];
	s8 pri;
};

#define CONF_MAXENT	8

/* boot menu entry: entry=label,dev:path[,pv|native] */
struct conf_entry {
	char label[24];
	char target[40];		/* dev[:path] */
	u8 mode;			/* ENTRY_* */
};
#define ENTRY_AUTO	0		/* conf.pv (mode=), if the MMU allows */
#define ENTRY_PV	1
#define ENTRY_NATIVE	2

struct conf {
	char serial[8];			/* debug | guest | shared */
	u32 baud;
	char def[40];			/* default: entry label or dev[:path] */
	u32 timeout;			/* autoboot seconds */
	u32 loglevel;
	u32 display;			/* text console on the screen */
	u32 keymap;			/* KBD_US, KBD_DE */
	u32 menu;			/* boot menu at startup */
	u32 fallback;			/* seconds: a quicker reboot falls back */
	u32 pv;				/* mode=: ENTRY_AUTO and chooser boots
					   deprivileged (1) or native (0) */
	u32 nentries;
	struct conf_entry entry[CONF_MAXENT];
	u32 mem;			/* mem=: MB for a native AMIX, 0 = all */
	u32 z3reloc;			/* z3reloc=yes: move the Z3660 boards before AMIX */
};

struct bootcap {
	u32 medium;			/* BOOT_* */
	u32 boot_part;			/* boot volume's first block on its
					 * device: 0 floppy, else the PVMK
					 * partition's start */
	u16 exec_ver, exec_rev, softver, attn;
	u8 vblank;
	u32 eclock;
	u32 hsync_hz;			/* CIA-B TOD rate */
	u16 kick_ver, kick_rev;
	u32 kick_base, kick_size;

	u32 nmem;
	struct memregion mem[BC_MAXMEM];
	u8 memhdr[BC_MAXMEM][32];	/* raw struct MemHeader, for AMIX */
	u32 ncd;
	u8 configdev[BC_MAXCD][CONFIGDEV_SIZE];
	u32 nfiles;
	struct bc_file files[BC_MAXFILES];
	u32 nbootnodes;			/* Kickstart's boot list, in order */
	struct bc_bootnode bootnodes[BC_MAXBOOTNODES];

	int conf_loaded;
	struct conf conf;

	u32 hole_base, hole_top;
	u32 hole_alloc, hole_alloc_size;	/* as allocated from exec */
	u32 image_base, image_size;	/* after takeover */

	u32 disp_chip;			/* chip RAM for the text display, or 0 */
	u32 scsi_chip;			/* chip RAM SCSI DMA bounce buffer, or 0 */
	int font_ok;
	u8 font[96 * 8];		/* topaz 8 from Kickstart, ' '..DEL */
};

extern struct bootcap bootcap;

#endif /* PVMK_BOOTCAP_H */
