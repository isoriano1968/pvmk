/*
 * GDB remote serial protocol stub (target: the stopped guest).
 *
 * Supported: ?, g/G, p/P (D0-D7, A0-A7, ps = virtual SR, pc), m/M (memory
 * through a fault-safe copy), Z0/z0 (software breakpoints with ILLEGAL,
 * told apart from a guest's own ILLEGAL by address; GDB's own m68k
 * breakpoint, TRAP #15, is our hypercall), c, s (trace bit;
 * an emulated instruction is one step too), D (detach: the guest runs on),
 * k (kill), Ctrl-C while running, qSupported, QStartNoAckMode,
 * qXfer:features:read (target description: core registers only), and the
 * single-thread queries. Guest console output becomes O packets.
 */
#include "pvmk/amiga.h"
#include "pvmk/gdb.h"
#include "pvmk/kprintf.h"
#include "pvmk/log.h"
#include "pvmk/mk.h"
#include "pvmk/paula.h"
#include "pvmk/vcpu.h"
#include "pvmk/vmmu.h"

#define PKTMAX		1024
#define MAXBP		16
#define BREAK_INSN	0x4afc		/* ILLEGAL: vector 4 on every 68k */
#define NREGS		18		/* d0-d7, a0-a7, ps, pc */

int gdb_active, gdb_step;

static int noack;
static char in[PKTMAX + 4];
static char out[PKTMAX + 4];

static struct {
	u32 addr;
	u16 orig;
	u8 used;
} bps[MAXBP];

static const char target_xml[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><architecture>m68k:68030</architecture>"
    "<feature name=\"org.gnu.gdb.m68k.core\">"
    "<reg name=\"d0\" bitsize=\"32\"/><reg name=\"d1\" bitsize=\"32\"/>"
    "<reg name=\"d2\" bitsize=\"32\"/><reg name=\"d3\" bitsize=\"32\"/>"
    "<reg name=\"d4\" bitsize=\"32\"/><reg name=\"d5\" bitsize=\"32\"/>"
    "<reg name=\"d6\" bitsize=\"32\"/><reg name=\"d7\" bitsize=\"32\"/>"
    "<reg name=\"a0\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"a1\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"a2\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"a3\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"a4\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"a5\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"fp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"ps\" bitsize=\"32\"/>"
    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "</feature></target>";

/* ------------------------------------------------------------------ */
/* Packets                                                             */

static const char hexd[] = "0123456789abcdef";

static int
hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int
getc_wait(void)
{
	int c;
	u32 s, ms;

	while ((c = paula_getc()) < 0)
		time_now(&s, &ms);
	return c;
}

static void
send_packet(const char *data)
{
	u8 sum;
	const char *p;
	int tries;

	for (tries = 0; tries < 8; tries++) {
		sum = 0;
		paula_putc_raw('$');
		for (p = data; *p; p++) {
			paula_putc_raw(*p);
			sum += (u8)*p;
		}
		paula_putc_raw('#');
		paula_putc_raw(hexd[sum >> 4]);
		paula_putc_raw(hexd[sum & 15]);
		if (noack || getc_wait() == '+')
			return;
	}
}

/* Receive one packet into in[]; returns its length. */
static u32
recv_packet(void)
{
	for (;;) {
		u32 n = 0;
		u8 sum = 0;
		int c, h1, h2;

		while ((c = getc_wait()) != '$')
			;			/* acks, stray Ctrl-C */
		while ((c = getc_wait()) != '#') {
			if (c == '$') {		/* restart */
				n = 0;
				sum = 0;
				continue;
			}
			if (n < PKTMAX)
				in[n++] = (char)c;
			sum += (u8)c;
		}
		in[n] = 0;
		h1 = hexval(getc_wait());
		h2 = hexval(getc_wait());
		if (noack)
			return n;
		if (h1 >= 0 && h2 >= 0 && (u8)(h1 << 4 | h2) == sum) {
			paula_putc_raw('+');
			return n;
		}
		paula_putc_raw('-');
	}
}

static char *
put_hex32(char *o, u32 v)
{
	int i;

	for (i = 28; i >= 0; i -= 4)
		*o++ = hexd[(v >> i) & 15];
	return o;
}

/* Parse hex up to a non-hex character; *p is advanced. */
static u32
get_hex(const char **p)
{
	u32 v = 0;
	int d;

	while ((d = hexval(**p)) >= 0) {
		v = v << 4 | (u32)d;
		(*p)++;
	}
	return v;
}

/* ------------------------------------------------------------------ */
/* Guest state                                                         */

static u32
get_reg(u32 n)
{
	if (n < 8)
		return guest.ctx.d[n];
	if (n < 16)
		return guest.ctx.a[n - 8];
	if (n == 16)
		return (guest.vsr & 0xff00) | (guest.ctx.sr & 0x1f);
	return guest.ctx.pc;
}

static void
set_reg(u32 n, u32 v)
{
	if (n < 8)
		guest.ctx.d[n] = v;
	else if (n < 16)
		guest.ctx.a[n - 8] = v;
	else if (n == 16)
		vcpu_set_ps((u16)v);
	else if (n == 17)
		guest.ctx.pc = v;
}

static void
stop_reply(void)
{
	char *o = out;

	if (!guest.active) {			/* exited */
		*o++ = 'W';
		*o++ = hexd[(guest.exit_code >> 4) & 15];
		*o++ = hexd[guest.exit_code & 15];
		*o = 0;
	} else {
		u32 sig = guest.stop_sig ? guest.stop_sig : 5;

		*o++ = 'T';
		*o++ = hexd[sig >> 4];
		*o++ = hexd[sig & 15];
		if (guest.stop_swbreak) {
			memcpy(o, "swbreak:;", 9);
			o += 9;
		}
		*o = 0;
	}
	send_packet(out);
}

int
gdb_breakpoint_at(u32 addr)
{
	int i;

	for (i = 0; i < MAXBP; i++)
		if (bps[i].used && bps[i].addr == addr)
			return 1;
	return 0;
}

/*
 * Guest memory as GDB sees it: virtual addresses in the guest's current
 * view, translated through its own tables (the debugger may write to
 * write-protected text). Faults on unmapped or nonexistent memory are
 * errors, not crashes.
 */
static int
guest_copy(u32 va, void *buf, u32 n, int to_guest)
{
	int super = (guest.vsr & VSR_S) != 0;
	u8 *b = buf;
	u32 i, pa;

	for (i = 0; i < n; i++) {
		if (vmmu_translate(va + i, super, 0, &pa))
			return -1;
		if (to_guest ? safe_copy((void *)(uintptr_t)pa, b + i, 1) :
		    safe_copy(b + i, (const void *)(uintptr_t)pa, 1))
			return -1;
	}
	return 0;
}

static int
bp_insert(u32 addr)
{
	u16 bkpt = BREAK_INSN;
	int i, freei = -1;

	for (i = 0; i < MAXBP; i++) {
		if (bps[i].used && bps[i].addr == addr)
			return 0;
		if (!bps[i].used && freei < 0)
			freei = i;
	}
	if (freei < 0 || (addr & 1) ||
	    guest_copy(addr, &bps[freei].orig, 2, 0) ||
	    guest_copy(addr, &bkpt, 2, 1))
		return -1;
	bps[freei].addr = addr;
	bps[freei].used = 1;
	icache_inval(cpuinfo.model);
	return 0;
}

static int
bp_remove(u32 addr)
{
	int i;

	for (i = 0; i < MAXBP; i++)
		if (bps[i].used && bps[i].addr == addr) {
			guest_copy(addr, &bps[i].orig, 2, 1);
			bps[i].used = 0;
			icache_inval(cpuinfo.model);
			return 0;
		}
	return -1;
}

static void
bp_remove_all(void)
{
	int i;

	for (i = 0; i < MAXBP; i++)
		if (bps[i].used)
			bp_remove(bps[i].addr);
}

void
gdb_console(const u8 *buf, u32 len)
{
	char *o = out;
	u32 i;

	*o++ = 'O';
	for (i = 0; i < len && o < out + PKTMAX - 2; i++) {
		*o++ = hexd[buf[i] >> 4];
		*o++ = hexd[buf[i] & 15];
	}
	*o = 0;
	send_packet(out);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */

static void
leave(void)
{
	bp_remove_all();
	gdb_active = 0;
	gdb_step = 0;
	noack = 0;
	con_serial(1);
}

static void
read_memory(const char *p)
{
	static u8 buf[PKTMAX / 2];
	u32 addr = get_hex(&p), len, i;
	char *o = out;

	if (*p++ != ',') {
		send_packet("E01");
		return;
	}
	len = get_hex(&p);
	if (len > sizeof(buf) - 1)
		len = sizeof(buf) - 1;
	if (guest_copy(addr, buf, len, 0)) {
		send_packet("E03");
		return;
	}
	for (i = 0; i < len; i++) {
		*o++ = hexd[buf[i] >> 4];
		*o++ = hexd[buf[i] & 15];
	}
	*o = 0;
	send_packet(out);
}

static void
write_memory(const char *p)
{
	static u8 buf[PKTMAX / 2];
	u32 addr = get_hex(&p), len, i;

	if (*p++ != ',') {
		send_packet("E01");
		return;
	}
	len = get_hex(&p);
	if (*p++ != ':' || len > sizeof(buf)) {
		send_packet("E01");
		return;
	}
	for (i = 0; i < len; i++) {
		int h = hexval(p[0]), l = hexval(p[1]);

		if (h < 0 || l < 0) {
			send_packet("E01");
			return;
		}
		buf[i] = (u8)(h << 4 | l);
		p += 2;
	}
	if (guest_copy(addr, buf, len, 1)) {
		send_packet("E03");
		return;
	}
	icache_inval(cpuinfo.model);
	send_packet("OK");
}

static void
xfer_features(const char *p)
{
	/* qXfer:features:read:target.xml:OFF,LEN */
	const char *name = "target.xml:";
	u32 off, len, total = sizeof(target_xml) - 1, n;

	while (*name && *p == *name)
		p++, name++;
	if (*name) {
		send_packet("E00");
		return;
	}
	off = get_hex(&p);
	if (*p++ != ',') {
		send_packet("E01");
		return;
	}
	len = get_hex(&p);
	if (off >= total) {
		send_packet("l");
		return;
	}
	n = total - off;
	if (n > len)
		n = len;
	if (n > PKTMAX - 2)
		n = PKTMAX - 2;
	out[0] = off + n < total ? 'm' : 'l';
	memcpy(out + 1, target_xml + off, n);
	out[n + 1] = 0;
	send_packet(out);
}

static int
starts(const char *s, const char *prefix)
{
	while (*prefix)
		if (*s++ != *prefix++)
			return 0;
	return 1;
}

void
gdb_main(void)
{
	if (!gdb_active) {
		kprintf("gdb: serial port handed to GDB (detach to get it "
		    "back)\n");
		while (!(REG16(SERDATR) & (1 << SERDATR_TSRE)))
			;
		gdb_active = 1;
		con_serial(0);			/* console: screen and log only */
	} else
		stop_reply();			/* the guest stopped */

	for (;;) {
		const char *p;
		u32 n, i;

		recv_packet();
		p = in + 1;
		switch (in[0]) {
		case '?':
			stop_reply();
			break;
		case 'g': {
			char *o = out;

			for (i = 0; i < NREGS; i++)
				o = put_hex32(o, get_reg(i));
			*o = 0;
			send_packet(out);
			break;
		}
		case 'G':
			for (i = 0; i < NREGS && p[0]; i++) {
				u32 v = 0, k;

				for (k = 0; k < 8; k++)
					v = v << 4 | (u32)hexval(*p++);
				set_reg(i, v);
			}
			send_packet("OK");
			break;
		case 'p':
			n = get_hex(&p);
			if (n >= NREGS) {
				send_packet("E01");
				break;
			}
			*put_hex32(out, get_reg(n)) = 0;
			send_packet(out);
			break;
		case 'P':
			n = get_hex(&p);
			if (*p++ != '=' || n >= NREGS) {
				send_packet("E01");
				break;
			}
			set_reg(n, get_hex(&p));
			send_packet("OK");
			break;
		case 'm':
			read_memory(p);
			break;
		case 'M':
			write_memory(p);
			break;
		case 'Z':
		case 'z':
			if (p[0] != '0' || p[1] != ',') {
				send_packet("");	/* only software ones */
				break;
			}
			p += 2;
			n = get_hex(&p);
			send_packet((in[0] == 'Z' ? bp_insert(n) : bp_remove(n)) ?
			    "E01" : "OK");
			break;
		case 'c':
		case 's':
			if (*p)
				guest.ctx.pc = get_hex(&p);
			if (!guest.active) {
				stop_reply();
				break;
			}
			gdb_step = in[0] == 's';
			vcpu_resume();			/* comes back via gdb_main */
		case 'D':
			send_packet("OK");
			leave();
			kprintf("gdb: detached\n");
			if (guest.active)
				vcpu_resume();
			return;
		case 'k':
			guest.active = 0;
			leave();
			kprintf("gdb: guest killed\n");
			return;
		case 'q':
			if (starts(in, "qSupported"))
				send_packet("PacketSize=400;qXfer:features:read+;"
				    "QStartNoAckMode+;swbreak+");
			else if (starts(in, "qXfer:features:read:"))
				xfer_features(in + 20);
			else if (starts(in, "qAttached"))
				send_packet("1");
			else if (starts(in, "qC"))
				send_packet("QC1");
			else if (starts(in, "qfThreadInfo"))
				send_packet("m1");
			else if (starts(in, "qsThreadInfo"))
				send_packet("l");
			else
				send_packet("");
			break;
		case 'Q':
			if (starts(in, "QStartNoAckMode")) {
				send_packet("OK");
				noack = 1;
			} else
				send_packet("");
			break;
		case 'H':
			send_packet("OK");
			break;
		default:
			send_packet("");
			break;
		}
	}
}
