/*
 * GDB remote serial protocol stub for the (stopped) guest, on the serial
 * port. While GDB is attached the console stays off the serial port
 * (screen and log only); guest console output goes to GDB.
 */
#ifndef PVMK_GDB_H
#define PVMK_GDB_H

#include "pvmk/types.h"

extern int gdb_active;			/* GDB owns the serial port */
extern int gdb_step;			/* single-step in progress */

/* Serve GDB until it detaches or kills the guest. Called from the shell
 * ("gdb") and again whenever the guest stops while GDB is attached. */
void gdb_main(void);

int  gdb_breakpoint_at(u32 addr);	/* a GDB software breakpoint there */
void gdb_console(const u8 *buf, u32 len);	/* guest output to GDB */

/* core/vcpu_s.S */
int safe_copy(void *dst, const void *src, u32 n);

#endif /* PVMK_GDB_H */
