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

/*
 * Mark a clean journal clean, instead of erasing all of it.
 *
 * ntfs_empty_logfile() -- upstream's, and what ntfs-3g's "recover" does -- walks
 * the whole $LogFile from VCN 0 writing 0xff. Measured on a real Windows volume,
 * a read-write mount that changes nothing else costs 1420 writes and 5.5 MiB on
 * a 1.7 GB stick; a 500 GB disk carries a 64 MiB journal, so 16384 pages. And
 * the FIRST thing it destroys is the pair of restart pages, which are the only
 * structures that make the log replayable.
 *
 * That is not theoretical. On 2026-09-13 this driver crashed part-way through
 * that loop on a user's disk and left restart pages erased, 152 pages torn, and
 * 16220 intact record pages: a journal full of recoverable work with no way to
 * reach it. See the incident note and docs/LOGFILE.md.
 *
 * Writing two clean restart pages achieves the same thing for the case that
 * matters -- Windows must not replay stale records over what we write -- because
 * a closed, RESTART_VOLUME_IS_CLEAN log with current_lsn at the end of the log
 * is exactly what Windows itself leaves on a clean dismount. ntfs3 and
 * ntfsrecover both write precisely this after replaying. Two pages, written
 * last, instead of thousands written first.
 *
 * Returns -ENOTSUP when the log cannot be parsed well enough to rewrite its
 * restart pages; the caller must fall back to the full erase in that case,
 * which is the one situation the erase is genuinely for.
 */
int ntfs_logfile_mark_clean_device(struct ntfs_bdev *dev)
{
	struct ntfs_image_io io = { .ctx = dev, .pread = bdev_pread,
				    .pwrite = bdev_pwrite, .sync = bdev_sync };
	struct ntfs_log_geometry geom;
	struct ntfs_image img;
	struct ntfs_log_io log_io;
	ntfs_logfile_t *log = NULL;
	int err;

	if (!dev)
		return -EINVAL;
	if (dev->read_only)
		return -EROFS;
	err = ntfs_image_open_io(&img, &io, dev->size_bytes, true);
	if (err)
		return err;
	ntfs_image_log_io(&img, &log_io);
	ntfs_image_geometry(&img, &geom);
	err = ntfs_logfile_open(&log_io, &geom, &log);
	if (err) {
		ntfs_image_close(&img);
		return -ENOTSUP;		/* unparseable: caller erases */
	}
	err = ntfs_logfile_mark_clean(log);
	ntfs_logfile_close(log);
	if (!err && dev->ops && dev->ops->flush)
		(void)dev->ops->flush(dev);	/* best effort; see fskit_flush */
	ntfs_image_close(&img);
	return err;
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
		if (real.flush_failed)
			/* The writes landed; the device would not confirm them. The
			 * journal is left dirty on purpose just below, so this replay
			 * is redone next time or by Windows. Saying "the volume may be
			 * inconsistent" here would be both wrong and frightening. */
			snprintf(out->message, sizeof(out->message),
				 "Replay was written but this device refused the final flush (%d), "
				 "so it cannot be confirmed as saved. The journal has been left "
				 "unchanged, so the same work will be redone next time or by "
				 "Windows. Nothing was lost. A device that never accepts a flush "
				 "is a hardware or enclosure problem, not a filesystem one.", err);
		else
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
