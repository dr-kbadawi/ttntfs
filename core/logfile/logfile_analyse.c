// SPDX-License-Identifier: GPL-2.0
/*
 * logfile_analyse.c - what replaying this volume's $LogFile would do
 *
 * A read-only analysis: the journal is opened, the restart pages are read, and
 * the replay engine runs its analysis, redo and undo passes with every write
 * going to an in-memory overlay. Nothing reaches the disk. The result is the
 * plan the engine built plus the counters describing it.
 *
 * Why this exists while ntfs_logfile_replay() is still refused: the v2.0 log
 * layout, which every dirty volume from a current Windows PC uses, is inferred
 * from Paragon's fslog.c and has never been checked against a journal Windows
 * actually wrote (docs/LOGFILE.md section 5). Applying it could corrupt a
 * volume. Reading it cannot, and what it reports is exactly the evidence needed
 * to decide whether the inference holds -- see the capture list in section 7.
 *
 * It runs on a block device rather than a mounted volume: the analysis is most
 * wanted precisely when the volume mounted read-only, and the geometry it needs
 * (boot sector, the $MFT and $LogFile run lists) is the same thing the tools
 * already parse for an image file.
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

static const char *state_name(int state)
{
	switch (state) {
	case NTFS_LOG_EMPTY:		return "empty (never written)";
	case NTFS_LOG_CLEAN:		return "clean";
	case NTFS_LOG_DIRTY:		return "dirty (replay needed)";
	case NTFS_LOG_CHKDSK:		return "chkdsk in progress";
	case NTFS_LOG_CORRUPT:		return "no usable restart page";
	case NTFS_LOG_UNSUPPORTED:	return "unsupported version";
	default:			return "unrecognised";
	}
}

int ntfs_logfile_analyse(struct ntfs_bdev *dev, struct ntfs_logfile_analysis *out)
{
	struct ntfs_image_io io = { .ctx = dev, .pread = bdev_pread };
	struct ntfs_log_replay_result result;
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

	err = ntfs_image_open_io(&img, &io, dev->size_bytes, false);
	if (err) {
		snprintf(out->message, sizeof(out->message), "%s", img.err[0] ? img.err : "cannot read the volume");
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
	out->supported = info.state != NTFS_LOG_UNSUPPORTED;
	out->log_version_major = info.major_ver;
	out->log_version_minor = info.minor_ver;
	snprintf(out->state, sizeof(out->state), "%s", state_name(info.state));

	if (out->clean) {
		snprintf(out->message, sizeof(out->message),
			 "The journal is %s; no replay is needed.", state_name(info.state));
		err = 0;
		goto out_log;
	}
	if (info.state == NTFS_LOG_CORRUPT) {
		snprintf(out->message, sizeof(out->message),
			 "The journal has no usable restart page, so there is nothing to replay "
			 "and no way to tell what Windows left unfinished. Only Windows can clear it.");
		err = 0;
		goto out_log;
	}
	if (!out->supported) {
		snprintf(out->message, sizeof(out->message),
			 "This journal is version %u.%u, which this driver does not understand. "
			 "Only Windows can clear it.", info.major_ver, info.minor_ver);
		err = 0;
		goto out_log;
	}

	/* Reads only: the apply vtable's write hooks are never called with
	 * dry_run, but the engine still needs to read the volume it would
	 * change. ntfs_image_apply() supplies both; the image is read-only, so
	 * a write would fail loudly rather than slip through. */
	ntfs_image_apply(&img, &apply);
	memset(&result, 0, sizeof(result));
	err = ntfs_logfile_replay(log, &apply, true /* dry run */, &result);
	if (err) {
		snprintf(out->message, sizeof(out->message),
			 "The journal could not be analysed (%d). Only Windows can clear it.", err);
		err = 0;
		goto out_log;
	}

	out->records_analyzed = result.records_analyzed;
	out->records_redone = result.records_redone;
	out->records_undone = result.records_undone;
	out->transactions_active = result.transactions_active;
	out->transactions_committed = result.transactions_committed;
	out->mft_records_written = result.mft_records_written;
	out->clusters_written = result.clusters_written;
	out->needs_chkdsk = result.needs_chkdsk;

	if (result.needs_chkdsk)
		snprintf(out->message, sizeof(out->message),
			 "Replay would not be safe: %u of %u records could not be applied. "
			 "This volume needs chkdsk in Windows.",
			 result.records_analyzed - result.records_redone - result.records_undone,
			 result.records_analyzed);
	else
		snprintf(out->message, sizeof(out->message),
			 "Replay would redo %u and undo %u of %u records, rolling back %u unfinished "
			 "transaction(s) and rewriting %u MFT record(s) and %u cluster(s).",
			 result.records_redone, result.records_undone, result.records_analyzed,
			 result.transactions_active, result.mft_records_written,
			 result.clusters_written);

out_log:
	ntfs_logfile_close(log);
out_image:
	ntfs_image_close(&img);
	return err;
}
