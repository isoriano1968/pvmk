/*
 * One-shot boot request, kept in the boot log area's header (which
 * survives a warm reset). The shell sets it and reboots; stage0 takes it
 * on the next boot, while Kickstart is still alive.
 */
#ifndef PVMK_BOOTREQ_H
#define PVMK_BOOTREQ_H

#include "pvmk/types.h"

#define BOOTREQ_AMIGAOS	1		/* let Kickstart boot this DOS device */

void bootreq_set(u32 logarea, u32 kind, const char *name);

/* Returns the kind and fills name[32] if a valid request was pending;
 * the request is cleared either way. 0 if none. */
u32  bootreq_take(u32 logarea, char *name);

/*
 * Once-only next boot ("nextboot"): a boot menu target and mode, kept
 * beside the boot request; stage0 leaves it alone, the boot menu takes
 * it. An empty target clears it.
 */
void nextboot_set(u32 logarea, const char *target, u32 mode);
/* 1 and target[40], *mode if one was pending; cleared either way */
int  nextboot_take(u32 logarea, char *target, u32 *mode);

#endif /* PVMK_BOOTREQ_H */
