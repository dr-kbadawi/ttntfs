/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_COMPILER_H
#define _LINUX_COMPILER_H

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __always_inline inline __attribute__((always_inline))
#define __maybe_unused __attribute__((unused))
#define __always_unused __attribute__((unused))
#define __must_check __attribute__((warn_unused_result))
#define __printf(a, b) __attribute__((format(printf, a, b)))
#undef __cold
#define __cold __attribute__((cold))
#undef __pure
#define __pure __attribute__((pure))
#define __noreturn __attribute__((noreturn))
#undef __used
#define __used __attribute__((used))
#define __init
#define __exit
#define __initdata
#define __ro_after_init
#define __read_mostly
#define __user
#define __kernel
#define __iomem
#define __force
#define __bitwise
#define __rcu
#define __percpu
#define __nocast
#undef __counted_by
#define __counted_by(m)
#define __free(f) __attribute__((cleanup(__cleanup_##f)))
#define noinline __attribute__((noinline))
#undef __weak
#define __weak __attribute__((weak))
#define fallthrough __attribute__((fallthrough))
#define unreachable() __builtin_unreachable()
#define __same_type(a, b) __builtin_types_compatible_p(__typeof__(a), __typeof__(b))
#define __must_be_array(a) 0
#define __stringify_1(x...) #x
#define __stringify(x...) __stringify_1(x)
#define ACCESS_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#ifndef __ASSEMBLY__
#define static_assert(expr, ...) _Static_assert(expr, #expr)
#endif
#define BUILD_BUG_ON(c) _Static_assert(!(c), #c)
#define BUILD_BUG_ON_MSG(c, m) _Static_assert(!(c), m)
#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int : (-!!(e)); }))
#define BUILD_BUG() do { } while (0)
#define KBUILD_MODNAME "ntfs"
#define __KERNEL__ 1

#endif /* _LINUX_COMPILER_H */
