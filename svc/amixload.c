/*
 * AMIX kernel relocation (written for SORIX-LDOM after the rules of
 * AMIX's amiga/boot/rel.c and bind.c, which define the boot/kernel
 * contract):
 *
 *  - relocatable ELF (ET_REL): .text, .rodata, .data and .bss are placed
 *    back to back from the bind address, in that order; COMMON symbols
 *    are appended to .bss (aligned to their st_value); the symbols etext,
 *    edata and end get their final addresses; every R_68K_32 relocation
 *    becomes S + A. The entry point is the start of .text.
 *  - fully linked ELF (ET_EXEC): the first program header is the merged
 *    text+data segment, entered at e_entry.
 *
 * The image is modified in place (section contents and symbol table);
 * amix_enter() then copies the sections to their final addresses.
 */
#include "pvmk/amix.h"
#include "pvmk/bootcap.h"
#include "pvmk/kprintf.h"

#define ET_REL		1
#define ET_EXEC		2
#define EM_68K		4
#define SHT_PROGBITS	1
#define SHT_SYMTAB	2
#define SHT_RELA	4
#define SHT_NOBITS	8
#define SHF_WRITE	1
#define SHF_ALLOC	2
#define SHF_EXECINSTR	4
#define SHN_UNDEF	0
#define SHN_ABS		0xfff1
#define SHN_COMMON	0xfff2
#define R_68K_NONE	0
#define R_68K_32	1

struct ehdr {
	u8 ident[16];
	u16 type, machine;
	u32 version, entry, phoff, shoff, flags;
	u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct shdr {
	u32 name, type, flags, addr, offset, size, link, info, align, entsize;
};

struct phdr {
	u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

struct sym {
	u32 name, value, size;
	u8 info, other;
	u16 shndx;
};

struct rela {
	u32 offset, info;
	s32 addend;
};

static int
in_image(u32 size, u32 off, u32 len)
{
	return off <= size && len <= size - off;
}

static int
name_is(const char *strtab, u32 strsize, u32 name, const char *want)
{
	u32 i;

	for (i = 0; name + i < strsize; i++) {
		if (strtab[name + i] != want[i])
			return 0;
		if (!want[i])
			return 1;
	}
	return 0;
}

u32
amix_bind_address(void)
{
	u32 i, base = 0, best = 0;

	for (i = 0; i < bootcap.nmem; i++) {
		const struct memregion *m = &bootcap.mem[i];
		u32 size;

		if (m->attr & 0x0002)		/* MEMF_CHIP */
			continue;
		size = m->upper - (m->lower & ~0xffu);
		if (size > best) {
			best = size;
			base = m->lower & ~0xffu;
		}
	}
	return base;
}

static int
relocate_rel(u8 *elf, u32 size, u32 base, struct amix_kernel *k)
{
	const struct ehdr *eh = (const struct ehdr *)elf;
	struct shdr *sh = (struct shdr *)(elf + eh->shoff);
	struct shdr *text = NULL, *rodata = NULL, *data = NULL, *bss = NULL;
	struct shdr *symtab = NULL;
	struct sym *syms;
	const char *strtab;
	u32 i, nsyms, strsize, end = base;

	if (eh->shentsize != sizeof(struct shdr) ||
	    !in_image(size, eh->shoff, (u32)eh->shnum * sizeof(struct shdr))) {
		kprintf("amix: bad section header table\n");
		return -1;
	}
	for (i = 0; i < eh->shnum; i++) {
		struct shdr *s = &sh[i];

		if (s->type == SHT_PROGBITS && s->size && (s->flags & SHF_ALLOC)) {
			if (s->flags & SHF_EXECINSTR)
				text = s;
			else if (s->flags & SHF_WRITE)
				data = s;
			else
				rodata = s;
		} else if (s->type == SHT_NOBITS && (s->flags & SHF_WRITE) &&
		    s->size)
			bss = s;
		else if (s->type == SHT_SYMTAB)
			symtab = s;
		if (s->type != SHT_NOBITS && !in_image(size, s->offset, s->size)) {
			kprintf("amix: section %u outside the file\n", i);
			return -1;
		}
	}
	if (!text || !symtab) {
		kprintf("amix: no .text or no symbol table\n");
		return -1;
	}

	/* bind: text, rodata, data, bss back to back */
	text->addr = end;
	end += text->size;
	if (rodata) {
		rodata->addr = end;
		end += rodata->size;
	}
	if (data) {
		data->addr = end;
		end += data->size;
	}
	if (bss) {
		bss->addr = end;
		end += bss->size;
	}

	syms = (struct sym *)(elf + symtab->offset);
	nsyms = symtab->size / sizeof(struct sym);
	if (symtab->link >= eh->shnum) {
		kprintf("amix: bad symbol string table\n");
		return -1;
	}
	strtab = (const char *)(elf + sh[symtab->link].offset);
	strsize = sh[symtab->link].size;

	/* COMMON symbols: appended to bss, aligned to st_value */
	for (i = 0; i < nsyms; i++) {
		struct sym *s = &syms[i];
		u32 align, va;

		if (s->shndx != SHN_COMMON)
			continue;
		if (!bss) {
			kprintf("amix: COMMON symbol but no .bss\n");
			return -1;
		}
		align = s->value ? s->value : 1;
		va = (bss->addr + bss->size + align - 1) & ~(align - 1);
		s->shndx = (u16)(bss - sh);
		s->value = va - bss->addr;
		end = va + s->size;
		bss->size = end - bss->addr;
	}

	/* etext, edata, end */
	for (i = 0; i < nsyms; i++) {
		struct sym *s = &syms[i];

		if (!s->name)
			continue;
		if (name_is(strtab, strsize, s->name, "etext")) {
			s->value = text->addr + text->size;
			s->shndx = SHN_ABS;
		} else if (name_is(strtab, strsize, s->name, "edata") && data) {
			s->value = data->addr + data->size;
			s->shndx = SHN_ABS;
		} else if (name_is(strtab, strsize, s->name, "end")) {
			s->value = end;
			s->shndx = SHN_ABS;
		}
	}

	/* relocations of the loaded sections */
	for (i = 0; i < eh->shnum; i++) {
		struct shdr *rs = &sh[i], *target;
		struct rela *r;
		u32 n, j;

		if (rs->type != SHT_RELA || rs->info >= eh->shnum ||
		    rs->link >= eh->shnum)
			continue;
		target = &sh[rs->info];
		if (!(target->flags & SHF_ALLOC) || target->type == SHT_NOBITS)
			continue;		/* debug information */
		if (&sh[rs->link] != symtab) {
			kprintf("amix: relocations against another symbol table\n");
			return -1;
		}
		r = (struct rela *)(elf + rs->offset);
		n = rs->size / sizeof(struct rela);
		for (j = 0; j < n; j++, r++) {
			u32 type = r->info & 0xff, si = r->info >> 8, value;
			const struct sym *s;

			if (type == R_68K_NONE)
				continue;
			if (type != R_68K_32) {
				kprintf("amix: unsupported relocation type %u\n", type);
				return -1;
			}
			if (si >= nsyms || r->offset + 4 > target->size) {
				kprintf("amix: bad relocation %u in section %u\n", j,
				    rs->info);
				return -1;
			}
			s = &syms[si];
			if (s->shndx == SHN_ABS)
				value = s->value;
			else if (s->shndx != SHN_UNDEF && s->shndx < eh->shnum &&
			    sh[s->shndx].addr)
				value = sh[s->shndx].addr + s->value;
			else {
				kprintf("amix: relocation against undefined or unloaded "
				    "symbol %s\n", s->name < strsize ? strtab + s->name :
				    "?");
				return -1;
			}
			*(u32 *)(elf + target->offset + r->offset) =
			    value + (u32)r->addend;
			k->nrel++;
		}
	}

	k->bd.entry = text->addr;
	k->bd.tvaddr = text->addr;
	k->bd.toffset = text->offset;
	k->bd.tsize = text->size;
	if (rodata) {
		k->bd.rvaddr = rodata->addr;
		k->bd.roffset = rodata->offset;
		k->bd.rsize = rodata->size;
	}
	if (data) {
		k->bd.dvaddr = data->addr;
		k->bd.doffset = data->offset;
		k->bd.dsize = data->size;
	}
	k->end = end;
	return 0;
}

int
amix_relocate(u8 *elf, u32 size, u32 base, struct amix_kernel *k)
{
	const struct ehdr *eh = (const struct ehdr *)elf;

	memset(k, 0, sizeof(*k));
	if (size < sizeof(*eh) || elf[0] != 0x7f || elf[1] != 'E' ||
	    elf[2] != 'L' || elf[3] != 'F' || elf[4] != 1 || elf[5] != 2) {
		kprintf("amix: not a 32-bit big-endian ELF file\n");
		return -1;
	}
	if (eh->machine != EM_68K) {
		kprintf("amix: not an m68k ELF file\n");
		return -1;
	}
	k->type = eh->type;
	if (eh->type == ET_REL)
		return relocate_rel(elf, size, base, k);
	if (eh->type == ET_EXEC) {
		const struct phdr *ph = (const struct phdr *)(elf + eh->phoff);

		if (!eh->phnum || !in_image(size, eh->phoff, sizeof(*ph)) ||
		    !in_image(size, ph->offset, ph->filesz)) {
			kprintf("amix: bad program header\n");
			return -1;
		}
		k->bd.entry = eh->entry;
		k->bd.tvaddr = ph->vaddr;
		k->bd.toffset = ph->offset;
		k->bd.tsize = ph->filesz;
		k->end = ph->vaddr + ph->memsz;
		return 0;
	}
	kprintf("amix: ELF type %u not supported\n", eh->type);
	return -1;
}
