/*
 * z3reloc: the Z3660's RAM and RTG moved below the AMIX kernel's virtual
 * space (svc/z3reloc.c).
 */
#ifndef PVMK_Z3RELOC_H
#define PVMK_Z3RELOC_H

#include "pvmk/types.h"

extern u32 z3reloc_done;	/* 1 once the bases moved (until a reset) */

void cmd_z3reloc(int argc, char **argv);
/* amix_start: the moved bases into AMIX's ConfigDevs and MemHeaders */
void z3reloc_bootinfo(u8 (*autocon)[0x44], u32 ncd, u8 (*memory)[32], u32 nmem);

#endif /* PVMK_Z3RELOC_H */
