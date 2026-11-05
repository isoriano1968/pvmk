/*
 * net <a.b.c.d>: the Z3660's network path without AMIX -- a small IPv4
 * stack on drv/z3660eth.c, polled, until a key is pressed:
 *
 *   ARP       answers for its address (and announces it at the start)
 *   ICMP      echo: ping it
 *   UDP 7     echo
 *   UDP 9     discard; the first four bytes, if any, are a sequence number,
 *             so the datagrams lost on the way in are counted
 *   UDP 5001  control: "SEND <n> <size> <port>" sends n datagrams of size
 *             bytes (sequence numbers first) to the asker's port, then
 *             "DONE <n> <ms>"; "STAT" answers with the counters
 *
 * Every second a line: frames in and out, KB/s each way, UDP 9 datagrams
 * and the ones lost, the firmware serials skipped (frames the board's ring
 * lost before pvmk saw them).  host/nettest.py is the PC's side.
 *
 * net -i <a.b.c.d>: the same, driven by the board's INT6 instead of polling
 * (the test for AMIX's interrupt receive): the ring is drained when the
 * interrupt says so, and a slow poll (every 100 ms) counts what it finds as
 * "late" (an interrupt missed).  The line adds the interrupts, the spurious
 * ones (INT6 not from the board) and frames per interrupt.  More than 5000
 * spurious ones in a second is a storm: interrupts off, back to polling.
 */
#include "pvmk/kprintf.h"
#include "pvmk/log.h"
#include "pvmk/net.h"

#define ETH_ARP		0x0806
#define ETH_IP		0x0800
#define IP_ICMP		1
#define IP_UDP		17

static u8 rxb[ETH_MAXLEN + 2] __attribute__((aligned(4)));
static u8 txb[ETH_MAXLEN + 2] __attribute__((aligned(4)));
static u32 my_ip;
static u16 ip_id;

static u32 udp9_n, udp9_lost, udp9_next;
static int udp9_seq;

static u16 g16(const u8 *p) { return p[0] << 8 | p[1]; }
static u32 g32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | p[2] << 8 | p[3]; }
static void p16(u8 *p, u16 v) { p[0] = v >> 8; p[1] = v; }
static void p32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void
mcopy(u8 *d, const u8 *s, int n)
{
	while (n-- > 0)
		*d++ = *s++;
}

static u16
csum(const u8 *p, int n, u32 sum)
{
	for (; n > 1; p += 2, n -= 2)
		sum += p[0] << 8 | p[1];
	if (n)
		sum += p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

static void
print_ip(u32 a)
{
	kprintf("%u.%u.%u.%u", a >> 24, a >> 16 & 255, a >> 8 & 255, a & 255);
}

static int
parse_ip(const char *s, u32 *out)
{
	u32 v = 0, part, k;

	for (k = 0; k < 4; k++) {
		if (*s < '0' || *s > '9')
			return 0;
		for (part = 0; *s >= '0' && *s <= '9'; s++)
			part = part * 10 + *s - '0';
		if (part > 255 || (k < 3 && *s++ != '.'))
			return 0;
		v = v << 8 | part;
	}
	*out = v;
	return *s == 0;
}

static void
eth_hdr(u8 *f, const u8 *dst, u16 type)
{
	mcopy(f, dst, ETH_ALEN);
	mcopy(f + 6, eth_mac, ETH_ALEN);
	p16(f + 12, type);
}

static void
arp_send(u16 op, const u8 *tha, u32 tip)
{
	static const u8 bcast[ETH_ALEN] = { 255, 255, 255, 255, 255, 255 };
	u8 *a = txb + ETH_HLEN;

	eth_hdr(txb, op == 1 ? bcast : tha, ETH_ARP);
	p16(a, 1);			/* ethernet */
	p16(a + 2, ETH_IP);
	a[4] = 6;
	a[5] = 4;
	p16(a + 6, op);
	mcopy(a + 8, eth_mac, ETH_ALEN);
	p32(a + 14, my_ip);
	mcopy(a + 18, tha, ETH_ALEN);
	p32(a + 24, tip);
	z3660eth_send(txb, ETH_HLEN + 28);
}

/* an IPv4 datagram of len payload bytes, already at txb + 34, to mac/ip */
static void
ip_send(const u8 *mac, u32 dst, u8 proto, int len)
{
	u8 *ip = txb + ETH_HLEN;

	eth_hdr(txb, mac, ETH_IP);
	ip[0] = 0x45;
	ip[1] = 0;
	p16(ip + 2, 20 + len);
	p16(ip + 4, ip_id++);
	p16(ip + 6, 0);
	ip[8] = 64;
	ip[9] = proto;
	p16(ip + 10, 0);
	p32(ip + 12, my_ip);
	p32(ip + 16, dst);
	p16(ip + 10, csum(ip, 20, 0));
	z3660eth_send(txb, ETH_HLEN + 20 + len);
}

/* UDP: payload already at txb + 42; no checksum (0 is allowed in IPv4) */
static void
udp_send(const u8 *mac, u32 dst, u16 sport, u16 dport, int len)
{
	u8 *u = txb + ETH_HLEN + 20;

	p16(u, sport);
	p16(u + 2, dport);
	p16(u + 4, 8 + len);
	p16(u + 6, 0);
	ip_send(mac, dst, IP_UDP, 8 + len);
}

static int
put_text(u8 *d, const char *s)
{
	int n = 0;

	while (s[n])
		d[n] = s[n], n++;
	return n;
}

static int
put_num(u8 *d, u32 v)
{
	char b[12];
	int n = 0, k = 0;

	do
		b[n++] = '0' + v % 10;
	while (v /= 10);
	while (n)
		d[k++] = b[--n];
	return k;
}

static u32
get_num(const u8 **p, const u8 *end)
{
	u32 v = 0;

	while (*p < end && **p == ' ')
		(*p)++;
	while (*p < end && **p >= '0' && **p <= '9')
		v = v * 10 + *(*p)++ - '0';
	return v;
}

static u32
now_ms(void)
{
	u32 s, ms;

	time_now(&s, &ms);
	return s * 1000 + ms;
}

/* "SEND n size port": n datagrams to the asker, then "DONE n ms" */
static void
udp_blast(const u8 *mac, u32 ip, u16 sport, const u8 *arg, const u8 *end)
{
	u32 n = get_num(&arg, end), size = get_num(&arg, end), port = get_num(&arg, end);
	u32 i, t0 = now_ms(), t;
	u8 *d = txb + ETH_HLEN + 28;
	int k;

	if (size < 4)
		size = 4;
	if (size > ETH_MAXLEN - ETH_HLEN - 28 - 4)
		size = ETH_MAXLEN - ETH_HLEN - 28 - 4;
	if (!port)
		port = sport;
	for (i = 0; i < size; i++)
		d[i] = (u8)i;
	for (i = 0; i < n; i++) {
		p32(d, i);
		udp_send(mac, ip, 5001, port, size);
	}
	t = now_ms() - t0;
	k = put_text(d, "DONE ");
	k += put_num(d + k, n);
	d[k++] = ' ';
	k += put_num(d + k, t);
	udp_send(mac, ip, 5001, sport, k);
	kprintf("\nnet: sent %u datagrams of %u bytes to ", n, size);
	print_ip(ip);
	kprintf(" in %u ms\n", t);
}

static void
udp_in(const u8 *f, const u8 *ip, int ihl, int iplen)
{
	const u8 *u = ip + ihl, *data = u + 8, *end;
	u16 sport = g16(u), dport = g16(u + 2);
	int len = g16(u + 4) - 8;
	u8 *d = txb + ETH_HLEN + 28;
	int k;

	if (len < 0 || 8 + len > iplen - ihl)
		return;
	end = data + len;
	if (dport == 9) {
		udp9_n++;
		if (len >= 4) {
			u32 seq = g32(data);

			if (!udp9_seq || seq < udp9_next)
				udp9_seq = 1;		/* a new run */
			else if (seq > udp9_next)
				udp9_lost += seq - udp9_next;
			udp9_next = seq + 1;
		}
	} else if (dport == 7) {
		mcopy(d, data, len);
		udp_send(f + 6, g32(ip + 12), 7, sport, len);
	} else if (dport == 5001) {
		if (len >= 4 && data[0] == 'S' && data[1] == 'E' && data[2] == 'N' && data[3] == 'D')
			udp_blast(f + 6, g32(ip + 12), sport, data + 4, end);
		else if (len >= 4 && data[0] == 'S' && data[1] == 'T') {
			k = put_text(d, "RX ");
			k += put_num(d + k, eth_stats.rx);
			k += put_text(d + k, " UDP9 ");
			k += put_num(d + k, udp9_n);
			k += put_text(d + k, " LOST ");
			k += put_num(d + k, udp9_lost);
			k += put_text(d + k, " SKIPPED ");
			k += put_num(d + k, eth_stats.rx_skipped);
			udp_send(f + 6, g32(ip + 12), 5001, sport, k);
		}
	}
}

static void
frame_in(int n)
{
	const u8 *f = rxb, *a, *ip;
	int ihl, iplen;

	if (n < ETH_HLEN)
		return;
	if (g16(f + 12) == ETH_ARP && n >= ETH_HLEN + 28) {
		a = f + ETH_HLEN;
		if (g16(a + 6) == 1 && g32(a + 24) == my_ip)
			arp_send(2, a + 8, g32(a + 14));
		return;
	}
	if (g16(f + 12) != ETH_IP || n < ETH_HLEN + 20)
		return;
	ip = f + ETH_HLEN;
	ihl = (ip[0] & 15) * 4;
	iplen = g16(ip + 2);
	if ((ip[0] >> 4) != 4 || ihl < 20 || iplen > n - ETH_HLEN || g32(ip + 16) != my_ip ||
	    (g16(ip + 6) & 0x3fff))		/* fragments: not here */
		return;
	if (ip[9] == IP_ICMP && iplen - ihl >= 8 && ip[ihl] == 8) {
		u8 *d = txb + ETH_HLEN + 20;

		mcopy(d, ip + ihl, iplen - ihl);
		d[0] = 0;			/* echo reply */
		p16(d + 2, 0);
		p16(d + 2, csum(d, iplen - ihl, 0));
		ip_send(f + 6, g32(ip + 12), IP_ICMP, iplen - ihl);
	} else if (ip[9] == IP_UDP && iplen - ihl >= 8)
		udp_in(f, ip, ihl, iplen);
}

/* drain the ring; the frames it held */
static u32
drain(void)
{
	u32 k = 0;
	int n;

	while ((n = z3660eth_recv(rxb, ETH_MAXLEN)) > 0) {
		frame_in(n);
		k++;
	}
	return k;
}

void
cmd_net(int argc, char **argv)
{
	static const u8 zero[ETH_ALEN];
	struct eth_stats last;
	u32 t0, t1, tp, sec = 0, last_u9 = 0, late = 0, irqf = 0, last_irq = 0, last_sp = 0;
	int irq = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'i';

	if (irq) {
		argc--;
		argv++;
	}
	if (argc < 2 || !parse_ip(argv[1], &my_ip)) {
		kprintf("usage: net [-i] <a.b.c.d>   (pvmk's address; -i: INT6; any key stops)\n");
		return;
	}
	if (z3660eth_init() < 0) {
		kprintf("net: no Z3660 ethernet\n");
		return;
	}
	kprintf("net: %02x:%02x:%02x:%02x:%02x:%02x as ", eth_mac[0], eth_mac[1],
	    eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);
	print_ip(my_ip);
	kprintf(": ping it, UDP 7 echo, 9 discard, 5001 control; %sany key stops\n",
	    irq ? "INT6 driven; " : "");
	eth_stats = (struct eth_stats){ 0 };
	udp9_n = udp9_lost = 0;
	udp9_seq = 0;
	arp_send(1, zero, my_ip);		/* gratuitous: here I am */
	last = eth_stats;
	t0 = tp = now_ms();
	if (irq)
		z3660eth_irq(1);
	for (;;) {
		if (!irq)
			drain();
		else if (eth_irq_pending) {
			eth_irq_pending = 0;
			irqf += drain();
			z3660eth_irq_rearm();
		}
		t1 = now_ms();
		if (irq && t1 - tp >= 100) {	/* the slow poll: what an interrupt missed */
			late += drain();
			tp = t1;
		}
		if (t1 - t0 >= 1000) {
			u32 dt = t1 - t0, ni = eth_irq_n - last_irq, sp = eth_irq_spurious - last_sp;

			sec++;
			kprintf("%4us in %5u %4u KB/s  out %5u %4u KB/s  udp9 %u (lost %u)  skipped %u",
			    sec, eth_stats.rx - last.rx,
			    (eth_stats.rx_bytes - last.rx_bytes) / dt,
			    eth_stats.tx - last.tx,
			    (eth_stats.tx_bytes - last.tx_bytes) / dt,
			    udp9_n - last_u9, udp9_lost, eth_stats.rx_skipped);
			if (irq)
				kprintf("  irq %u spurious %u late %u", ni, sp, late);
			kprintf("\n");
			if (irq && sp > 5000) {
				z3660eth_irq(0);
				irq = 0;
				kprintf("net: %u spurious INT6 in a second: a storm; interrupts off, polling\n", sp);
			}
			last = eth_stats;
			last_u9 = udp9_n;
			last_irq = eth_irq_n;
			last_sp = eth_irq_spurious;
			t0 = t1;
		}
		if (net_getc() >= 0)
			break;
	}
	if (irq) {
		z3660eth_irq(0);
		kprintf("net: INT6 %u, spurious %u, frames by interrupt %u (%u.%u a time), late %u\n",
		    eth_irq_n, eth_irq_spurious, irqf, eth_irq_n ? irqf / eth_irq_n : 0,
		    eth_irq_n ? irqf * 10 / eth_irq_n % 10 : 0, late);
	}
	kprintf("net: in %u frames (%u bad), out %u (%u no link, %u errors), "
	    "udp9 %u (lost %u), skipped %u\n", eth_stats.rx, eth_stats.rx_bad,
	    eth_stats.tx, eth_stats.tx_nolink, eth_stats.tx_err, udp9_n, udp9_lost,
	    eth_stats.rx_skipped);
}
