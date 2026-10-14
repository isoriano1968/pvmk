/*
 * pvmk image and boot block parameter formats.
 *
 * Shared by the boot block (assembler, via __ASSEMBLER__), the pvmk image
 * entry code, and the host tools (mkimage, mkadf). All fields are big-endian.
 */
#ifndef PVMK_IMAGE_H
#define PVMK_IMAGE_H

/*
 * PVBB: boot block parameter area, at a fixed offset inside the 1 KB boot
 * block. Filled in by mkadf (floppy) or pvinstall (hard disk partition).
 */
#define PVBB_OFFSET	0x3c0		/* offset inside the boot block */
#define PVBB_MAGIC	0x50564242	/* 'PVBB' */
#define PVBB_VERSION	1

#define PVBB_O_MAGIC	0		/* u32 'PVBB' */
#define PVBB_O_VERSION	4		/* u16 */
#define PVBB_O_MEDIUM	6		/* u16 boot medium, see BOOT_* */
#define PVBB_O_START	8		/* u32 first block of image (absolute on device) */
#define PVBB_O_NBLOCKS	12		/* u32 number of 512-byte blocks */
#define PVBB_O_SIZE	16		/* u32 image size in bytes (CRC range) */
#define PVBB_O_ALLOC	20		/* u32 bytes to allocate (>= nblocks * 512) */
#define PVBB_O_CRC	24		/* u32 CRC-32 (IEEE, zlib) of image */
#define PVBB_O_ENTRY	28		/* u32 entry offset from image base */
#define PVBB_O_BAUD	32		/* u32 serial debug baud rate */
#define PVBB_SIZE	64

#define BOOT_FLOPPY	1
#define BOOT_HARDDISK	2

/* Image return values to the boot block (besides 0 = boot AmigaOS here) */
#define BOOTBLOCK_DECLINE 2		/* let strap boot the next device */

/*
 * PVMK image header, at offset 0 of the pvmk image (stage0/entry.S).
 * The image is linked at address 0 and carries its own relocation table.
 */
#define PVMK_MAGIC	0x50564d4b	/* 'PVMK' */
#define PVMK_HDR_VERSION 1

#define PVMK_O_BRA	0		/* bra.w to entry code */
#define PVMK_O_MAGIC	4		/* u32 'PVMK' */
#define PVMK_O_VERSION	8		/* u16 */
#define PVMK_O_HDRSIZE	10		/* u16 */
#define PVMK_O_FILESIZE	12		/* u32 bytes of text+data */
#define PVMK_O_BSSEND	16		/* u32 end of bss (offset) */
#define PVMK_O_RELOFF	20		/* u32 offset of relocation table in the file;
					   moved to BSSEND by entry.S at load */
#define PVMK_O_RELCNT	24		/* u32 number of 32-bit relocations */
#define PVMK_O_ALLOC	28		/* u32 memory needed to load and run */
#define PVMK_HDR_SIZE	40

#endif /* PVMK_IMAGE_H */
