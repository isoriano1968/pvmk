/*
 * Microkernel memory reservation ("hole") at the top of the largest fast
 * RAM region. Reserved with AllocAbs at stage0 and hidden from guests later.
 *
 *	hole_base                                                    hole_top
 *	| image (text, data, bss, relocs) | free ... | sstack | log area |
 *
 * The log area sits at a fixed distance from the top of RAM so a warm
 * reset finds the previous boot's log in the same place.
 */
#ifndef PVMK_LAYOUT_H
#define PVMK_LAYOUT_H

#define HOLE_SIZE	(320 * 1024)
#define LOG_AREA_SIZE	(32 * 1024)
#define SSTACK_SIZE	(16 * 1024)

#endif /* PVMK_LAYOUT_H */
