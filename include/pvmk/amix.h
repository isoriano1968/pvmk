/*
 * AMIX personality: loading the kernel (relocatable ELF, as /stand/unix)
 * and starting it the way AMIX's own boot2/copyit do (boot method 3,
 * "EXEC1": D0 = 3, D1 = &struct bootinfo).
 */
#ifndef PVMK_AMIX_H
#define PVMK_AMIX_H

#include "pvmk/types.h"

#define AMIX_EXEC1		3
#define AMIX_DEFAULT_PATH	"/stand/unix"
#define AMIX_NAUTO		16

/* struct BootData as in AMIX's amiga/boot/bootdata.h (10 longs) */
struct amix_bootdata {
	u32 entry;
	u32 tvaddr, toffset, tsize;
	u32 rvaddr, roffset, rsize;
	u32 dvaddr, doffset, dsize;
};

/* struct bootinfo as built by AMIX's boot2 (bootinfo.h, exec structures
 * with 2-byte packing) */
struct amix_bootinfo {
	u8 autocon[AMIX_NAUTO][0x44];	/* struct ConfigDev */
	u8 memory[AMIX_NAUTO][32];	/* struct MemHeader */
	u8 keystates[16];
	struct amix_bootdata bootdata;
	u8 reserved[256];
} __attribute__((packed));

struct amix_kernel {
	struct amix_bootdata bd;	/* where text/rodata/data go */
	u32 end;			/* end of bss (incl. COMMON symbols) */
	u32 nrel;			/* relocations applied */
	u32 type;			/* ELF e_type */
};

/* Relocate an ELF kernel image in place for text at base. 0 on success;
 * otherwise a message has been printed. */
int amix_relocate(u8 *elf, u32 size, u32 base, struct amix_kernel *k);

/* AMIX's choice of kernel address: start of the largest non-chip memory
 * region, rounded down to 256 bytes (amiga/boot/bind.c). */
u32 amix_bind_address(void);

/* core/amixenter.S: copy the sections into place and jump to the kernel.
 * Does not return. */
struct amix_handoff {
	u32 sp;				/* initial stack (chip RAM) */
	u32 entry, d0, d1;
	u32 tbuf, tvaddr, tsize;
	u32 rbuf, rvaddr, rsize;
	u32 dbuf, dvaddr, dsize;
	u32 cpu;			/* 30, 40, 60: cache flush */
};
void amix_enter(const struct amix_handoff *h) __attribute__((noreturn));

#endif /* PVMK_AMIX_H */
