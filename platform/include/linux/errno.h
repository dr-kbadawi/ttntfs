/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ERRNO_H
#define _LINUX_ERRNO_H

#include <errno.h>

/* Linux errno values that macOS lacks or names differently. Values are
 * chosen above every Darwin errno so they never collide. */
#ifndef ERESTARTSYS
#define ERESTARTSYS 512
#endif
#ifndef ERESTARTNOINTR
#define ERESTARTNOINTR 513
#endif
#ifndef EUCLEAN
#define EUCLEAN 517
#endif
#ifndef ENOMEDIUM
#define ENOMEDIUM 523
#endif
#ifndef EMEDIUMTYPE
#define EMEDIUMTYPE 525
#endif
#ifndef ENOTUNIQ
#define ENOTUNIQ 526
#endif
#ifndef ECHRNG
#define ECHRNG 527
#endif
#ifndef ENOKEY
#define ENOKEY 528
#endif
#ifndef ENOPARAM
#define ENOPARAM 519
#endif
#ifndef EBADE
#define EBADE 529
#endif
#ifndef ENOTSUPP
#define ENOTSUPP 524
#endif
#ifndef ENODATA
#define ENODATA 96
#endif
#ifndef ENOATTR
#define ENOATTR 93
#endif

/* Map the Linux-only values back to something Darwin callers understand. */
static inline int platform_errno_to_host(int err)
{
	switch (err) {
	case EUCLEAN: return EIO;
	case ENOMEDIUM: return ENXIO;
	case EMEDIUMTYPE: return ENXIO;
	case ERESTARTSYS: case ERESTARTNOINTR: return EINTR;
	case ENOTUNIQ: return EEXIST;
	case ENOKEY: return EACCES;
	case EBADE: return EIO;
	default: return err;
	}
}

#endif /* _LINUX_ERRNO_H */
