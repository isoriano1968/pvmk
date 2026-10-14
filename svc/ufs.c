/*
 * Read-only UFS: 4.3BSD fast file system as written by AMIX (SVR4),
 * big-endian.
 *
 * Superblock at byte 8192 of the partition (magic 0x011954 at offset 1372).
 * Fragment addresses are in fsize units; cylinder group c starts at
 * fpg * c + cgoffset * (c & ~cgmask) and has its inodes at iblkno. Inodes
 * are 128 bytes: mode at 0, nlink at 2, size at 8 (64-bit), 12 direct
 * block pointers at 40, single/double/triple indirect at 88, block count at
 * 104. Directory entries are {u32 ino, u16 reclen, u16 namlen, name}.
 * Symbolic links keep their target in the data blocks.
 */
#include "pvmk/kprintf.h"
#include "pvmk/ufs.h"

#define SBOFF		8192
#define SB_MAGIC	0x011954
#define DINODE_SIZE	128
#define NDADDR		12
#define MAXBSIZE	8192
#define MAXSYMLINKS	8
#define MAXPATH		256

#define DI_MODE		0
#define DI_NLINK	2
#define DI_SIZE		12		/* low 32 bits of the 64-bit size */
#define DI_DB		40
#define DI_IB		88

static u8 sbuf[2048];			/* superblock, then the inode block */
static u8 ibuf[MAXBSIZE];		/* indirect blocks */
static u32 ib_cached;			/* fragment address held in ibuf */
static u8 dbuf[MAXBSIZE];		/* data blocks */

static u32
rd32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static u32
rd16(const u8 *p)
{
	return (u32)p[0] << 8 | p[1];
}

/* Read count fragments starting at fragment address fa. */
static int
read_frags(struct ufs *fs, u32 fa, u32 count, void *buf)
{
	u32 spf = fs->fsize / 512;

	return fs->read(fs->ctx, fa * spf, count * spf, buf) ? UFS_EIO : UFS_OK;
}

int
ufs_mount(struct ufs *fs)
{
	if (fs->read(fs->ctx, SBOFF / 512, sizeof(sbuf) / 512, sbuf))
		return UFS_EIO;
	if (rd32(sbuf + 1372) != SB_MAGIC)
		return UFS_EBAD;
	fs->iblkno = rd32(sbuf + 16);
	fs->cgoffset = rd32(sbuf + 24);
	fs->cgmask = rd32(sbuf + 28);
	fs->size = rd32(sbuf + 36);
	fs->ncg = rd32(sbuf + 44);
	fs->bsize = rd32(sbuf + 48);
	fs->fsize = rd32(sbuf + 52);
	fs->frag = rd32(sbuf + 56);
	fs->fsbtodb = rd32(sbuf + 100);
	fs->nindir = rd32(sbuf + 116);
	fs->inopb = rd32(sbuf + 120);
	fs->ipg = rd32(sbuf + 184);
	fs->fpg = rd32(sbuf + 188);
	if (fs->bsize > MAXBSIZE || fs->bsize < 4096 || fs->fsize < 512 ||
	    fs->bsize % fs->fsize || fs->frag != fs->bsize / fs->fsize ||
	    !fs->ipg || !fs->inopb || fs->inopb * DINODE_SIZE != fs->bsize ||
	    fs->nindir != fs->bsize / 4)
		return UFS_EBAD;
	ib_cached = 0;
	return UFS_OK;
}

/* Read inode ino into st, keeping the raw dinode in sbuf[0..127]. */
static int
get_inode(struct ufs *fs, u32 ino, struct ufs_stat *st)
{
	u32 cg = ino / fs->ipg, cgstart, fa, off, sector;
	u8 sec[512];

	if (ino < 2 || cg >= fs->ncg)
		return UFS_EBAD;
	cgstart = fs->fpg * cg + fs->cgoffset * (cg & ~fs->cgmask);
	fa = cgstart + fs->iblkno + (ino % fs->ipg) / fs->inopb * fs->frag;
	off = (ino % fs->inopb) * DINODE_SIZE;
	sector = fa * (fs->fsize / 512) + off / 512;
	if (fs->read(fs->ctx, sector, 1, sec))
		return UFS_EIO;
	memcpy(sbuf, sec + off % 512, DINODE_SIZE);
	st->ino = ino;
	st->mode = rd16(sbuf + DI_MODE);
	st->nlink = rd16(sbuf + DI_NLINK);
	st->size = rd32(sbuf + DI_SIZE);
	if (rd32(sbuf + 8))		/* > 4 GB: not for us */
		return UFS_ETOOBIG;
	return UFS_OK;
}

/* Fragment address of logical file block lbn (of the inode in sbuf). */
static int
bmap(struct ufs *fs, const u8 *di, u32 lbn, u32 *fa)
{
	u32 n = fs->nindir, ib;
	int err;

	if (lbn < NDADDR) {
		*fa = rd32(di + DI_DB + lbn * 4);
		return UFS_OK;
	}
	lbn -= NDADDR;
	if (lbn < n) {
		ib = rd32(di + DI_IB);
	} else {
		lbn -= n;
		if (lbn >= n * n)
			return UFS_ETOOBIG;	/* triple indirect: >16 GB */
		ib = rd32(di + DI_IB + 4);
		if (!ib) {
			*fa = 0;
			return UFS_OK;
		}
		if (ib != ib_cached) {
			if ((err = read_frags(fs, ib, fs->frag, ibuf)))
				return err;
			ib_cached = ib;
		}
		ib = rd32(ibuf + lbn / n * 4);
		lbn %= n;
	}
	if (!ib) {
		*fa = 0;			/* hole */
		return UFS_OK;
	}
	if (ib != ib_cached) {
		if ((err = read_frags(fs, ib, fs->frag, ibuf)))
			return err;
		ib_cached = ib;
	}
	*fa = rd32(ibuf + lbn * 4);
	return UFS_OK;
}

int
ufs_read_file(struct ufs *fs, const struct ufs_stat *st, u32 off, void *dst,
    u32 len)
{
	u8 di[DINODE_SIZE], *out = dst;
	struct ufs_stat tmp;
	int err;

	if ((err = get_inode(fs, st->ino, &tmp)))
		return err;
	memcpy(di, sbuf, sizeof(di));
	if (off > st->size)
		return UFS_EBAD;
	if (len > st->size - off)
		len = st->size - off;
	while (len) {
		u32 lbn = off / fs->bsize, boff = off % fs->bsize, fa;
		u32 n = fs->bsize - boff;

		if (n > len)
			n = len;
		if ((err = bmap(fs, di, lbn, &fa)))
			return err;
		if (!fa)
			memset(out, 0, n);
		else if (!boff && n == fs->bsize) {
			if ((err = read_frags(fs, fa, fs->frag, out)))
				return err;
		} else {
			/* only the fragments that hold [boff, boff + n) */
			u32 f0 = boff / fs->fsize;
			u32 f1 = (boff + n + fs->fsize - 1) / fs->fsize;

			if ((err = read_frags(fs, fa + f0, f1 - f0,
			    dbuf + f0 * fs->fsize)))
				return err;
			memcpy(out, dbuf + boff, n);
		}
		out += n;
		off += n;
		len -= n;
	}
	return UFS_OK;
}

int
ufs_readlink(struct ufs *fs, const struct ufs_stat *st, char *buf, u32 size)
{
	int err;

	if ((st->mode & UFS_IFMT) != UFS_IFLNK || st->size >= size)
		return UFS_EBAD;
	if ((err = ufs_read_file(fs, st, 0, buf, st->size)))
		return err;
	buf[st->size] = 0;
	return UFS_OK;
}

/*
 * Walk a directory; cb gets each entry (except "." and ".."). If want is
 * set, stop at the entry of that name and return its inode in *found.
 */
static int
walk_dir(struct ufs *fs, const struct ufs_stat *dir, const char *want,
    u32 wantlen, u32 *found, ufs_dir_cb cb, void *arg)
{
	static u8 blk[MAXBSIZE];
	u32 off;
	int err;

	if ((dir->mode & UFS_IFMT) != UFS_IFDIR)
		return UFS_ENOTDIR;
	for (off = 0; off < dir->size; off += fs->bsize) {
		u32 n = dir->size - off < fs->bsize ? dir->size - off : fs->bsize;
		u32 o = 0;

		if ((err = ufs_read_file(fs, dir, off, blk, n)))
			return err;
		while (o + 8 <= n) {
			u32 ino = rd32(blk + o), reclen = rd16(blk + o + 4);
			u32 namlen = rd16(blk + o + 6);

			if (reclen < 8 || o + reclen > n || namlen > reclen - 8)
				return UFS_EBAD;
			if (ino && want) {
				u32 i;

				for (i = 0; i < namlen && i < wantlen &&
				    blk[o + 8 + i] == (u8)want[i]; i++)
					;
				if (i == namlen && i == wantlen) {
					*found = ino;
					return UFS_OK;
				}
			} else if (ino && cb && !(namlen == 1 && blk[o + 8] == '.') &&
			    !(namlen == 2 && blk[o + 8] == '.' && blk[o + 9] == '.')) {
				char name[256];
				struct ufs_stat st;
				u8 save[DINODE_SIZE];

				memcpy(name, blk + o + 8, namlen);
				name[namlen] = 0;
				memcpy(save, sbuf, sizeof(save));
				if (get_inode(fs, ino, &st) == UFS_OK)
					cb(arg, name, &st);
				memcpy(sbuf, save, sizeof(save));
			}
			o += reclen;
		}
	}
	return want ? UFS_ENOENT : UFS_OK;
}

int
ufs_list_dir(struct ufs *fs, const struct ufs_stat *dir, ufs_dir_cb cb,
    void *arg)
{
	return walk_dir(fs, dir, NULL, 0, NULL, cb, arg);
}

int
ufs_lookup(struct ufs *fs, const char *path, int follow, struct ufs_stat *st)
{
	static char buf[2][MAXPATH];
	struct ufs_stat cur, parent;
	const char *p = path;
	int err, links = 0, which = 0;

	if ((err = get_inode(fs, 2, &cur)))
		return err;
	parent = cur;
	for (;;) {
		const char *end;
		u32 len, ino;

		while (*p == '/')
			p++;
		if (!*p)
			break;
		for (end = p; *end && *end != '/'; end++)
			;
		len = (u32)(end - p);
		if ((err = walk_dir(fs, &cur, p, len, &ino, NULL, NULL)))
			return err;
		parent = cur;
		if ((err = get_inode(fs, ino, &cur)))
			return err;
		p = end;
		if ((cur.mode & UFS_IFMT) == UFS_IFLNK && (*p || follow)) {
			char *t = buf[which];
			u32 tl, rl = 0;

			if (++links > MAXSYMLINKS)
				return UFS_ELOOP;
			if ((err = ufs_readlink(fs, &cur, t, MAXPATH)))
				return err;
			/* target followed by the rest of the path */
			tl = (u32)strlen(t);
			while (p[rl])
				rl++;
			if (tl + rl >= MAXPATH)
				return UFS_ETOOBIG;
			memcpy(t + tl, p, rl + 1);
			p = t;
			which ^= 1;
			if (*p == '/') {
				if ((err = get_inode(fs, 2, &cur)))
					return err;
			} else
				cur = parent;	/* relative to the link's directory */
		}
	}
	*st = cur;
	return UFS_OK;
}

const char *
ufs_strerror(int err)
{
	switch (err) {
	case UFS_OK:		return "ok";
	case UFS_ENOENT:	return "not found";
	case UFS_EIO:		return "read error";
	case UFS_EBAD:		return "not a UFS file system, or corrupt";
	case UFS_ETOOBIG:	return "too large";
	case UFS_ELOOP:		return "too many symbolic links";
	case UFS_ENOTDIR:	return "not a directory";
	}
	return "error";
}
