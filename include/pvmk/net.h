/*
 * The Z3660's ethernet (drv/z3660eth.c) and a small IPv4 stack on it
 * (svc/net.c): ARP, ICMP echo, UDP echo and discard, and a UDP sender, to
 * test the board's network path before AMIX runs on it.
 */
#ifndef PVMK_NET_H
#define PVMK_NET_H

#include "pvmk/types.h"

#define ETH_ALEN	6
#define ETH_HLEN	14
#define ETH_MINLEN	60		/* without FCS: the driver pads to it */
#define ETH_MAXLEN	1518

struct eth_stats {
	u32 rx, tx;			/* frames */
	u32 rx_bytes, tx_bytes;
	u32 rx_bad;			/* length out of range */
	u32 rx_skipped;			/* firmware serials missed */
	u32 tx_nolink, tx_err;
};

extern struct eth_stats eth_stats;
extern u8 eth_mac[ETH_ALEN];

/* 0 when the Z3660's ethernet answers (MAC read), -1 if absent */
int z3660eth_init(void);
/* a received frame into buf (up to max bytes); its length, or 0 if none */
int z3660eth_recv(u8 *buf, int max);
/* send a frame; 0 sent, 1 no link, -1 error */
int z3660eth_send(const u8 *buf, int len);

/* svc/net.c: the shell's net command */
void cmd_net(int argc, char **argv);
/* svc/shell.c: a key from serial or keyboard, or -1 (does not wait) */
int net_getc(void);

#endif /* PVMK_NET_H */
