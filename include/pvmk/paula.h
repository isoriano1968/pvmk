#ifndef PVMK_PAULA_H
#define PVMK_PAULA_H

#include "pvmk/types.h"

/* Polled Paula UART: usable with interrupts off and inside exception handlers. */
void paula_init(u32 colorclock_hz, u32 baud);
void paula_putc(int c);		/* '\n' is sent as "\r\n" */
void paula_putc_raw(int c);		/* no translation (GDB packets) */
int  paula_getc(void);		/* -1 if no character is waiting */

#endif /* PVMK_PAULA_H */
