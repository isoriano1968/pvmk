/*
 * Boot menu (M5), shown at startup on the screen and the serial port, in
 * the manner of the FreeBSD/illumos loader menu: entries from pvmk.conf
 * (entry=label,dev:path[,pv|native]) or, without any, one per AMIX root
 * partition (/stand/unix) and the AmigaOS partition Kickstart would boot;
 * a kernel chooser for /stand on the AMIX roots; deprivileged or native;
 * the SORIX-ILOM-LDOM shell; reboot.
 */
#ifndef PVMK_MENU_H
#define PVMK_MENU_H

#include "pvmk/types.h"

/* svc/menu.c */
void menu_start(void);		/* at startup: nextboot, then the menu */
/* Run the menu (preselect: the entry the countdown boots, NULL = the
 * default). Returns when the shell is chosen. */
void menu_run(const char *preselect);
int  menu_is_default(const char *target);

/* svc/shell.c, for the menu */
#define BOOT_ENOTFIT	(-2)
int  boot_target(const char *target, int pv, int force);
int  amix_kernels(const char *part, void (*cb)(void *arg, const char *path,
	u32 size), void *arg);
const char *amigaos_next(void);
int  shell_getc(u32 timeout_ms);	/* 0: wait */
void shell_reboot(void) __attribute__((noreturn));

#endif /* PVMK_MENU_H */
