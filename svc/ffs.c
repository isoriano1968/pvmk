/*
 * Read-only FFS (DOS\1) reader. Block layout as written by host/mkadf.c:
 * headers and extension blocks are checksummed, data blocks are raw.
 */
#include "pvmk/ffs.h"
#include "pvmk/kprintf.h"

#define HT_SIZE		72
#define T_HEADER	2
#define T_LIST		16
#define ST_ROOT		1
#define ST_USERDIR	2
#define ST_FILE		(-3)

#define O_TYPE		0
#define O_KEY		4
#define O_HIGHSEQ	8
#define O_TABLE		24
#define O_BYTESIZE	324
#define O_NAMECMT	328		/* DOS\6/7: name, then comment */
#define O_NAME		432
#define O_HASHCHAIN	496
#define O_EXTENSION	504
#define O_SECTYPE	508

static u32
rd32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static int
upper(const struct ffs *fs, int c)
{
	if (c >= 'a' && c <= 'z')
		return c - 32;
	if (fs->intl && c >= 0xe0 && c <= 0xfe && c != 0xf7)
		return c - 32;			/* Latin-1 */
	return c;
}

static int
name_hash(const struct ffs *fs, const char *name, int len)
{
	u32 h = (u32)len;
	int i;

	for (i = 0; i < len; i++)
		h = (h * 13 + (u32)upper(fs, (u8)name[i])) & 0x7ff;
	return (int)(h % HT_SIZE);
}

void
ffs_set_dostype(struct ffs *fs, u32 dostype)
{
	u32 v = dostype & 0xff;

	fs->intl = v >= 2;
	fs->longname = v >= 6;
}

/* the entry name in hbuf, a BSTR; *max its longest length */
static const u8 *
entry_name(const struct ffs *fs, u32 *max)
{
	*max = fs->longname ? 107 : 30;
	return fs->hbuf + (fs->longname ? O_NAMECMT : O_NAME);
}

/* Read a checksummed header/list block of the expected type into hbuf. */
static int
read_meta(struct ffs *fs, u32 blk, u32 type)
{
	u32 sum = 0;
	int i;

	if (fs->read(fs->ctx, blk, fs->hbuf))
		return FFS_EIO;
	for (i = 0; i < FFS_BSIZE; i += 4)
		sum += rd32(fs->hbuf + i);
	if (sum != 0 || rd32(fs->hbuf + O_TYPE) != type)
		return FFS_EBAD;
	if (type == T_HEADER && blk != fs->root && rd32(fs->hbuf + O_KEY) != blk)
		return FFS_EBAD;
	return FFS_OK;
}

static int
sectype(struct ffs *fs)
{
	return (int)rd32(fs->hbuf + O_SECTYPE);
}

int
ffs_lookup(struct ffs *fs, const char *path, u32 *blk, int *is_dir)
{
	u32 cur = fs->root;
	int err;

	while (*path == '/')
		path++;
	for (;;) {
		const char *end = path;
		int len, guard = 0;
		u32 b;

		if ((err = read_meta(fs, cur, T_HEADER)))
			return err;
		if (!*path)
			break;
		if (sectype(fs) != ST_ROOT && sectype(fs) != ST_USERDIR)
			return FFS_ENOENT;
		while (*end && *end != '/')
			end++;
		len = (int)(end - path);
		b = rd32(fs->hbuf + O_TABLE + name_hash(fs, path, len) * 4);
		while (b) {
			const u8 *n;
			u32 max;
			int i;

			if (guard++ > 4096)
				return FFS_EBAD;
			if ((err = read_meta(fs, b, T_HEADER)))
				return err;
			n = entry_name(fs, &max);
			if (n[0] == len) {
				for (i = 0; i < len; i++)
					if (upper(fs, n[1 + i]) != upper(fs, (u8)path[i]))
						break;
				if (i == len)
					break;
			}
			b = rd32(fs->hbuf + O_HASHCHAIN);
		}
		if (!b)
			return FFS_ENOENT;
		cur = b;
		path = end;
		while (*path == '/')
			path++;
	}
	*blk = cur;
	*is_dir = sectype(fs) == ST_ROOT || sectype(fs) == ST_USERDIR;
	return FFS_OK;
}

int
ffs_read_file(struct ffs *fs, u32 blk, void *dst, u32 max, u32 *size)
{
	u8 *out = dst;
	u32 total, done = 0;
	int err;

	if ((err = read_meta(fs, blk, T_HEADER)))
		return err;
	if (sectype(fs) != ST_FILE)
		return FFS_ENOENT;
	total = rd32(fs->hbuf + O_BYTESIZE);
	*size = total;
	if (total > max)
		return FFS_ETOOBIG;
	for (;;) {
		u32 cnt = rd32(fs->hbuf + O_HIGHSEQ), i, ext;

		if (cnt > HT_SIZE)
			return FFS_EBAD;
		for (i = 0; i < cnt && done < total; i++) {
			u32 d = rd32(fs->hbuf + O_TABLE + (HT_SIZE - 1 - i) * 4);
			u32 n = total - done;

			if (n >= FFS_BSIZE) {
				if (fs->read(fs->ctx, d, out + done))
					return FFS_EIO;
				done += FFS_BSIZE;
			} else {
				if (fs->read(fs->ctx, d, fs->dbuf))
					return FFS_EIO;
				memcpy(out + done, fs->dbuf, n);
				done += n;
			}
		}
		ext = rd32(fs->hbuf + O_EXTENSION);
		if (!ext || done >= total)
			break;
		if ((err = read_meta(fs, ext, T_LIST)))
			return err;
	}
	return done == total ? FFS_OK : FFS_EBAD;
}

int
ffs_list_dir(struct ffs *fs, u32 blk, ffs_dir_cb cb, void *arg)
{
	u32 table[HT_SIZE];
	int h, err;

	if ((err = read_meta(fs, blk, T_HEADER)))
		return err;
	if (sectype(fs) != ST_ROOT && sectype(fs) != ST_USERDIR)
		return FFS_ENOENT;
	for (h = 0; h < HT_SIZE; h++)
		table[h] = rd32(fs->hbuf + O_TABLE + h * 4);
	for (h = 0; h < HT_SIZE; h++) {
		u32 b = table[h];
		int guard = 0;

		while (b && guard++ < 4096) {
			char name[108];
			const u8 *s;
			u32 n, max;

			if ((err = read_meta(fs, b, T_HEADER)))
				return err;
			s = entry_name(fs, &max);
			n = s[0] > max ? max : s[0];
			memcpy(name, s + 1, n);
			name[n] = 0;
			cb(arg, name, rd32(fs->hbuf + O_BYTESIZE),
			    sectype(fs) == ST_USERDIR, b);
			b = rd32(fs->hbuf + O_HASHCHAIN);
		}
	}
	return FFS_OK;
}

const char *
ffs_strerror(int err)
{
	switch (err) {
	case FFS_OK:		return "ok";
	case FFS_ENOENT:	return "not found";
	case FFS_EIO:		return "read error";
	case FFS_EBAD:		return "corrupt file system";
	case FFS_ETOOBIG:	return "file too large";
	}
	return "error";
}
