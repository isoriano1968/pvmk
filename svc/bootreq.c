/*
 * One-shot boot request in the log area header (see log.c: the header
 * itself uses the first 64 bytes of a 256-byte area).
 */
#include "pvmk/bootreq.h"
#include "pvmk/kprintf.h"

#define BOOTREQ_OFFSET	128
#define BOOTREQ_MAGIC	0x42524551	/* 'BREQ' */

struct bootreq {
	u32 magic;
	u32 kind;
	char name[32];
	u32 sum;
};

static u32
req_sum(const volatile struct bootreq *r)
{
	u32 s = r->magic + r->kind + 0x13572468, i;

	for (i = 0; i < sizeof(r->name); i++)
		s = s * 31 + (u8)r->name[i];
	return s;
}

void
bootreq_set(u32 logarea, u32 kind, const char *name)
{
	volatile struct bootreq *r =
	    (volatile struct bootreq *)(logarea + BOOTREQ_OFFSET);
	u32 i;

	r->magic = BOOTREQ_MAGIC;
	r->kind = kind;
	for (i = 0; i < sizeof(r->name) - 1 && name[i]; i++)
		r->name[i] = name[i];
	for (; i < sizeof(r->name); i++)
		r->name[i] = 0;
	r->sum = req_sum(r);
}

u32
bootreq_take(u32 logarea, char *name)
{
	volatile struct bootreq *r =
	    (volatile struct bootreq *)(logarea + BOOTREQ_OFFSET);
	u32 kind = 0, i;

	if (r->magic == BOOTREQ_MAGIC && r->sum == req_sum(r)) {
		kind = r->kind;
		for (i = 0; i < sizeof(r->name); i++)
			name[i] = r->name[i];
		name[sizeof(r->name) - 1] = 0;
	}
	r->magic = 0;
	r->sum = 0;
	return kind;
}
