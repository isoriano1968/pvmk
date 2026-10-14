/*
 * ufstool — run the microkernel's UFS reader (svc/ufs.c) on the host,
 * against a disk image, read-only. For testing and for inspecting AMIX
 * disks.
 *
 *   ufstool image.hdf START_BLOCK ls PATH
 *   ufstool image.hdf START_BLOCK cat PATH
 *   ufstool image.hdf START_BLOCK get PATH OUTFILE
 *
 * START_BLOCK is the partition's first 512-byte block (see mkhdf --list:
 * lowcyl * blocks per cylinder).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* svc/ufs.c expects the microkernel's freestanding headers */
#define PVMK_TYPES_H
#define PVMK_KPRINTF_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
#include "../svc/ufs.c"

static FILE *img;
static long long part_off;

static int
img_read(void *ctx, u32 blk, u32 count, void *buf)
{
	(void)ctx;
	if (fseeko(img, part_off + (long long)blk * 512, SEEK_SET) ||
	    fread(buf, 512, count, img) != count)
		return -1;
	return 0;
}

static void
show(void *arg, const char *name, const struct ufs_stat *st)
{
	(void)arg;
	printf("%06o %8u %s%s\n", st->mode, st->size, name,
	    (st->mode & UFS_IFMT) == UFS_IFDIR ? "/" :
	    (st->mode & UFS_IFMT) == UFS_IFLNK ? "@" : "");
}

int
main(int argc, char **argv)
{
	struct ufs fs = { .read = img_read };
	struct ufs_stat st;
	int err;

	if (argc < 5) {
		fprintf(stderr, "usage: ufstool image START_BLOCK ls|cat|get "
		    "PATH [OUT]\n");
		return 2;
	}
	if (!(img = fopen(argv[1], "rb"))) {
		perror(argv[1]);
		return 1;
	}
	part_off = strtoll(argv[2], NULL, 0) * 512;
	if ((err = ufs_mount(&fs))) {
		fprintf(stderr, "ufstool: %s\n", ufs_strerror(err));
		return 1;
	}
	err = ufs_lookup(&fs, argv[4], 1, &st);
	if (err) {
		fprintf(stderr, "ufstool: %s: %s\n", argv[4], ufs_strerror(err));
		return 1;
	}
	if (!strcmp(argv[3], "ls")) {
		if ((st.mode & UFS_IFMT) == UFS_IFDIR)
			err = ufs_list_dir(&fs, &st, show, NULL);
		else
			show(NULL, argv[4], &st);
	} else if (!strcmp(argv[3], "cat") || !strcmp(argv[3], "get")) {
		char *buf = malloc(st.size + 1);
		FILE *out = stdout;

		err = ufs_read_file(&fs, &st, 0, buf, st.size);
		if (!err && argc > 5 && !(out = fopen(argv[5], "wb"))) {
			perror(argv[5]);
			return 1;
		}
		if (!err)
			fwrite(buf, 1, st.size, out);
		if (out != stdout)
			fclose(out);
	}
	if (err) {
		fprintf(stderr, "ufstool: %s\n", ufs_strerror(err));
		return 1;
	}
	return 0;
}
