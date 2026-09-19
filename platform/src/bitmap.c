/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * Bit search and bitmap helpers (linux/bitops.h). Bit 0 is the LSB of
 * word 0, the kernel's little-endian bitmap layout, which is also the
 * on-disk layout of $Bitmap and the MFT bitmap.
 */
#include <linux/bitops.h>
#include <linux/kernel.h>

unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{
	unsigned long word, mask;

	if (offset >= size)
		return size;
	word = addr[BIT_WORD(offset)] & (~0UL << (offset % BITS_PER_LONG));
	offset &= ~(BITS_PER_LONG - 1);
	while (!word) {
		offset += BITS_PER_LONG;
		if (offset >= size)
			return size;
		word = addr[BIT_WORD(offset)];
	}
	offset += __ffs(word);
	if (offset >= size)
		return size;
	mask = 0;
	(void)mask;
	return offset;
}

unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset)
{
	unsigned long word;

	if (offset >= size)
		return size;
	word = ~addr[BIT_WORD(offset)] & (~0UL << (offset % BITS_PER_LONG));
	offset &= ~(BITS_PER_LONG - 1);
	while (!word) {
		offset += BITS_PER_LONG;
		if (offset >= size)
			return size;
		word = ~addr[BIT_WORD(offset)];
	}
	offset += __ffs(word);
	return offset < size ? offset : size;
}

unsigned long find_first_bit(const unsigned long *addr, unsigned long size)
{
	return find_next_bit(addr, size, 0);
}

unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
	return find_next_zero_bit(addr, size, 0);
}

unsigned long find_last_bit(const unsigned long *addr, unsigned long size)
{
	unsigned long idx;

	if (!size)
		return 0;
	idx = (size - 1) / BITS_PER_LONG;
	for (;;) {
		unsigned long word = addr[idx];
		unsigned long valid = (idx + 1) * BITS_PER_LONG > size ?
			(~0UL >> ((idx + 1) * BITS_PER_LONG - size)) : ~0UL;
		word &= valid;
		if (word)
			return idx * BITS_PER_LONG + __fls(word);
		if (!idx)
			return size;
		idx--;
	}
}

unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits)
{
	unsigned int i, w = 0, full = nbits / BITS_PER_LONG;

	for (i = 0; i < full; i++)
		w += hweight_long(src[i]);
	if (nbits % BITS_PER_LONG)
		w += hweight_long(src[full] & (~0UL >> (BITS_PER_LONG - nbits % BITS_PER_LONG)));
	return w;
}

void bitmap_set(unsigned long *map, unsigned int start, unsigned int nbits)
{
	while (nbits) {
		unsigned int off = start % BITS_PER_LONG;
		unsigned int len = BITS_PER_LONG - off;
		unsigned long mask;

		if (len > nbits)
			len = nbits;
		mask = len == BITS_PER_LONG ? ~0UL : ((1UL << len) - 1) << off;
		map[BIT_WORD(start)] |= mask;
		start += len;
		nbits -= len;
	}
}

void bitmap_clear(unsigned long *map, unsigned int start, unsigned int nbits)
{
	while (nbits) {
		unsigned int off = start % BITS_PER_LONG;
		unsigned int len = BITS_PER_LONG - off;
		unsigned long mask;

		if (len > nbits)
			len = nbits;
		mask = len == BITS_PER_LONG ? ~0UL : ((1UL << len) - 1) << off;
		map[BIT_WORD(start)] &= ~mask;
		start += len;
		nbits -= len;
	}
}
