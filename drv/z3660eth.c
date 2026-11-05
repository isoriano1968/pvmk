/*
 * The Z3660's ethernet, polled (the firmware's registers in the combo
 * board's RTG page; the same interface as AMIX's z3660eth driver, from
 * which the register map and the ZS-05 rule are taken):
 *
 *   0x104 CONFIG    W 0: no ethernet interrupt (pvmk polls)
 *   0x190 ETH_TX    W length: send the frame at board + 0x07EE0000
 *                   (synchronous); R: 0 sent, 1 no link, else an error
 *   0x194 ETH_RX    W: free the current receive slot
 *   0x198 MAC_HI    R: MAC bytes 0-1;  0x19C MAC_LO: bytes 2-5
 *   0x1A4 RX_ADDR   R: board offset of the current receive slot, in the
 *                   ring at 0x07ED0000 (2 KB slots: length, serial, frame)
 *
 * ZS-05: the FPGA delivers a board access at (previous address AND this
 * one) when the address rises between back-to-back cycles.  So each access
 * follows a read of an offset with every bit it has, in one asm block with
 * interrupts masked: CONFIG after RX_ADDR, ETH_RX after MAC_LO, ETH_TX after
 * itself, and reads after themselves.
 *
 * The data cache is off in pvmk: no maintenance around the frames.
 */
#include "pvmk/amiga.h"
#include "pvmk/kprintf.h"
#include "pvmk/net.h"
#include "pvmk/z3660.h"

#define ZZ_CONFIG	0x104
#define ZZ_ETH_TX	0x190
#define ZZ_ETH_RX	0x194
#define ZZ_ETH_MAC_HI	0x198
#define ZZ_ETH_MAC_LO	0x19C
#define ZZ_ETH_RX_ADDR	0x1A4
#define ZZ_INT_STATUS	0x1A8		/* R: pending, ETH = bit 0 */

#define RX_RING		0x07ED0000
#define RX_SLOTS	64		/* the firmware's backlog range */
#define SLOT_SIZE	2048
#define TX_FRAME	0x07EE0000

struct eth_stats eth_stats;
u8 eth_mac[ETH_ALEN];

static u32 board;
static u16 last_serial;
static int first;			/* no frame read yet: no gap to count */

/* n bytes; long words when both ends allow it (the board is 32 bits wide) */
static void
copy(volatile u8 *d, const volatile u8 *s, int n)
{
	int i = 0;

	if (!(((uintptr_t)d | (uintptr_t)s) & 3))
		for (; i + 4 <= n; i += 4)
			*(volatile u32 *)(d + i) = *(const volatile u32 *)(s + i);
	for (; i < n; i++)
		d[i] = s[i];
}

/* read arm, write off */
static inline void
wr_armed(u32 arm, u32 off, u32 v)
{
	u32 sr, t;

	__asm__ __volatile__("move.w %%sr,%0\n\tor.w #0x700,%%sr\n\t"
	    "move.l (%2),%1\n\tmove.l %4,(%3)\n\tmove.w %0,%%sr"
	    : "=&d" (sr), "=&d" (t)
	    : "a" (board + arm), "a" (board + off), "d" (v) : "memory");
}

/* read off, write off, nop, read off: the send and its result */
static inline u32
wr_result(u32 off, u32 v)
{
	u32 sr, t;

	__asm__ __volatile__("move.w %%sr,%0\n\tor.w #0x700,%%sr\n\t"
	    "move.l (%2),%1\n\tmove.l %3,(%2)\n\tnop\n\tmove.l (%2),%1\n\t"
	    "move.w %0,%%sr"
	    : "=&d" (sr), "=&d" (t)
	    : "a" (board + off), "d" (v) : "memory");
	return t;
}

/* read off twice, the second one counts */
static inline u32
rd(u32 off)
{
	u32 sr, t;

	__asm__ __volatile__("move.w %%sr,%0\n\tor.w #0x700,%%sr\n\t"
	    "move.l (%2),%1\n\tmove.l (%2),%1\n\tmove.w %0,%%sr"
	    : "=&d" (sr), "=&d" (t)
	    : "a" (board + off) : "memory");
	return t;
}

int
z3660eth_init(void)
{
	u32 hi, lo;

	if (z3660_init() < 0 || !(board = z3660_board()))
		return -1;
	hi = rd(ZZ_ETH_MAC_HI);
	lo = rd(ZZ_ETH_MAC_LO);
	if (hi == 0xffffffff || hi == 0 || (hi & 0xffff0000)) {
		kprintf("z3660eth: no answer (MAC %08x %08x)\n", hi, lo);
		return -1;
	}
	eth_mac[0] = hi >> 8;
	eth_mac[1] = hi;
	eth_mac[2] = lo >> 24;
	eth_mac[3] = lo >> 16;
	eth_mac[4] = lo >> 8;
	eth_mac[5] = lo;
	wr_armed(ZZ_ETH_RX_ADDR, ZZ_CONFIG, 0);	/* no interrupt: polled */
	/* the slot at the cursor is the next one to read: the cursor moves on
	   only when it is freed.  Its serial says nothing about whether it was
	   read; as AMIX's driver, start below the firmware's first, 1 (after a
	   reset without a power cycle the first frame may be an old one) */
	last_serial = 0;
	first = 1;
	return 0;
}

int
z3660eth_recv(u8 *buf, int max)
{
	volatile u8 *s;
	u32 off;
	u16 serial;
	int len;

	off = rd(ZZ_ETH_RX_ADDR);
	if (off < RX_RING || off >= RX_RING + RX_SLOTS * SLOT_SIZE)
		return 0;
	s = (volatile u8 *)(uintptr_t)(board + off);
	serial = s[2] << 8 | s[3];
	if (serial == last_serial)
		return 0;			/* nothing new */
	if (!first && (u16)(serial - last_serial) != 1)
		eth_stats.rx_skipped += (u16)(serial - last_serial) - 1;
	first = 0;
	last_serial = serial;
	len = s[0] << 8 | s[1];
	if (len < ETH_HLEN || len > ETH_MAXLEN) {
		eth_stats.rx_bad++;
		len = 0;
	} else {
		if (len > max)
			len = max;
		copy(buf, s + 4, len);
		eth_stats.rx++;
		eth_stats.rx_bytes += len;
	}
	wr_armed(ZZ_ETH_MAC_LO, ZZ_ETH_RX, 1);	/* free the slot */
	return len;
}

int
z3660eth_send(const u8 *buf, int len)
{
	volatile u8 *t = (volatile u8 *)(uintptr_t)(board + TX_FRAME);
	u32 rc;
	int i;

	if (len > ETH_MAXLEN)
		return -1;
	copy(t, buf, len);
	for (i = len; i < ETH_MINLEN; i++)
		t[i] = 0;
	if (len < ETH_MINLEN)
		len = ETH_MINLEN;
	rc = wr_result(ZZ_ETH_TX, len);
	if (rc == 0) {
		eth_stats.tx++;
		eth_stats.tx_bytes += len;
	} else if (rc == 1)
		eth_stats.tx_nolink++;
	else
		eth_stats.tx_err++;
	return rc == 0 ? 0 : rc == 1 ? 1 : -1;
}

/* ---- interrupt mode ---------------------------------------------------- */

volatile u32 eth_irq_n, eth_irq_spurious, eth_irq_pending;

/* level 6: CIA-B and the expansion bus share it; ours when INT_STATUS says
   ETH.  Acknowledge and mask (CONFIG 0, armed by RX_ADDR), leave the drain
   to the main loop, clear Paula's EXTER request after the source. */
static void
z3660eth_isr(void)
{
	u32 st = rd(ZZ_INT_STATUS);

	if (st != 0xffffffff && (st & 1)) {
		wr_armed(ZZ_ETH_RX_ADDR, ZZ_CONFIG, 0);
		eth_irq_pending = 1;
		eth_irq_n++;
	} else
		eth_irq_spurious++;
	(void)*(volatile u8 *)CIA_B_ICR;		/* CIA-B's, if it was that */
	REG16(CUSTOM_INTREQ) = INT_EXTER;
}

void
z3660eth_irq_rearm(void)
{
	wr_armed(ZZ_ETH_RX_ADDR, ZZ_CONFIG, 1);
}

void
z3660eth_irq(int on)
{
	if (on) {
		*(volatile u8 *)CIA_B_ICR = 0x7f;	/* no CIA-B interrupt */
		(void)*(volatile u8 *)CIA_B_ICR;
		eth_irq_n = eth_irq_spurious = eth_irq_pending = 0;
		irq6_hook = z3660eth_isr;
		REG16(CUSTOM_INTREQ) = INT_EXTER;
		REG16(CUSTOM_INTENA) = INT_SETCLR | INT_INTEN | INT_EXTER;
		z3660eth_irq_rearm();
		__asm__ __volatile__("move.w #0x2500,%%sr" ::: "memory");
	} else {
		__asm__ __volatile__("move.w #0x2700,%%sr" ::: "memory");
		wr_armed(ZZ_ETH_RX_ADDR, ZZ_CONFIG, 0);
		REG16(CUSTOM_INTENA) = INT_INTEN | INT_EXTER;	/* off again */
		REG16(CUSTOM_INTREQ) = INT_EXTER;
		irq6_hook = 0;
	}
}
