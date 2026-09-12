/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BITOPS_H
#define _LINUX_BITOPS_H

#include <string.h>
#include <strings.h>	/* declare ffs()/fls() before we shadow them */
#include <linux/types.h>
#include <linux/atomic.h>

#define BIT(nr) (1UL << (nr))
#define BIT_ULL(nr) (1ULL << (nr))
#define BIT_MASK(nr) (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr) ((nr) / BITS_PER_LONG)
#define BITS_TO_LONGS(nr) DIV_ROUND_UP_BITS(nr, BITS_PER_LONG)
#define DIV_ROUND_UP_BITS(n, d) (((n) + (d) - 1) / (d))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (64 - 1 - (h))))
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]

/* Atomic bit operations on unsigned long arrays (bit 0 = LSB of word 0,
 * matching the kernel's little-endian bitmap layout). */
static inline void set_bit(unsigned int nr, volatile unsigned long *addr)
{ __atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST); }
static inline void clear_bit(unsigned int nr, volatile unsigned long *addr)
{ __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST); }
static inline void change_bit(unsigned int nr, volatile unsigned long *addr)
{ __atomic_fetch_xor(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST); }
static inline int test_bit(unsigned int nr, const volatile unsigned long *addr)
{ return (__atomic_load_n(&addr[BIT_WORD(nr)], __ATOMIC_SEQ_CST) >> (nr % BITS_PER_LONG)) & 1; }
static inline int test_and_set_bit(unsigned int nr, volatile unsigned long *addr)
{ return (__atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)) != 0; }
static inline int test_and_clear_bit(unsigned int nr, volatile unsigned long *addr)
{ return (__atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)) != 0; }
#define __set_bit set_bit
#define __clear_bit clear_bit
#define __test_and_set_bit test_and_set_bit
#define __test_and_clear_bit test_and_clear_bit
#define set_bit_le set_bit
#define clear_bit_le clear_bit
#define test_bit_le test_bit
#define test_and_set_bit_le test_and_set_bit
#define test_and_clear_bit_le test_and_clear_bit
#define set_bit_unlock clear_bit /* not used meaningfully */
#define clear_bit_unlock clear_bit

/* <strings.h> declares ffs()/fls() with int arguments; the kernel versions
 * take unsigned. Route through macros to our own helpers. */
static inline int kernel_ffs(unsigned int x) { return __builtin_ffs((int)x); }
static inline int kernel_fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
#define ffs(x) kernel_ffs(x)
#define fls(x) kernel_fls(x)
static inline int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static inline unsigned long __ffs(unsigned long w) { return __builtin_ctzl(w); }
static inline unsigned long __fls(unsigned long w) { return BITS_PER_LONG - 1 - __builtin_clzl(w); }
static inline unsigned long ffz(unsigned long w) { return __builtin_ctzl(~w); }
static inline unsigned int hweight8(u8 w) { return __builtin_popcount(w); }
static inline unsigned int hweight16(u16 w) { return __builtin_popcount(w); }
static inline unsigned int hweight32(u32 w) { return __builtin_popcount(w); }
static inline unsigned int hweight64(u64 w) { return __builtin_popcountll(w); }
static inline unsigned long hweight_long(unsigned long w) { return __builtin_popcountl(w); }
static inline u32 ror32(u32 w, unsigned int s) { return (w >> s) | (w << ((32 - s) & 31)); }
static inline u32 rol32(u32 w, unsigned int s) { return (w << s) | (w >> ((32 - s) & 31)); }

unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
unsigned long find_first_bit(const unsigned long *addr, unsigned long size);
unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size);
unsigned long find_last_bit(const unsigned long *addr, unsigned long size);
#define find_next_bit_le find_next_bit
#define find_next_zero_bit_le find_next_zero_bit
#define for_each_set_bit(bit, addr, size) \
	for ((bit) = find_first_bit((addr), (size)); (bit) < (size); (bit) = find_next_bit((addr), (size), (bit) + 1))
#define for_each_clear_bit(bit, addr, size) \
	for ((bit) = find_first_zero_bit((addr), (size)); (bit) < (size); (bit) = find_next_zero_bit((addr), (size), (bit) + 1))

/* Bitmap helpers (linux/bitmap.h subset). */
unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits);
void bitmap_set(unsigned long *map, unsigned int start, unsigned int nbits);
void bitmap_clear(unsigned long *map, unsigned int start, unsigned int nbits);
static inline void bitmap_zero(unsigned long *dst, unsigned int nbits) { __builtin_memset(dst, 0, BITS_TO_LONGS(nbits) * sizeof(long)); }
static inline void bitmap_fill(unsigned long *dst, unsigned int nbits) { __builtin_memset(dst, 0xff, BITS_TO_LONGS(nbits) * sizeof(long)); }

#endif /* _LINUX_BITOPS_H */
