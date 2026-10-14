/*
 * mkimage — turn the pvmk ELF (linked at 0 with --emit-relocs) into a flat,
 * self-relocating image:
 *
 *	[ text | rodata | data ][ pad to 4 ][ u32 reloc offsets ... ]
 *
 * At load time entry.S moves the offsets to just past bss (where takeover
 * finds them to relocate the image again), so the image needs
 * bss_end + 4 * nrel bytes of memory.
 * and fill in the PVMK header (include/pvmk/image.h). Only R_68K_32
 * relocations against non-absolute symbols are recorded; PC-relative ones
 * need no fixup. Anything else is an error.
 *
 * usage: mkimage in.elf out.bin
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pvmk/image.h"

#define SHT_PROGBITS	1
#define SHT_SYMTAB	2
#define SHT_RELA	4
#define SHT_NOBITS	8
#define SHT_REL		9
#define SHF_ALLOC	0x2
#define SHN_ABS		0xfff1

#define R_68K_NONE	0
#define R_68K_32	1
#define R_68K_PC32	4
#define R_68K_PC16	5
#define R_68K_PC8	6

static uint8_t *elf;
static size_t elf_size;

static uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | p[2] << 8 | p[3];
}

static uint16_t
be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void
die(const char *msg)
{
	fprintf(stderr, "mkimage: %s\n", msg);
	exit(1);
}

struct shdr {
	uint32_t type, flags, addr, offset, size, link, info, entsize;
};

static struct shdr
section(unsigned i)
{
	uint32_t shoff = be32(elf + 32);
	uint16_t shentsize = be16(elf + 46);
	const uint8_t *s = elf + shoff + (size_t)i * shentsize;
	struct shdr h;

	if (shoff + (size_t)(i + 1) * shentsize > elf_size)
		die("section header out of range");
	h.type = be32(s + 4);
	h.flags = be32(s + 8);
	h.addr = be32(s + 12);
	h.offset = be32(s + 16);
	h.size = be32(s + 20);
	h.link = be32(s + 24);
	h.info = be32(s + 28);
	h.entsize = be32(s + 36);
	return h;
}

static int
cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

int
main(int argc, char **argv)
{
	FILE *f;
	unsigned i, shnum;
	uint32_t file_end = 0, mem_end = 0, nrel = 0, cap = 1024, reloff, alloc;
	uint32_t *rel;
	uint8_t *img;
	size_t out_size;

	if (argc != 3) {
		fprintf(stderr, "usage: mkimage in.elf out.bin\n");
		return 2;
	}
	if (!(f = fopen(argv[1], "rb")))
		die("cannot open input");
	fseek(f, 0, SEEK_END);
	elf_size = (size_t)ftell(f);
	rewind(f);
	elf = malloc(elf_size);
	if (!elf || fread(elf, 1, elf_size, f) != elf_size)
		die("read error");
	fclose(f);

	if (elf_size < 52 || memcmp(elf, "\177ELF\1\2", 6) != 0)
		die("not a 32-bit big-endian ELF file");
	if (be16(elf + 18) != 4)
		die("not an m68k ELF file");
	shnum = be16(elf + 48);

	for (i = 0; i < shnum; i++) {
		struct shdr s = section(i);

		if (!(s.flags & SHF_ALLOC) || s.size == 0)
			continue;
		if (s.addr + s.size > mem_end)
			mem_end = s.addr + s.size;
		if (s.type != SHT_NOBITS && s.addr + s.size > file_end)
			file_end = s.addr + s.size;
	}
	if (file_end < PVMK_HDR_SIZE)
		die("image too small");
	if (mem_end & 3)
		die("bss end not 4-byte aligned (check pvmk.ld)");

	img = calloc(1, file_end);
	if (!img)
		die("out of memory");
	for (i = 0; i < shnum; i++) {
		struct shdr s = section(i);

		if (!(s.flags & SHF_ALLOC) || s.type == SHT_NOBITS || !s.size)
			continue;
		if (s.offset + s.size > elf_size)
			die("section data out of range");
		memcpy(img + s.addr, elf + s.offset, s.size);
	}
	if (be32(img + PVMK_O_MAGIC) != PVMK_MAGIC)
		die("PVMK header not found at offset 0 (entry.S must link first)");

	rel = malloc(cap * sizeof(*rel));
	for (i = 0; i < shnum; i++) {
		struct shdr s = section(i), target, symtab;
		uint32_t j;

		if (s.type == SHT_REL)
			die("SHT_REL relocations are not supported");
		if (s.type != SHT_RELA)
			continue;
		target = section(s.info);
		if (!(target.flags & SHF_ALLOC))
			continue;	/* debug sections */
		if (target.type == SHT_NOBITS)
			die("relocation into bss");
		symtab = section(s.link);
		for (j = 0; j < s.size / 12; j++) {
			const uint8_t *r = elf + s.offset + j * 12;
			uint32_t off = be32(r), info = be32(r + 4);
			uint32_t type = info & 0xff, sym = info >> 8;
			uint16_t shndx = be16(elf + symtab.offset + sym * 16 + 14);

			switch (type) {
			case R_68K_NONE:
			case R_68K_PC32:
			case R_68K_PC16:
			case R_68K_PC8:
				continue;
			case R_68K_32:
				if (shndx == SHN_ABS)
					continue;
				if (off + 4 > file_end)
					die("relocation outside the image");
				if (nrel == cap)
					rel = realloc(rel, (cap *= 2) * sizeof(*rel));
				rel[nrel++] = off;
				break;
			default:
				fprintf(stderr, "mkimage: unsupported relocation type %u at "
				    "0x%x\n", type, off);
				return 1;
			}
		}
	}
	qsort(rel, nrel, sizeof(*rel), cmp_u32);

	reloff = (file_end + 3) & ~3u;
	out_size = reloff + (size_t)nrel * 4;
	alloc = mem_end + nrel * 4;
	if (alloc < out_size)
		alloc = (uint32_t)out_size;
	img = realloc(img, out_size);
	memset(img + file_end, 0, reloff - file_end);
	for (i = 0; i < nrel; i++)
		put32(img + reloff + i * 4, rel[i]);

	/* bss is cleared in longwords from FILESIZE; keep it aligned. */
	put32(img + PVMK_O_FILESIZE, reloff);
	put32(img + PVMK_O_BSSEND, mem_end);
	put32(img + PVMK_O_RELOFF, reloff);
	put32(img + PVMK_O_RELCNT, nrel);
	put32(img + PVMK_O_ALLOC, alloc);

	if (!(f = fopen(argv[2], "wb")) || fwrite(img, 1, out_size, f) != out_size)
		die("write error");
	fclose(f);
	printf("mkimage: %s: text+data %u, bss end %u, %u relocs, file %zu, "
	    "alloc %u\n", argv[2], file_end, mem_end, nrel, out_size, alloc);
	return 0;
}
