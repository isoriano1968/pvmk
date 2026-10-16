/*
 * The pvtest guest, built separately (linked at PVTEST_BASE) and embedded
 * in the SORIX-ILOM-LDOM image as a flat binary.
 */
	.section .rodata
	.balign	4
	.globl	pvtest_image, pvtest_image_end
pvtest_image:
	.incbin	"build/pvtest.bin"
pvtest_image_end:
