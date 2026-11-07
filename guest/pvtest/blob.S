/*
 * The pvtest guest, built separately (linked at PVTEST_BASE) and embedded
 * in the SORIX-ILOM-LDOM image as a flat binary -- only with make PVTEST=1:
 * its 15 KB are needed in the real machine's 320 KB hole for other things,
 * and only make test uses it.
 */
	.section .rodata
	.balign	4
	.globl	pvtest_image, pvtest_image_end
pvtest_image:
#ifdef PVMK_PVTEST		/* make PVTEST=1: the FS-UAE acceptance tests */
	.incbin	"build/pvtest.bin"
#endif
pvtest_image_end:
