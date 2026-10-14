/*
 * Amiga hardware registers and the few Kickstart (exec) structure offsets
 * pvmk uses before takeover. Offsets rather than structs so the same
 * definitions serve C and assembler without AmigaOS headers.
 */
#ifndef PVMK_AMIGA_H
#define PVMK_AMIGA_H

/* Custom chips */
#define CUSTOM_BASE	0xdff000
#define SERDATR		0xdff018	/* serial data and status (read) */
#define SERDAT		0xdff030	/* serial data (write) */
#define SERPER		0xdff032	/* serial period */
#define COLOR00		0xdff180
#define CUSTOM_INTREQR	0xdff01e
#define CUSTOM_INTENA	0xdff09a
#define CUSTOM_INTREQ	0xdff09c

#define SERDATR_TBE	13		/* transmit buffer empty */
#define SERDATR_TSRE	12		/* transmit shift register empty */
#define SERDATR_RBF	14		/* receive buffer full */

/* exec.library LVOs */
#define LVO_Alert	(-108)
#define LVO_Disable	(-120)
#define LVO_Enable	(-126)
#define LVO_Forbid	(-132)
#define LVO_FindResident (-96)
#define LVO_AllocMem	(-198)
#define LVO_FreeMem	(-210)
#define LVO_DoIO	(-456)
#define LVO_CacheClearU	(-636)

#define MEMF_PUBLIC	0x0001
#define MEMF_CHIP	0x0002
#define MEMF_FAST	0x0004

/* struct ExecBase offsets */
#define EB_VERSION	20		/* UWORD LibNode.lib_Version */
#define EB_REVISION	22		/* UWORD LibNode.lib_Revision */
#define EB_SOFTVER	34		/* UWORD SoftVer */
#define EB_ATTNFLAGS	0x128		/* UWORD AttnFlags */
#define EB_MEMLIST	0x142		/* struct List MemList */
#define EB_VBLANKFREQ	0x212		/* UBYTE VBlankFrequency */
#define EB_ECLOCKFREQ	0x238		/* ULONG ex_EClockFrequency (V36+) */

/* AttnFlags bits */
#define AFB_68010	0
#define AFB_68020	1
#define AFB_68030	2
#define AFB_68040	3
#define AFB_68881	4
#define AFB_68882	5
#define AFB_FPU40	6
#define AFB_68060	7

/* struct MemHeader offsets (after a 14-byte struct Node) */
#define MH_SUCC		0
#define MH_NAME		10
#define MH_ATTRIBUTES	14		/* UWORD */
#define MH_LOWER	20
#define MH_UPPER	24
#define MH_FREE		28

/* struct IOStdReq offsets */
#define IO_COMMAND	28		/* UWORD */
#define IO_ERROR	31		/* BYTE */
#define IO_ACTUAL	32
#define IO_LENGTH	36
#define IO_DATA		40
#define IO_OFFSET	44

#define CMD_READ	2
#define TD_MOTOR	9

/* struct Resident */
#define RT_INIT		22

#endif /* PVMK_AMIGA_H */
