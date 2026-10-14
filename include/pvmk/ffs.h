/*
 * Read-only Amiga Fast File System (DOS\1) reader over a block callback.
 * Used by stage0 on the boot floppy (trackdisk.device via exec DoIO) and,
 * later, on hard disk partitions.
 */
#ifndef PVMK_FFS_H
#define PVMK_FFS_H

#include "pvmk/types.h"

#define FFS_BSIZE	512

#define FFS_OK		0
#define FFS_ENOENT	(-1)
#define FFS_EIO		(-2)
#define FFS_EBAD	(-3)		/* corrupt structure */
#define FFS_ETOOBIG	(-4)

struct ffs {
	int (*read)(void *ctx, u32 blk, void *buf);	/* 0 = ok */
	void *ctx;
	u32 root;
	u8 hbuf[FFS_BSIZE];		/* header / extension block */
	u8 dbuf[FFS_BSIZE];		/* partial data block */
};

typedef void (*ffs_dir_cb)(void *arg, const char *name, u32 size,
    int is_dir, u32 blk);

int ffs_lookup(struct ffs *fs, const char *path, u32 *blk, int *is_dir);
int ffs_read_file(struct ffs *fs, u32 blk, void *dst, u32 max, u32 *size);
int ffs_list_dir(struct ffs *fs, u32 blk, ffs_dir_cb cb, void *arg);
const char *ffs_strerror(int err);

#endif /* PVMK_FFS_H */
