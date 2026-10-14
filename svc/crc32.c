/*
 * CRC-32 (IEEE 802.3, as zlib), bitwise; same as the boot block.
 */
#include "pvmk/mk.h"

u32
crc32(const void *buf, u32 len)
{
	const u8 *p = buf;
	u32 c = 0xffffffff;
	int k;

	while (len--) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = c & 1 ? (c >> 1) ^ 0xedb88320 : c >> 1;
	}
	return ~c;
}
