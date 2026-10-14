/*
 * Microkernel-internal interfaces shared by stage0, stage1, the shell and
 * the exception code.
 */
#ifndef PVMK_MK_H
#define PVMK_MK_H

#include "pvmk/types.h"

/* core/vectors.S */
void exc_entry(void);
void restart(u32 sp, void (*fn)(void)) __attribute__((noreturn));
void set_vbr(void *vbr);
u32  get_vbr(void);
u32  get_sr(void);
u32  get_cacr(void);
long call_addr(u32 addr);
void icache_inval(u32 model);
void cold_reboot(void) __attribute__((noreturn));

/* core/exc.c */
struct excregs {
	u32 d[8];
	u32 a[8];
	u16 sr;
	u32 pc;
	u16 fmtvec;
} __attribute__((packed));

void exception(struct excregs *r);
void exc_show_last(void);
const char *exc_name(u32 vec);

/* cpu/cpu.c */
struct cpuinfo {
	u32 model;			/* 20, 30, 40, 60 */
	u32 pcr;			/* 68060 only */
	int fpu;			/* -1 none, else FSAVE idle frame size */
};
extern struct cpuinfo cpuinfo;
void cpu_probe(void);
void cpu_show(void);

/* cpu/probe.S */
extern volatile u32 probe_resume;
int probe_pcr(u32 *pcr);
int probe_urp(void);
int probe_cacr_fd(void);
int probe_fpu(void);

/* core/stage1.c */
void stage1_main(void *bootcap) __attribute__((noreturn));
u32  sstack_top(void);

/* svc/shell.c */
void shell_main(void) __attribute__((noreturn));

/* svc/crc32.c */
u32 crc32(const void *buf, u32 len);

#endif /* PVMK_MK_H */
