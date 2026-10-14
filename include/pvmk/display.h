/*
 * Text console on the Amiga display (ECS/OCS, after takeover).
 *
 * One hires bitplane, 640 pixels wide: 80 x 32 characters (PAL) or
 * 80 x 25 (NTSC), 8x8 glyphs. The bitplane and the Copper list live in a
 * chip RAM block reserved by stage0.
 */
#ifndef PVMK_DISPLAY_H
#define PVMK_DISPLAY_H

#include "pvmk/types.h"

#define DISP_COLS	80
#define DISP_BPR	(DISP_COLS)		/* bytes per raster line */
#define DISP_LINES_PAL	256
#define DISP_LINES_NTSC	200
#define DISP_CHIP_SIZE	(DISP_BPR * DISP_LINES_PAL + 64)	/* plane + copper */

#define FONT_FIRST	32
#define FONT_CHARS	96			/* ' ' .. DEL */

/* Starts the display; font is FONT_CHARS glyphs of 8 bytes. */
void display_init(u32 chipmem, int pal, const u8 *font);
void display_putc(int c);	/* no-op until display_init() */

#endif /* PVMK_DISPLAY_H */
