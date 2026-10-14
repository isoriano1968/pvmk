/*
 * Amiga keyboard, polled through CIA-A's serial port (after takeover, with
 * interrupts masked). US and German key maps.
 */
#ifndef PVMK_KEYBOARD_H
#define PVMK_KEYBOARD_H

#define KBD_US		0
#define KBD_DE		1		/* Amiga German keyboard */
#define KBD_DE_PC	2		/* German PC keyboard under an emulator */

void kbd_init(int keymap);

/*
 * Next character typed, or -1. Cursor up/down/right/left are returned as
 * ANSI sequences (ESC [ A..D), Ctrl+letter as control codes, Return as '\r'.
 */
int kbd_getc(void);

#endif /* PVMK_KEYBOARD_H */
