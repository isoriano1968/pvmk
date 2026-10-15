/*
 * Paula serial port, polled.
 */
#include "pvmk/amiga.h"
#include "pvmk/paula.h"

void
paula_init(u32 colorclock_hz, u32 baud)
{
	REG16(SERPER) = (u16)((colorclock_hz + baud / 2) / baud - 1);
}

static void
paula_tx(int c)
{
	while (!(REG16(SERDATR) & (1 << SERDATR_TBE)))
		;
	REG16(SERDAT) = (u16)((c & 0xff) | 0x100);
}

void
paula_putc_raw(int c)
{
	paula_tx(c);
}

void
paula_putc(int c)
{
	if (c == '\n')
		paula_tx('\r');
	paula_tx(c);
}

int
paula_getc(void)
{
	u16 v = REG16(SERDATR);

	if (!(v & (1 << SERDATR_RBF)))
		return -1;
	REG16(CUSTOM_BASE + 0x9c) = 0x0800;	/* INTREQ: clear RBF */
	return v & 0xff;
}
