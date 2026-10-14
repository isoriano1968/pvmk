/*
 * Text console on the Amiga display.
 *
 * The CPU draws glyphs into a single hires bitplane; the Copper reloads the
 * bitplane pointer every frame. Scrolling is a CPU copy of the plane.
 */
#include "pvmk/display.h"
#include "pvmk/kprintf.h"

#define C(off)		(*(volatile u16 *)(0xdff000 + (off)))
#define DMACON		0x096
#define COP1LC		0x080
#define COPJMP1		0x088
#define DIWSTRT		0x08e
#define DIWSTOP		0x090
#define DDFSTRT		0x092
#define DDFSTOP		0x094
#define BPLCON0		0x100
#define BPLCON1		0x102
#define BPLCON2		0x104
#define BPL1MOD		0x108
#define BPL1PTH		0x0e0
#define BPL1PTL		0x0e2
#define COLOR00		0x180
#define COLOR01		0x182
#define FMODE		0x1fc

#define COL_BG		0x003		/* dark blue, as in stage1 */
#define COL_FG		0xccc

static u8 *plane;
static const u8 *glyphs;
static u32 rows, row, col;

static void
cursor(void)
{
	/* underline cursor: XOR the glyph cell's last raster line */
	plane[(row * 8 + 7) * DISP_BPR + col] ^= 0xff;
}

static void
scroll(void)
{
	u32 *d = (u32 *)plane, *s = (u32 *)(plane + 8 * DISP_BPR);
	u32 n = (rows - 1) * 8 * DISP_BPR / 4, i;

	for (i = 0; i < n; i++)
		*d++ = *s++;
	for (i = 0; i < 8 * DISP_BPR / 4; i++)
		*d++ = 0;
}

static void
newline(void)
{
	col = 0;
	if (++row == rows) {
		scroll();
		row = rows - 1;
	}
}

static void
draw(int c)
{
	const u8 *g;
	u8 *p = plane + row * 8 * DISP_BPR + col;
	int i;

	if (c < FONT_FIRST || c >= FONT_FIRST + FONT_CHARS)
		c = '?';
	g = glyphs + (c - FONT_FIRST) * 8;
	for (i = 0; i < 8; i++)
		p[i * DISP_BPR] = g[i];
	if (++col == DISP_COLS)
		newline();
}

void
display_putc(int c)
{
	if (!plane)
		return;
	cursor();
	switch (c) {
	case '\n':
		newline();
		break;
	case '\r':
		col = 0;
		break;
	case '\b':
		if (col)
			col--;
		break;
	case '\t':
		do
			draw(' ');
		while (col % 8);
		break;
	default:
		if (c >= 0x20)
			draw(c);
		break;
	}
	cursor();
}

void
display_init(u32 chipmem, int pal, const u8 *font)
{
	u32 lines = pal ? DISP_LINES_PAL : DISP_LINES_NTSC;
	u16 *cop = (u16 *)(chipmem + DISP_BPR * DISP_LINES_PAL);

	plane = (u8 *)chipmem;
	glyphs = font;
	rows = lines / 8;
	row = col = 0;
	memset(plane, 0, DISP_BPR * lines);

	cop[0] = BPL1PTH;
	cop[1] = (u16)(chipmem >> 16);
	cop[2] = BPL1PTL;
	cop[3] = (u16)chipmem;
	cop[4] = 0xffff;			/* wait forever */
	cop[5] = 0xfffe;

	C(DMACON) = 0x7fff;
	C(FMODE) = 0;				/* AGA: OCS-compatible fetch */
	C(BPLCON0) = 0x9200;			/* hires, 1 plane, colour */
	C(BPLCON1) = 0;
	C(BPLCON2) = 0;
	C(BPL1MOD) = 0;
	C(DIWSTRT) = 0x2c81;
	C(DIWSTOP) = pal ? 0x2cc1 : 0xf4c1;	/* 256 or 200 lines */
	C(DDFSTRT) = 0x003c;
	C(DDFSTOP) = 0x00d4;
	C(COLOR00) = COL_BG;
	C(COLOR01) = COL_FG;
	*(volatile u32 *)(0xdff000 + COP1LC) = (u32)(uintptr_t)cop;
	C(COPJMP1) = 0;
	C(DMACON) = 0x8380;			/* DMAEN | BPLEN | COPEN */
	cursor();
}
