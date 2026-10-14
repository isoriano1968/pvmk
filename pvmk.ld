/*
 * pvmk image link script. The image is linked at 0 and relocated at load
 * time from the table mkimage builds out of --emit-relocs output.
 */
OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)

SECTIONS
{
	. = 0;
	.text : {
		KEEP(*(.text.entry))
		*(.text .text.*)
	}
	.rodata : ALIGN(4) {
		*(.rodata .rodata.*)
	}
	.data : ALIGN(4) {
		*(.data .data.*)
		. = ALIGN(4);
	}
	_edata = .;
	.bss : ALIGN(4) {
		*(.bss .bss.* COMMON)
		. = ALIGN(4);
	}
	_end = .;

	/DISCARD/ : {
		*(.comment) *(.note*) *(.eh_frame*) *(.got*) *(.interp)
	}
}
