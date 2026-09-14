/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ntfs_xattr_flags.h - the one definition of the ntfs_setxattr() flag bits.
 *
 * Part of the public contract; reached by including ntfscore.h, which every
 * consumer already does. It exists as its own header only so that
 * platform/include/linux/xattr.h can derive XATTR_CREATE / XATTR_REPLACE from
 * it: including all of ntfscore.h there is not possible, because the ported
 * code in core/ntfs declares its own struct ntfs_attr and its own
 * ntfs_get/setattr and ntfs_get/setxattr, which collide with the public names
 * (see the rename block at the top of core/vfs/api.c). This header therefore
 * declares nothing but two macros -- no types, no functions, no includes -- so
 * it can be pulled in from either side.
 *
 * These are the LINUX numbers. The core is a port of Linux fs/ntfs, and the
 * ported code (core/ntfs/ea.c, core/vfs/api.c) tests the caller's flags against
 * platform/include/linux/xattr.h, so the bits crossing the public ABI have to
 * be the ones that code already means.
 *
 * They are deliberately NOT Darwin's, where <sys/xattr.h> has XATTR_CREATE 2
 * and XATTR_REPLACE 4 for the same two names. A caller that reaches for the
 * system header gets silently wrong behaviour rather than an error: Darwin's
 * XATTR_CREATE (2) arrives as REPLACE and fails with -ENOATTR, and Darwin's
 * XATTR_REPLACE (4) arrives as no flags at all and creates the xattr the caller
 * wanted to require already existed. Changing these numbers to match Darwin
 * would silently change what every existing on-disk caller means, so they stay
 * Linux's and callers translate.
 *
 * core/tests/test_names.c pins both the values and the behaviour.
 */
#ifndef NTFS_XATTR_FLAGS_H
#define NTFS_XATTR_FLAGS_H

#define NTFS_XATTR_CREATE	0x1	/* fail with -EEXIST if it exists */
#define NTFS_XATTR_REPLACE	0x2	/* fail with -ENOATTR if it does not */

#endif /* NTFS_XATTR_FLAGS_H */
