/*
 * stage1 — first code running from the hole after takeover. Kickstart is
 * gone: interrupts masked, DMA off, MMU off, data cache off.
 */
#include "pvmk/amiga.h"
#include "pvmk/bootcap.h"
#include "pvmk/display.h"
#include "pvmk/keyboard.h"
#include "pvmk/kprintf.h"
#include "pvmk/layout.h"
#include "pvmk/log.h"
#include "pvmk/mk.h"
#include "pvmk/rdb.h"
#include "pvmk/scsi.h"
#include "pvmk/version.h"

static u32 vectors[256];

u32
sstack_top(void)
{
	return bootcap.hole_top - LOG_AREA_SIZE;
}

void
stage1_main(void *arg)
{
	struct bootcap *bc = arg;
	int i;

	for (i = 2; i < 256; i++)
		vectors[i] = (u32)(uintptr_t)exc_entry;
	set_vbr(vectors);
	REG16(COLOR00) = 0x003;		/* dark blue: microkernel running */

	if (bc->disp_chip) {
		/* replay what stage0 logged, then mirror the console */
		display_init(bc->disp_chip, bc->vblank == 50, bc->font);
		log_dump(0, display_putc);
	}
	kbd_init((int)bc->conf.keymap);

	kprintf("mk: takeover complete, running from %08x-%08x\n",
	    bc->image_base, bc->image_base + bc->image_size - 1);
	kprintf("mk: VBR %08x, supervisor stack %08x-%08x, log %08x-%08x\n",
	    get_vbr(), sstack_top() - SSTACK_SIZE, sstack_top() - 1,
	    sstack_top(), bc->hole_top - 1);
	cpu_probe();
	cpu_show();
	scsi_dma_buffer(bc->scsi_chip, SCSI_DMA_BLOCKS * 512);
	if (scsi_init() == SCSI_OK) {
		kprintf("scsi: A3000 internal SCSI (WD33C93A), host ID %u\n",
		    SCSI_HOST_ID);
		disk_scan(1);
		rdb_scan(0);
		part_show();
	} else
		kprintf("scsi: no controller found\n");
	shell_main();
}
