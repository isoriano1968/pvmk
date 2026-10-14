/*
 * Amiga keyboard driver, polled.
 *
 * The keyboard shifts each key event into CIA-A's serial data register and
 * waits for a handshake: the host pulls KDAT low (serial port to output
 * mode) for at least 85 us. The byte is inverted and rotated: after
 * decoding, bits 0-6 are the raw key code and bit 7 is set on key release.
 *
 * Key maps: US, German (Amiga keyboard) and German PC keyboard under an
 * emulator, which differs in two keys: the PC "^" key arrives as raw 0x00
 * and the PC "#" key (next to Return) as raw 0x0D. Characters outside ASCII
 * (umlauts, sharp s) are not produced. On both German maps, right Alt
 * (AltGr on a PC keyboard) gives \ @ { [ ] } | ~ at their PC positions.
 */
#include "pvmk/keyboard.h"
#include "pvmk/types.h"

#define CIAA_SDR	0xbfec01
#define CIAA_ICR	0xbfed01
#define CIAA_CRA	0xbfee01
#define ICR_SP		0x08
#define CRA_SPMODE	0x40
#define VHPOSR		0xdff006

#define K_LSHIFT	0x60
#define K_RSHIFT	0x61
#define K_CAPS		0x62
#define K_CTRL		0x63
#define K_RALT		0x65
#define K_UP		0x4c
#define K_DOWN		0x4d
#define K_RIGHT		0x4e
#define K_LEFT		0x4f

#define CIA(a)		(*(volatile u8 *)(a))

/* Raw key codes 0x00-0x4a, [map][shift]; 0 = no character. */
static const char keymap[2][2][0x4b] = {
	{	/* us */
	{
		'`', '1', '2', '3', '4', '5', '6', '7',	/* 00 */
		'8', '9', '0', '-', '=', '\\', 0, '0',
		'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',	/* 10 */
		'o', 'p', '[', ']', 0, '1', '2', '3',
		'a', 's', 'd', 'f', 'g', 'h', 'j', 'k',	/* 20 */
		'l', ';', '\'', 0, 0, '4', '5', '6',
		0, 'z', 'x', 'c', 'v', 'b', 'n', 'm',	/* 30 */
		',', '.', '/', 0, '.', '7', '8', '9',
		' ', 0x08, '\t', '\r', '\r', 0x1b, 0x7f, 0,	/* 40 */
		0, 0, '-',
	},
	{
		'~', '!', '@', '#', '$', '%', '^', '&',
		'*', '(', ')', '_', '+', '|', 0, '0',
		'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
		'O', 'P', '{', '}', 0, '1', '2', '3',
		'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K',
		'L', ':', '"', 0, 0, '4', '5', '6',
		0, 'Z', 'X', 'C', 'V', 'B', 'N', 'M',
		'<', '>', '?', 0, '.', '7', '8', '9',
		' ', 0x08, '\t', '\r', '\r', 0x1b, 0x7f, 0,
		0, 0, '-',
	},
	},
	{	/* de */
	{
		'`', '1', '2', '3', '4', '5', '6', '7',	/* 00 */
		'8', '9', '0', 0, '\'', '\\', 0, '0',
		'q', 'w', 'e', 'r', 't', 'z', 'u', 'i',	/* 10 */
		'o', 'p', 0, '+', 0, '1', '2', '3',
		'a', 's', 'd', 'f', 'g', 'h', 'j', 'k',	/* 20 */
		'l', 0, 0, '#', 0, '4', '5', '6',
		'<', 'y', 'x', 'c', 'v', 'b', 'n', 'm',	/* 30 */
		',', '.', '-', 0, '.', '7', '8', '9',
		' ', 0x08, '\t', '\r', '\r', 0x1b, 0x7f, 0,	/* 40 */
		0, 0, '-',
	},
	{
		'~', '!', '"', 0, '$', '%', '&', '/',
		'(', ')', '=', '?', '`', '|', 0, '0',
		'Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I',
		'O', 'P', 0, '*', 0, '1', '2', '3',
		'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K',
		'L', 0, 0, '^', 0, '4', '5', '6',
		'>', 'Y', 'X', 'C', 'V', 'B', 'N', 'M',
		';', ':', '_', 0, '.', '7', '8', '9',
		' ', 0x08, '\t', '\r', '\r', 0x1b, 0x7f, 0,
		0, 0, '-',
	},
	},
};

/* German map, right Alt held: raw code -> character */
static const struct {
	u8 code;
	char c;
} altgr_de[] = {
	{ 0x0b, '\\' }, { 0x10, '@' }, { 0x07, '{' }, { 0x08, '[' },
	{ 0x09, ']' }, { 0x0a, '}' }, { 0x30, '|' }, { 0x1b, '~' },
};

static int map, pc, shift_l, shift_r, ctrl, caps, ralt;
static char queue[4];
static int qhead, qlen;

static void
push(char c)
{
	if (qlen < (int)sizeof(queue))
		queue[(qhead + qlen++) % sizeof(queue)] = c;
}

/* Wait at least three raster lines (>= 128 us) using the beam counter. */
static void
delay_lines(int n)
{
	u8 v = CIA(VHPOSR);

	while (n > 0) {
		u8 now = CIA(VHPOSR);

		if (now != v) {
			v = now;
			n--;
		}
	}
}

void
kbd_init(int keymap)
{
	map = keymap == KBD_DE || keymap == KBD_DE_PC;
	pc = keymap == KBD_DE_PC;
	CIA(CIAA_CRA) &= (u8)~CRA_SPMODE;	/* serial port: input */
	(void)CIA(CIAA_ICR);			/* clear pending flags */
	qhead = qlen = 0;
}

static void
key(u8 code, int up)
{
	int shift;
	char c;

	switch (code) {
	case K_LSHIFT:
		shift_l = !up;
		return;
	case K_RSHIFT:
		shift_r = !up;
		return;
	case K_CTRL:
		ctrl = !up;
		return;
	case K_CAPS:
		caps = !up;	/* the key reports down/up as the lock toggles */
		return;
	case K_RALT:
		ralt = !up;
		return;
	}
	if (up)
		return;
	if (code >= K_UP && code <= K_LEFT) {
		push(0x1b);
		push('[');
		push("ABCD"[code - K_UP]);
		return;
	}
	if (code >= sizeof(keymap[0][0]))
		return;
	if (map && ralt) {
		unsigned i;

		for (i = 0; i < sizeof(altgr_de) / sizeof(altgr_de[0]); i++)
			if (altgr_de[i].code == code)
				push(altgr_de[i].c);
		return;
	}
	shift = shift_l || shift_r;
	c = keymap[map][shift][code];
	if (pc && code == 0x00)
		c = shift ? 0 : '^';
	else if (pc && code == 0x0d)
		c = shift ? '\'' : '#';
	if (caps && c >= 'a' && c <= 'z')
		c = keymap[map][1][code];
	else if (caps && c >= 'A' && c <= 'Z')
		c = keymap[map][0][code];
	if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
		c = (char)(c & 0x1f);
	if (c)
		push(c);
}

int
kbd_getc(void)
{
	if (!qlen && (CIA(CIAA_ICR) & ICR_SP)) {
		u8 b = (u8)~CIA(CIAA_SDR);

		/* handshake: hold KDAT low, then release */
		CIA(CIAA_CRA) |= CRA_SPMODE;
		delay_lines(3);
		CIA(CIAA_CRA) &= (u8)~CRA_SPMODE;

		b = (u8)(b >> 1 | b << 7);
		if ((b & 0x7f) < 0x78)		/* 0x78+: reset warning, sync */
			key(b & 0x7f, b & 0x80);
	}
	if (qlen) {
		char c = queue[qhead];

		qhead = (qhead + 1) % (int)sizeof(queue);
		qlen--;
		return (u8)c;
	}
	return -1;
}
