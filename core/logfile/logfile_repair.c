// SPDX-License-Identifier: GPL-2.0
/*
 * logfile_repair.c - replay the journal onto the volume
 *
 * THIS WRITES TO THE FILESYSTEM, using an on-disk layout that has not been
 * verified against a journal Windows actually wrote. docs/LOGFILE.md section 5
 * lists what is inferred: for v1.1 everything below the restart area, and for
 * v2.0 the page layout as well, taken from a single source. Section 7 lists the
 * captures that would turn that into knowledge. Until they exist, a wrong guess
 * here does not fail cleanly -- it restructures metadata and the damage surfaces
 * later. It is therefore never automatic: the caller passes
 * NTFS_MOUNT_REPLAY_JOURNAL only on an explicit, per-volume instruction from
 * the user, and the app that asks says plainly what is being risked.
 *
 * It runs on the raw device *before* the volume is mounted, not on a live
 * mount. Replaying under a mounted volume would write MFT records and clusters
 * behind the core's own caches, which is a second, entirely avoidable way to
 * corrupt a filesystem.
 *
 * Refusals, all of which leave the volume untouched:
 *   - any version other than 1.1 or 2.0
 *   - a journal with no usable restart page, or already clean
 *   - a dry run that reports an error or needs_chkdsk: if the engine cannot
 *     account for every record, it does not get to apply any of them
 *   - a read-only device
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "ntfscore.h"
#include "ntfs_logfile.h"
#include "ntfs_image.h"
#include "ntfsport/bdev.h"

static int bdev_pread(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct ntfs_bdev *dev = ctx;
	ssize_t n;

	if (!dev->ops || !dev->ops->pread)
		return -ENOTSUP;
	n = dev->ops->pread(dev, buf, len, off);
	if (n < 0)
		return (int)n;
	return (size_t)n == len ? 0 : -EIO;
}

static int bdev_pwrite(void *ctx, uint64_t off, const void *buf, size_t len)
{
	struct ntfs_bdev *dev = ctx;
	ssize_t n;

	if (!dev->ops || !dev->ops->pwrite)
		return -ENOTSUP;
	n = dev->ops->pwrite(dev, buf, len, off);
	if (n < 0)
		return (int)n;
	return (size_t)n == len ? 0 : -EIO;
}

static int bdev_sync(void *ctx)
{
	struct ntfs_bdev *dev = ctx;

	if (!dev->ops || !dev->ops->flush)
		return 0;
	return dev->ops->flush(dev);
}

/* Only the versions the caller asked for, and only those. */
static bool version_allowed(const struct ntfs_logfile_info *info)
{
	if (info->major_ver == 1 && (info->minor_ver == 0 || info->minor_ver == 1))
		return true;
	return info->major_ver == 2 && info->minor_ver == 0;
}

int ntfs_logfile_replay_device(struct ntfs_bdev *dev, struct ntfs_logfile_analysis *out)
{
	struct ntfs_image_io io = { .ctx = dev, .pread = bdev_pread,
				    .pwrite = bdev_pwrite, .sync = bdev_sync };
	struct ntfs_log_replay_result dry, real;
	struct ntfs_logfile_info info;
	struct ntfs_log_geometry geom;
	struct ntfs_image img;
	struct ntfs_log_apply apply;
	struct ntfs_log_io log_io;
	ntfs_logfile_t *log = NULL;
	int err;

	if (!dev || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (dev->read_only) {
		snprintf(out->message, sizeof(out->message), "The device is read-only.");
		return -EROFS;
	}

	err = ntfs_image_open_io(&img, &io, dev->size_bytes, true);
	if (err) {
		snprintf(out->message, sizeof(out->message), "%s",
			 img.err[0] ? img.err : "cannot read the volume");
		return err;
	}
	ntfs_image_log_io(&img, &log_io);
	ntfs_image_geometry(&img, &geom);
	err = ntfs_logfile_open(&log_io, &geom, &log);
	if (err) {
		snprintf(out->message, sizeof(out->message), "cannot read $LogFile");
		goto out_image;
	}

	out->log_present = true;
	ntfs_logfile_get_info(log, &info);
	out->clean = ntfs_logfile_is_clean(log);
	out->log_version_major = info.major_ver;
	out->log_version_minor = info.minor_ver;
	out->supported = version_allowed(&info);
	snprintf(out->state, sizeof(out->state), "v%u.%u", info.major_ver, info.minor_ver);

	if (out->clean) {
		snprintf(out->message, sizeof(out->message), "The journal is already clean.");
		err = 0;
		goto out_log;
	}
	if (!out->supported) {
		snprintf(out->message, sizeof(out->message),
			 "Journal version %u.%u is not one this driver will replay (only 1.0, 1.1 and 2.0).",
			 info.major_ver, info.minor_ver);
		err = -ENOTSUP;
		goto out_log;
	}

	/* Dry run first, always. The engine must be able to account for every
	 * record before it is allowed to write one. */
	ntfs_image_apply(&img, &apply);
	memset(&dry, 0, sizeof(dry));
	err = ntfs_logfile_replay(log, &apply, true, &dry);
	if (err) {
		snprintf(out->message, sizeof(out->message),
			 "The journal could not be analysed (%d); nothing was written.", err);
		goto out_log;
	}
	if (dry.needs_chkdsk) {
		snprintf(out->message, sizeof(out->message),
			 "Replay would not be safe: the journal contains records this driver "
			 "cannot apply. Nothing was written; run chkdsk in Windows.");
		err = -EINVAL;
		goto out_log;
	}

	memset(&real, 0, sizeof(real));
	err = ntfs_logfile_replay(log, &apply, false, &real);
	if (err) {
		snprintf(out->message, sizeof(out->message),
			 "Replay failed part-way (%d). The volume may be inconsistent; "
			 "run chkdsk in Windows before writing to it.", err);
		goto out_log;
	}

	out->records_analyzed = real.records_analyzed;
	out->records_redone = real.records_redone;
	out->records_undone = real.records_undone;
	out->transactions_active = real.transactions_active;
	out->transactions_committed = real.transactions_committed;
	out->mft_records_written = real.mft_records_written;
	out->clusters_written = real.clusters_written;

	/* Leave the journal as Windows would after a clean dismount, so neither
	 * we nor Windows replays these records a second time. */
	err = ntfs_logfile_mark_clean(log);
	if (err) {
		snprintf(out->message, sizeof(out->message),
			 "Replay applied %u record(s) but the journal could not be marked clean (%d). "
			 "Run chkdsk in Windows.", real.records_redone + real.records_undone, err);
		goto out_log;
	}
	out->clean = true;
	snprintf(out->message, sizeof(out->message),
		 "Replayed %u record(s): %u redone, %u undone, %u transaction(s) rolled back, "
		 "%u MFT record(s) and %u cluster(s) rewritten.",
		 real.records_analyzed, real.records_redone, real.records_undone,
		 real.transactions_active, real.mft_records_written, real.clusters_written);

out_log:
	ntfs_logfile_close(log);
out_image:
	ntfs_image_close(&img);
	return err;
}
