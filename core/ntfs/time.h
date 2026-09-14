/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NTFS time conversion functions.
 *
 * Copyright (c) 2001-2005 Anton Altaparmakov
 */

#ifndef _LINUX_NTFS_TIME_H
#define _LINUX_NTFS_TIME_H

#include <linux/time.h>
#include <asm/div64.h>		/* For do_div(). */

#define NTFS_TIME_OFFSET ((s64)(369 * 365 + 89) * 24 * 3600)

/*
 * utc2ntfs - convert Linux UTC time to NTFS time
 * @ts:		Linux UTC time to convert to NTFS time
 *
 * Convert the Linux UTC time @ts to its corresponding NTFS time and return
 * that in little endian format.
 *
 * Linux stores time in a struct timespec64 consisting of a time64_t tv_sec
 * and a long tv_nsec where tv_sec is the number of 1-second intervals since
 * 1st January 1970, 00:00:00 UTC and tv_nsec is the number of 1-nano-second
 * intervals since the value of tv_sec.
 *
 * NTFS uses Microsoft's standard time format which is stored in a s64 and is
 * measured as the number of 100-nano-second intervals since 1st January 1601,
 * 00:00:00 UTC.
 */
static inline __le64 utc2ntfs(const struct timespec64 ts)
{
	/*
	 * Convert the seconds to 100ns intervals, add the nano-seconds
	 * converted to 100ns intervals, and then add the NTFS time offset.
	 */
	return cpu_to_le64((u64)(ts.tv_sec + NTFS_TIME_OFFSET) * 10000000 +
			ts.tv_nsec / 100);
}

/*
 * get_current_ntfs_time - get the current time in little endian NTFS format
 *
 * Get the current time from the Linux kernel, convert it to its corresponding
 * NTFS time and return that in little endian format.
 */
static inline __le64 get_current_ntfs_time(void)
{
	struct timespec64 ts;

	ktime_get_coarse_real_ts64(&ts);
	return utc2ntfs(ts);
}

/*
 * ntfs2utc - convert NTFS time to Linux time
 * @time:	NTFS time (little endian) to convert to Linux UTC
 *
 * Convert the little endian NTFS time @time to its corresponding Linux UTC
 * time and return that in cpu format.
 *
 * Linux stores time in a struct timespec64 consisting of a time64_t tv_sec
 * and a long tv_nsec where tv_sec is the number of 1-second intervals since
 * 1st January 1970, 00:00:00 UTC and tv_nsec is the number of 1-nano-second
 * intervals since the value of tv_sec.
 *
 * NTFS uses Microsoft's standard time format which is stored in a s64 and is
 * measured as the number of 100 nano-second intervals since 1st January 1601,
 * 00:00:00 UTC.
 */
static inline struct timespec64 ntfs2utc(const __le64 time)
{
	struct timespec64 ts;
	s32 t32;

	/* Subtract the NTFS time offset. */
	s64 t = le64_to_cpu(time) - NTFS_TIME_OFFSET * 10000000;
	/*
	 * Convert the time to 1-second intervals and the remainder to
	 * 1-nano-second intervals.
	 */
	ts.tv_sec = div_s64_rem(t, 10000000, &t32);
	ts.tv_nsec = t32 * 100;
	/*
	 * PORT: normalise tv_nsec into [0, 10^9).
	 *
	 * div_s64_rem() truncates toward zero, so the remainder carries the
	 * sign of the dividend: for any instant before 1970 that is not on a
	 * whole second, tv_nsec comes back negative. NTFS tick 1 gives
	 * tv_sec = -11644473599, tv_nsec = -999999900.
	 *
	 * Upstream leaves it that way and is not wrong to: in the kernel the
	 * value only ever goes back through utc2ntfs(), which adds tv_sec and
	 * tv_nsec/100 and so is exact for either representation. Here the same
	 * struct is copied field for field into struct ntfs_timespec and handed
	 * over the public ABI to FSKit, and POSIX requires 0 <= tv_nsec < 10^9;
	 * a negative nsec there reads as a timestamp a second in the future.
	 * Files Windows stamped before 1970 (restored archives, deliberately
	 * backdated files) are what reach this.
	 *
	 * Borrowing a second leaves the instant unchanged, so the round trip
	 * through utc2ntfs() still returns the original tick count.
	 */
	if (ts.tv_nsec < 0) {
		ts.tv_nsec += 1000000000L;
		ts.tv_sec--;
	}
	return ts;
}
#endif /* _LINUX_NTFS_TIME_H */
