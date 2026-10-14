/*
 * Console output, timestamps and the warm-reset-surviving boot log.
 */
#include "pvmk/display.h"
#include "pvmk/log.h"
#include "pvmk/paula.h"

#define LOG_MAGIC	0x534c4f47	/* 'SLOG' */
#define LOG_VERSION	1
#define LOG_HDR_SIZE	256

struct logslot {
	u32 wpos;			/* total bytes written */
	u32 len;			/* bytes valid, <= slot_size */
};

struct loghdr {
	u32 magic;
	u32 version;
	u32 bootcount;
	u32 active;
	u32 slot_size;
	struct logslot slot[2];
	u32 sum;
};

static volatile struct loghdr *hdr;
static volatile u8 *slots;
static int prev_valid;
static int serial_on = 1;
static int at_bol = 1;
static int interactive;

/* CIA-B TOD: 24-bit counter of horizontal sync pulses. */
#define CIAB_TODHI	0xbfda00
#define CIAB_TODMID	0xbfd900
#define CIAB_TODLO	0xbfd800

static u32 tod_hz = 15625;
static u32 tod_last;
static int tod_started;
static u32 acc_sec, acc_ticks;	/* monotonic time since the first read */

void
time_init(u32 hsync_hz)
{
	tod_hz = hsync_hz;
}

static u32
tod_read(void)
{
	u32 hi = *(volatile u8 *)CIAB_TODHI;	/* latches until LO is read */
	u32 mid = *(volatile u8 *)CIAB_TODMID;
	u32 lo = *(volatile u8 *)CIAB_TODLO;

	return hi << 16 | mid << 8 | lo;
}

/*
 * Monotonic time from the 24-bit TOD counter. Forward steps (including a
 * wrap) are accumulated; a backward step means someone reset the counter
 * (Kickstart may, before takeover) and is ignored. Must be called at least
 * every ~9 minutes to see wraps; the shell's input loop does.
 */
void
time_now(u32 *sec, u32 *msec)
{
	u32 t = tod_read(), delta;

	delta = (t - tod_last) & 0xffffff;
	tod_last = t;
	if (!tod_started || delta >= 0x800000)
		delta = 0;
	tod_started = 1;
	acc_ticks += delta;
	acc_sec += acc_ticks / tod_hz;
	acc_ticks %= tod_hz;
	*sec = acc_sec;
	*msec = acc_ticks * 1000 / tod_hz;
}

static u32
hdr_sum(void)
{
	return hdr->magic + hdr->version + hdr->bootcount + hdr->active +
	    hdr->slot_size + hdr->slot[0].wpos + hdr->slot[0].len +
	    hdr->slot[1].wpos + hdr->slot[1].len + 0x5a5a5a5a;
}

static int
hdr_valid(u32 slot_size)
{
	return hdr->magic == LOG_MAGIC && hdr->version == LOG_VERSION &&
	    hdr->slot_size == slot_size && hdr->active < 2 &&
	    hdr->slot[0].len <= slot_size && hdr->slot[1].len <= slot_size &&
	    hdr->sum == hdr_sum();
}

int
log_init(u32 area, u32 size)
{
	u32 slot_size = (size - LOG_HDR_SIZE) / 2;

	hdr = (volatile struct loghdr *)area;
	slots = (volatile u8 *)area + LOG_HDR_SIZE;
	if (hdr_valid(slot_size)) {
		prev_valid = 1;
		hdr->active ^= 1;
		hdr->bootcount++;
	} else {
		prev_valid = 0;
		hdr->magic = LOG_MAGIC;
		hdr->version = LOG_VERSION;
		hdr->bootcount = 1;
		hdr->active = 0;
		hdr->slot_size = slot_size;
		hdr->slot[1].wpos = hdr->slot[1].len = 0;
	}
	hdr->slot[hdr->active].wpos = 0;
	hdr->slot[hdr->active].len = 0;
	hdr->sum = hdr_sum();
	at_bol = 1;
	return prev_valid;
}

int
log_prev_valid(void)
{
	return prev_valid;
}

u32
log_bootcount(void)
{
	return hdr ? hdr->bootcount : 0;
}

static void
log_store(int c)
{
	volatile struct logslot *s;

	if (!hdr)
		return;
	s = &hdr->slot[hdr->active];
	slots[hdr->active * hdr->slot_size + s->wpos % hdr->slot_size] = (u8)c;
	s->wpos++;
	if (s->len < hdr->slot_size)
		s->len++;
	hdr->sum = hdr_sum();
}

static void
raw_putc(int c)
{
	if (serial_on)
		paula_putc(c);
	display_putc(c);
	log_store(c);
}

static void
put_dec(u32 v, int width, int pad)
{
	char buf[10];
	int i = 0;

	do {
		buf[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (i < width--)
		raw_putc(pad);
	while (i)
		raw_putc(buf[--i]);
}

int
con_interactive(int on)
{
	int old = interactive;

	interactive = on;
	at_bol = 1;
	return old;
}

void
con_putc(int c)
{
	if (interactive) {
		paula_putc(c);
		display_putc(c);
		return;
	}
	if (at_bol && c != '\n' && hdr) {
		u32 sec, ms;

		time_now(&sec, &ms);
		raw_putc('[');
		put_dec(sec, 4, ' ');
		raw_putc('.');
		put_dec(ms, 3, '0');
		raw_putc(']');
		raw_putc(' ');
	}
	at_bol = c == '\n';
	raw_putc(c);
}

void
con_serial(int on)
{
	serial_on = on;
}

void
log_dump(int prev, void (*out)(int c))
{
	u32 slot, i, start;
	volatile struct logslot *s;

	if (!hdr || (prev && !prev_valid)) {
		const char *m = "(no log)\n";

		while (*m)
			out(*m++);
		return;
	}
	slot = prev ? hdr->active ^ 1 : hdr->active;
	s = &hdr->slot[slot];
	start = s->wpos - s->len;
	for (i = 0; i < s->len; i++)
		out(slots[slot * hdr->slot_size + (start + i) % hdr->slot_size]);
}
