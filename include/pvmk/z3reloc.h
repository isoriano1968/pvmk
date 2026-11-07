/*
 * z3reloc: the Z3660's RAM and RTG moved below the AMIX kernel's virtual
 * space (svc/z3reloc.c).
 */
#ifndef PVMK_Z3RELOC_H
#define PVMK_Z3RELOC_H

#include "pvmk/types.h"

extern u32 z3reloc_done;	/* 1 once the bases moved: no boot until a reset */

void cmd_z3reloc(int argc, char **argv);

#endif /* PVMK_Z3RELOC_H */
