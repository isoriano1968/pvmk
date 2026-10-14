/*
 * Console and boot log.
 *
 * con_putc() writes to the serial port and, once log_init() has run, to a
 * RAM ring that survives a warm reset: the log area holds two slots, the
 * current boot's and the previous boot's ("dmesg -p"). Every line gets a
 * timestamp from the CIA-B time-of-day counter (horizontal sync rate).
 */
#ifndef PVMK_LOG_H
#define PVMK_LOG_H

#include "pvmk/types.h"

void con_putc(int c);
void con_serial(int on);

/*
 * Interactive mode (shell): console output goes to the serial port only,
 * without timestamps and without filling the boot log. Returns the
 * previous mode. Exception reports switch it off.
 */
int  con_interactive(int on);

void time_init(u32 hsync_hz);
void time_now(u32 *sec, u32 *msec);

/* Returns 1 if a valid log from the previous boot was found. */
int  log_init(u32 area, u32 size);
int  log_prev_valid(void);
u32  log_bootcount(void);
void log_dump(int prev, void (*out)(int c));

#endif /* PVMK_LOG_H */
