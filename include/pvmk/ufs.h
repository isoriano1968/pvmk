/*
 * Read-only UFS (4.3BSD FFS as used by AMIX/SVR4, big-endian) over a
 * block callback, for loading AMIX kernels from their root file system.
 */
#ifndef PVMK_UFS_H
#define PVMK_UFS_H

#include "pvmk/types.h"

#define UFS_OK		0
#define UFS_ENOENT	(-1)
#define UFS_EIO		(-2)
#define UFS_EBAD	(-3)		/* not UFS, or corrupt */
#define UFS_ETOOBIG	(-4)
#define UFS_ELOOP	(-5)		/* too many symbolic links */
#define UFS_ENOTDIR	(-6)

#define UFS_IFMT	0170000
#define UFS_IFDIR	0040000
#define UFS_IFREG	0100000
#define UFS_IFLNK	0120000

struct ufs {
	/* read count 512-byte blocks from the partition; 0 = ok */
	int (*read)(void *ctx, u32 blk, u32 count, void *buf);
	void *ctx;
	/* from the superblock */
	u32 bsize, fsize, frag, fsbtodb, cgoffset, cgmask, iblkno, ipg, fpg;
	u32 inopb, nindir, ncg, size;
};

struct ufs_stat {
	u32 ino, mode, size, nlink;
};

typedef void (*ufs_dir_cb)(void *arg, const char *name, const struct ufs_stat *st);

int  ufs_mount(struct ufs *fs);		/* read and check the superblock */
/* Resolve path from the root; symbolic links are followed (also the last
 * component if follow). */
int  ufs_lookup(struct ufs *fs, const char *path, int follow,
	struct ufs_stat *st);
int  ufs_read_file(struct ufs *fs, const struct ufs_stat *st, u32 off,
	void *dst, u32 len);
int  ufs_list_dir(struct ufs *fs, const struct ufs_stat *dir, ufs_dir_cb cb,
	void *arg);
int  ufs_readlink(struct ufs *fs, const struct ufs_stat *st, char *buf,
	u32 size);
const char *ufs_strerror(int err);

#endif /* PVMK_UFS_H */
