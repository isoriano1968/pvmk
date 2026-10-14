/*
 * A3000 internal SCSI: WD33C93A behind the SDMAC.
 *
 * Polled (the microkernel runs with IPL 7), data by SDMAC DMA through a
 * bounce buffer in chip RAM, which the SDMAC can always reach; the data
 * cache is off, so no cache maintenance is needed. Each command is one
 * "select with ATN and transfer". If the target leaves the data phase
 * early (short INQUIRY data, for example), the chip stops with "unexpected
 * status phase" and the command is resumed at the status phase.
 */
#include "pvmk/kprintf.h"
#include "pvmk/log.h"
#include "pvmk/scsi.h"

/* SDMAC (A3000) */
#define SDMAC		0xdd0000
#define DAWR		0x02
#define CNTR		0x0a
#define ACR		0x0c		/* DMA address, 32 bits */
#define ST_DMA		0x12
#define FLUSH		0x16
#define CINT		0x1a
#define ISTR		0x1e
#define SP_DMA		0x3e
#define SASR		0x41		/* WD33C93A address / aux status */
#define SCMD		0x43		/* WD33C93A register data */
#define CNTR_PDMD	(1 << 3)	/* peripheral is the SCSI chip */
#define CNTR_DDIR	(1 << 1)	/* DMA from memory to SCSI */
#define ISTR_FE		(1 << 0)	/* FIFO empty (flush done) */

/* WD33C93A registers */
#define R_OWNID		0x00
#define R_CONTROL	0x01
#define R_TIMEOUT	0x02
#define R_CDB		0x03
#define R_LUN		0x0f		/* target LUN, then target status */
#define R_PHASE		0x10
#define R_SYNC		0x11
#define R_COUNT		0x12		/* 24 bits, 0x12-0x14 */
#define R_DESTID	0x15
#define R_SRCID		0x16
#define R_STATUS	0x17
#define R_CMD		0x18
#define R_DATA		0x19

#define OWNID_EAF	0x08		/* enable advanced features */
#define CON_DMA		0x80		/* DMA mode: WD bus DMA via the SDMAC */
#define CON_EDI		0x08		/* ending disconnect interrupt */
#define CON_IDI		0x04		/* intermediate disconnect interrupt */
#define DESTID_DPD	0x40		/* data phase direction: in */

#define ASR_INT		0x80
#define ASR_BSY		0x20
#define ASR_CIP		0x10
#define ASR_DBR		0x01

#define CMD_RESET	0x00
#define CMD_SEL_ATN_XFER 0x09

#define ST_RESET_EAF	0x01
#define ST_RESET	0x00
#define ST_SAT_OK	0x16		/* select-and-transfer completed */
#define ST_SEL_TIMEOUT	0x42
#define ST_UNEXP_STATUS	0x4b		/* unexpected status phase */
#define PHASE_STATUS	0x46		/* resume at the status phase */

#define SCSI_TIMEOUT_MS	5000

#define B(off)		(*(volatile u8 *)(SDMAC + (off)))
#define W(off)		(*(volatile u16 *)(SDMAC + (off)))
#define L(off)		(*(volatile u32 *)(SDMAC + (off)))

static u8 *dmabuf;
static u32 dmasize;

u8 scsi_sense[18];
u32 scsi_last;			/* status << 8 | phase of the last failure */

static void
wr(u8 reg, u8 val)
{
	B(SASR) = reg;
	B(SCMD) = val;
}

static u8
rd(u8 reg)
{
	B(SASR) = reg;
	return B(SCMD);
}

static u8
asr(void)
{
	return B(SASR);
}

static u32
ms_now(void)
{
	u32 s, ms;

	time_now(&s, &ms);
	return s * 1000 + ms;
}

/* Wait for the chip's interrupt flag; returns the SCSI status register. */
static int
wait_int(u32 timeout_ms)
{
	u32 t0 = ms_now();

	while (!(asr() & ASR_INT))
		if (ms_now() - t0 > timeout_ms)
			return -1;
	return rd(R_STATUS);
}

void
scsi_dma_buffer(u32 chipmem, u32 size)
{
	dmabuf = (u8 *)chipmem;
	dmasize = size;
}

int
scsi_init(void)
{
	int st;

	W(SP_DMA) = 0;			/* stop any DMA (strobe) */
	W(DAWR) = 3;
	W(CNTR) = CNTR_PDMD;		/* no SDMAC interrupts, no DMA */
	W(CINT) = 0;			/* clear interrupts (strobe) */

	wr(R_OWNID, OWNID_EAF | SCSI_HOST_ID);
	wr(R_CMD, CMD_RESET);
	st = wait_int(500);
	if (st != ST_RESET_EAF && st != ST_RESET)
		return SCSI_EIO;
	if (!dmabuf)
		return SCSI_EIO;
	wr(R_CONTROL, CON_DMA | CON_EDI | CON_IDI);
	wr(R_TIMEOUT, 0x30);		/* selection timeout ~250 ms */
	wr(R_SRCID, 0);			/* no reselection */
	wr(R_SYNC, 0);			/* asynchronous transfers */
	return SCSI_OK;
}

static void
dma_stop(void)
{
	u32 t0 = ms_now();

	W(FLUSH) = 0;			/* drain the FIFO to memory */
	while (!(W(ISTR) & ISTR_FE) && ms_now() - t0 < 100)
		;
	W(CINT) = 0;
	W(SP_DMA) = 0;
}

static int
transfer(u32 target, u32 lun, const u8 *cdb, u8 *buf, u32 len, int data_in)
{
	u32 cdblen = (cdb[0] >> 5) == 0 ? 6 : (cdb[0] >> 5) == 5 ? 12 : 10;
	u32 i, done, t0;
	int st;

	if (len > dmasize)
		return SCSI_EIO;
	if (len && !data_in)
		memcpy(dmabuf, buf, len);

	t0 = ms_now();
	while (asr() & (ASR_BSY | ASR_CIP))
		if (ms_now() - t0 > 1000)
			return SCSI_ETIMEOUT;
	for (i = 0; i < cdblen; i++)
		wr((u8)(R_CDB + i), cdb[i]);
	wr(R_LUN, (u8)lun);
	wr(R_PHASE, 0);
	wr(R_COUNT, (u8)(len >> 16));
	wr(R_COUNT + 1, (u8)(len >> 8));
	wr(R_COUNT + 2, (u8)len);
	wr(R_DESTID, (u8)(target | (data_in ? DESTID_DPD : 0)));
	if (len) {
		W(CNTR) = CNTR_PDMD | (data_in ? 0 : CNTR_DDIR);
		L(ACR) = (u32)(uintptr_t)dmabuf;
		W(ST_DMA) = 0;		/* start (strobe) */
	}
	wr(R_CMD, CMD_SEL_ATN_XFER);

	for (;;) {
		u8 a = asr();

		if (a & ASR_INT) {
			st = rd(R_STATUS);
			if (st == ST_SAT_OK)
				break;
			if (st == ST_UNEXP_STATUS) {
				/* short data phase: finish status and message */
				wr(R_COUNT, 0);
				wr(R_COUNT + 1, 0);
				wr(R_COUNT + 2, 0);
				wr(R_PHASE, PHASE_STATUS);
				wr(R_CMD, CMD_SEL_ATN_XFER);
				continue;
			}
			if (len)
				dma_stop();
			if (st == ST_SEL_TIMEOUT)
				return SCSI_NODEV;
			scsi_last = (u32)st << 16 | (u32)rd(R_PHASE) << 8;
			return SCSI_EIO;
		}
		if (ms_now() - t0 > SCSI_TIMEOUT_MS) {
			if (len)
				dma_stop();
			wr(R_CMD, CMD_RESET);
			wait_int(500);
			scsi_init();
			return SCSI_ETIMEOUT;
		}
	}
	if (len)
		dma_stop();
	done = len - ((u32)rd(R_COUNT) << 16 | (u32)rd(R_COUNT + 1) << 8 |
	    rd(R_COUNT + 2));
	if (len && data_in)
		memcpy(buf, dmabuf, done);
	st = rd(R_LUN);			/* now: the target's status byte */
	scsi_last = (u32)st << 24 | done;
	return st == 0 ? SCSI_OK : st == 2 ? SCSI_ECHECK : SCSI_EIO;
}

int
scsi_cmd(u32 target, u32 lun, const u8 *cdb, void *buf, u32 len, int data_in)
{
	int err = transfer(target, lun, cdb, buf, len, data_in);

	if (err == SCSI_ECHECK) {
		u8 rs[6] = { 0x03, (u8)(lun << 5), 0, 0, sizeof(scsi_sense), 0 };
		u32 i;

		for (i = 0; i < sizeof(scsi_sense); i++)
			scsi_sense[i] = 0;
		if (transfer(target, lun, rs, scsi_sense, sizeof(scsi_sense), 1))
			scsi_sense[2] = 0xff;
	}
	return err;
}

const char *
scsi_strerror(int err)
{
	switch (err) {
	case SCSI_OK:		return "ok";
	case SCSI_NODEV:	return "no device";
	case SCSI_ECHECK:	return "check condition";
	case SCSI_EIO:		return "I/O error";
	case SCSI_ETIMEOUT:	return "timeout";
	}
	return "error";
}
