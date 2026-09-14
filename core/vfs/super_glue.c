// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * super_glue.c - mount/unmount entry points for the user-space port.
 *
 * PORT: replaces the kernel's fs_context/get_tree_bdev plumbing around
 * super.c. The boot sector, system files and root inode are loaded by the
 * unchanged super.c (ntfs_fill_super) through the fs_context operations of
 * ntfs_fs_type; this file drives them, applies the port's mount options and
 * implements the dirty/hibernation policy of docs/PORTING.md §6:
 *
 *   The volume is always mounted read-only first (nothing on disk is
 *   touched), its state is inspected, and only a clean volume is then
 *   switched read-write through super.c's own remount path
 *   (ntfs_reconfigure: re-checks the flags, empties the clean journal, marks
 *   quotas out of date). A dirty or hibernated volume, or one whose $LogFile
 *   is not clean, stays read-only with the reason reported in
 *   ntfs_volume_info.ro_reason (or the mount fails with -EROFS when the
 *   caller did not ask for the fallback). The journal is never reset.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/statfs.h>

/*
 * PORT: core/ntfs/inode.h declares an internal struct ntfs_attr and the
 * kernel-signature ntfs_getattr()/ntfs_setattr(); the public ABI in
 * ntfscore.h uses the same names. Rename the internal ones for this file.
 */
#define ntfs_attr ntfs_attr_internal
#define ntfs_getattr ntfs_getattr_internal
#define ntfs_setattr ntfs_setattr_internal
#include "ntfs.h"
#include "logfile.h"
#include "dir.h"
#include "vfs.h"
#undef ntfs_attr
#undef ntfs_getattr
#undef ntfs_setattr
#include "glue.h"
/* The journal tools' volume parser: one MFT record for the label, no mount. */
#include "../logfile/ntfs_image.h"

extern struct file_system_type ntfs_fs_type;	/* super.c (PORT: exported) */

/* ---- one-time module setup ------------------------------------------- */

static pthread_once_t module_once = PTHREAD_ONCE_INIT;
static int module_err;

static void module_init_once(void)
{
	module_err = ntfsport_module_init();	/* super.c init_ntfs_fs() */
}

static int ntfs_glue_module_init(void)
{
	pthread_once(&module_once, module_init_once);
	return module_err;
}

/* ---- logging ---------------------------------------------------------- */

static ntfs_log_fn g_log_fn;
static void *g_log_ctx;

static void ntfs_glue_log_sink(int level, const char *msg, void *ctx)
{
	int lvl;

	(void)ctx;
	switch (level) {
	case PLATFORM_LOG_CRIT:
	case PLATFORM_LOG_ERR:
		lvl = NTFS_LOG_ERROR;
		break;
	case PLATFORM_LOG_WARN:
		lvl = NTFS_LOG_WARN;
		break;
	case PLATFORM_LOG_INFO:
		lvl = NTFS_LOG_INFO;
		break;
	default:
		lvl = NTFS_LOG_DEBUG;
		break;
	}
	if (g_log_fn)
		g_log_fn(lvl, msg, g_log_ctx);
}

void ntfs_set_logger(ntfs_log_fn fn, void *ctx)
{
	g_log_fn = fn;
	g_log_ctx = ctx;
	platform_set_log_sink(fn ? ntfs_glue_log_sink : NULL, NULL);
}

const char *ntfs_core_version(void)
{
	return "ntfsport-0.1 (linux-7.1 fs/ntfs)";
}

/* ---- mount options ---------------------------------------------------- */

static int ntfs_glue_param(struct fs_context *fc, const char *key, const char *value)
{
	struct fs_parameter param = {
		.key = key,
		.type = value ? fs_value_is_string : fs_value_is_flag,
		.string = (char *)value,
		.size = value ? strlen(value) : 0,
	};
	int err = fc->ops->parse_param(fc, &param);

	if (err)
		platform_log(PLATFORM_LOG_ERR, "ntfs: mount option %s%s%s rejected (%d)",
			     key, value ? "=" : "", value ? value : "", err);
	return err;
}

static int ntfs_glue_apply_options(struct fs_context *fc,
				   const struct ntfs_mount_options *opts)
{
	char buf[32];
	int err;

	snprintf(buf, sizeof(buf), "%u", (unsigned int)opts->uid);
	if ((err = ntfs_glue_param(fc, "uid", buf)))
		return err;
	snprintf(buf, sizeof(buf), "%u", (unsigned int)opts->gid);
	if ((err = ntfs_glue_param(fc, "gid", buf)))
		return err;
	snprintf(buf, sizeof(buf), "%o", (unsigned int)opts->fmask & 0777);
	if ((err = ntfs_glue_param(fc, "fmask", buf)))
		return err;
	snprintf(buf, sizeof(buf), "%o", (unsigned int)opts->dmask & 0777);
	if ((err = ntfs_glue_param(fc, "dmask", buf)))
		return err;
	/* Runtime errors switch the volume read-only (NTFS_RO_ERRORS). */
	if ((err = ntfs_glue_param(fc, "errors", "remount-ro")))
		return err;
	if ((err = ntfs_glue_param(fc, opts->flags & NTFS_MOUNT_CASE_SENSITIVE ?
				   "case_sensitive" : "nocase", NULL)))
		return err;
	if ((opts->flags & NTFS_MOUNT_SHOW_SYSTEM) &&
	    (err = ntfs_glue_param(fc, "show_sys_files", NULL)))
		return err;
	if ((opts->flags & NTFS_MOUNT_HIDE_HIDDEN) &&
	    (err = ntfs_glue_param(fc, "nohidden", NULL)))
		return err;
	if (!(opts->flags & NTFS_MOUNT_ALLOW_WINDOWS_ILLEGAL) &&
	    (err = ntfs_glue_param(fc, "windows_names", NULL)))
		return err;
	if ((opts->flags & NTFS_MOUNT_DISCARD) &&
	    (err = ntfs_glue_param(fc, "discard", NULL)))
		return err;
	/*
	 * No pre-allocation slack: writes allocate exactly what they need, so
	 * allocated_size never exceeds data_size rounded up to a cluster and
	 * nothing is left to trim on close (the port has no ->release()).
	 */
	if ((err = ntfs_glue_param(fc, "preallocated_size", "0")))
		return err;
	return 0;
}

/* ---- state checks ----------------------------------------------------- */

/*
 * The old ntfs_is_logfile_clean(): the journal is clean when it is empty, when
 * it is closed, or when it is open with RESTART_VOLUME_IS_CLEAN set.
 *
 * The two conditions are alternatives, not requirements, and getting that wrong
 * costs every modern volume its write access. logfile.h says it plainly: XP and
 * later leave the log *open* even across a clean shutdown, so client_in_use_list
 * is 0 on a healthy disk; what distinguishes clean from dirty there is the flag,
 * which XP+ sets at dismount and clears at mount. Requiring both meant a volume
 * Windows had dismounted cleanly still mounted read-only, while the journal
 * module (core/logfile, ntfs_logfile_is_clean) read the same restart area and
 * correctly called it clean -- the app then showed "Journal: clean" next to
 * "mounted read-only because $LogFile is not clean". Both paths must keep
 * answering the same question the same way.
 */
static bool ntfs_glue_logfile_clean(struct ntfs_volume *vol)
{
	struct restart_page_header *rp = NULL;
	struct restart_area *ra;
	bool clean;

	if (!vol->logfile_ino)
		return false;
	if (NVolLogFileEmpty(vol))
		return true;
	if (!ntfs_check_logfile(vol->logfile_ino, &rp))
		return false;
	if (!rp)
		return NVolLogFileEmpty(vol);
	ra = (struct restart_area *)((u8 *)rp + le16_to_cpu(rp->restart_area_offset));
	clean = ra->client_in_use_list == LOGFILE_NO_CLIENT ||
		(ra->flags & RESTART_VOLUME_IS_CLEAN);
	kvfree(rp);
	return clean;
}

#define NTFS_HIBERFIL_HEADER_SIZE 4096

/*
 * super.c's check_windows_hibernation_status() is static and only reports
 * through NVolErrors(); repeat its test here so the reason can be named.
 * Returns 1 if Windows is hibernated on the volume, 0 if not, -errno.
 */
static int ntfs_glue_hibernated(struct ntfs_volume *vol)
{
	struct inode *vi;
	struct folio *folio;
	const u32 *kaddr, *kend;
	int ret = 1;

	vi = ntfs_vfs_lookup(vol->root_ino, "hiberfil.sys", 12);
	if (IS_ERR(vi))
		return PTR_ERR(vi) == -ENOENT ? 0 : PTR_ERR(vi);
	if (i_size_read(vi) < NTFS_HIBERFIL_HEADER_SIZE)
		goto iput_out;
	folio = read_mapping_folio(vi->i_mapping, 0, NULL);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto iput_out;
	}
	kaddr = folio->data;
	if (*(const __le32 *)kaddr == cpu_to_le32(0x72626968)/*'hibr'*/)
		goto put_out;
	kend = kaddr + NTFS_HIBERFIL_HEADER_SIZE / sizeof(*kaddr);
	for (; kaddr < kend; kaddr++)
		if (*kaddr)
			goto put_out;
	ret = 0;
put_out:
	folio_put(folio);
iput_out:
	iput(vi);
	return ret;
}

/*
 * ntfs_glue_make_rw - switch a clean, read-only mounted volume read-write
 *
 * What super.c's ntfs_reconfigure() does for a rw remount, minus
 * ntfs_mark_quotas_out_of_date(): that upstream function looks up the
 * $Quota index under the wrong name ("$I30") and reads the entry's key as
 * its data, so it always fails; the kernel's own rw *mount* never calls it
 * (only remount does) and neither does ntfs-3g, so a rw mount here matches
 * both. Emptying a clean journal is what every rw mount of this driver does.
 */
static int ntfs_glue_make_rw(struct ntfs_volume *vol)
{
	struct super_block *sb = vol->sb;

	if (NVolErrors(vol)) {
		platform_log(PLATFORM_LOG_WARN, "ntfs: %s: rw refused: errors flagged", sb->s_id);
		return -EROFS;
	}
	if (vol->vol_flags & (VOLUME_IS_DIRTY | VOLUME_MODIFIED_BY_CHKDSK |
			      VOLUME_MUST_MOUNT_RO_MASK)) {
		platform_log(PLATFORM_LOG_WARN, "ntfs: %s: rw refused: vol_flags 0x%x",
			     sb->s_id, (unsigned)le16_to_cpu(vol->vol_flags));
		return -EROFS;
	}
	if (vol->logfile_ino && !ntfs_empty_logfile(vol->logfile_ino)) {
		ntfs_error(sb, "Failed to empty journal LogFile.  Staying read-only.");
		NVolSetErrors(vol);
		return -EROFS;
	}
	sb->s_flags &= ~SB_RDONLY;
	return 0;
}

/* ---- probe ------------------------------------------------------------ */

static int ntfs_glue_check_boot_sector(struct ntfs_bdev *dev,
				       struct ntfs_boot_sector *b,
				       struct ntfs_volume_info *info)
{
	u32 sector_size, spc, cluster_size, mft_record_size;
	int err;

	err = ntfs_bdev_read(dev, b, 0, sizeof(*b));
	if (err)
		return err;
	if (b->oem_id != cpu_to_le64(0x202020205346544eULL))	/* "NTFS    " */
		return -ENXIO;
	sector_size = le16_to_cpu(b->bpb.bytes_per_sector);
	if (sector_size < 256 || sector_size > 4096 || (sector_size & (sector_size - 1)))
		return -ENXIO;
	spc = b->bpb.sectors_per_cluster;
	if (spc > 0x80)
		spc = 1U << (256 - spc);
	if (!spc || (spc & (spc - 1)))
		return -ENXIO;
	cluster_size = sector_size * spc;
	if (b->clusters_per_mft_record < 0)
		mft_record_size = 1U << -b->clusters_per_mft_record;
	else
		mft_record_size = b->clusters_per_mft_record * cluster_size;
	if (!b->number_of_sectors || b->end_of_sector_marker != cpu_to_le16(0xaa55))
		return -ENXIO;
	if (info) {
		info->serial = le64_to_cpu(b->volume_serial_number);
		info->cluster_size = cluster_size;
		info->sector_size = sector_size;
		info->mft_record_size = mft_record_size;
		info->total_clusters = le64_to_cpu(b->number_of_sectors) / spc;
	}
	return 0;
}

static int glue_img_pread(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct ntfs_bdev *d = ctx;
	ssize_t n;

	if (!d->ops || !d->ops->pread)
		return -ENOTSUP;
	n = d->ops->pread(d, buf, len, off);
	if (n < 0)
		return (int)n;
	return (size_t)n == len ? 0 : -EIO;
}

/*
 * Identify a volume without mounting it.
 *
 * ntfs_probe() below mounts and unmounts, which on a 1 TB disk takes a couple
 * of seconds; Disk Arbitration probes twice per mount and loadResource asks
 * again, so plugging a disk in cost four mounts before Finder showed it. The
 * label is the only thing a probe actually needs that the boot sector does not
 * carry, and it lives in $Volume -- one MFT record, read with the same parser
 * the tools use rather than a second NTFS reader.
 */
int ntfs_probe_light(struct ntfs_bdev *dev, struct ntfs_volume_info *info)
{
	struct ntfs_image_io io = { .ctx = dev, .pread = glue_img_pread };
	struct ntfs_boot_sector b;
	struct ntfs_image img;
	uint16_t vol_flags = 0;
	int err;

	if (!dev)
		return -EINVAL;
	err = ntfs_glue_check_boot_sector(dev, &b, info);
	if (err || !info)
		return err;

	/* The label is a convenience: a volume whose $Volume cannot be read is
	 * still a mountable NTFS volume, so report it as one and let the mount
	 * produce the real error. */
	if (ntfs_image_open_io(&img, &io, dev->size_bytes, false) == 0) {
		if (ntfs_image_volume_info(&img, info->label, sizeof(info->label),
					   &info->major_ver, &info->minor_ver,
					   &vol_flags) == 0)
			/* 0x0001 = VOLUME_IS_DIRTY. Spelled out because layout.h
			 * defines it as an le16 constant and vol_flags is already
			 * in host order. */
			info->dirty = (vol_flags & 0x0001u) != 0;
		ntfs_image_close(&img);
	}
	return 0;
}

int ntfs_probe(struct ntfs_bdev *dev, struct ntfs_volume_info *info)
{
	struct ntfs_boot_sector b;
	int err;

	if (info)
		memset(info, 0, sizeof(*info));
	err = ntfs_glue_check_boot_sector(dev, &b, info);
	if (err)
		return err;
	if (info) {
		/* Label, versions and free space need the system files. */
		struct ntfs_mount_options opts = {
			.flags = NTFS_MOUNT_RDONLY | NTFS_MOUNT_RDONLY_FALLBACK,
			.uid = 0, .gid = 0, .fmask = 022, .dmask = 022,
		};
		ntfs_volume_t *vol;

		if (!ntfs_mount(dev, &opts, &vol)) {
			ntfs_volume_get_info(vol, info);
			ntfs_unmount(vol);
		}
	}
	return 0;
}

/* ---- mount / unmount -------------------------------------------------- */

static void ntfs_glue_teardown(struct ntfs_volume_handle *h)
{
	struct super_block *sb = h->sb;

	if (sb) {
		/*
		 * generic_shutdown_super(): flush, drop the root dentry's
		 * reference, then put_super releases the system inodes. With
		 * SB_ACTIVE cleared every iput evicts at once, so nothing is
		 * left for sb_free() to complain about.
		 */
		sync_filesystem(sb);
		sb->s_flags &= ~SB_ACTIVE;
		if (sb->s_root && sb->s_root->d_inode) {
			iput(sb->s_root->d_inode);
			sb->s_root->d_inode = NULL;
		}
		if (sb->s_op && sb->s_op->put_super)
			sb->s_op->put_super(sb);
		sb->s_fs_info = NULL;
		sb_free(sb);
	}
	if (h->fc.ops && h->fc.ops->free)
		h->fc.ops->free(&h->fc);
	kfree(h);
}

int ntfs_mount(struct ntfs_bdev *dev, const struct ntfs_mount_options *opts,
	       ntfs_volume_t **vol_out)
{
	struct ntfs_volume_handle *h;
	struct ntfs_volume *vol;
	struct super_block *sb;
	bool want_rw, made_rw = false;
	int err, hib;

	*vol_out = NULL;
	if (!dev || !opts)
		return -EINVAL;
	err = ntfs_glue_module_init();
	if (err)
		return err;

	h = kzalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->opts = *opts;

	h->fc.bdev = dev;
	h->fc.purpose = FS_CONTEXT_FOR_MOUNT;
	h->fc.sb_flags = SB_RDONLY;	/* always start read-only, see above */
	err = ntfs_fs_type.init_fs_context(&h->fc);
	if (err)
		goto err_free;
	err = ntfs_glue_apply_options(&h->fc, opts);
	if (err)
		goto err_fc;

	/*
	 * Replay the journal before anything mounts, if the user asked. On the
	 * raw device deliberately: replaying under a live mount would write MFT
	 * records and clusters behind this driver's own caches. The result is
	 * only logged -- a refusal leaves the volume exactly as it was, and the
	 * read-only decision below then explains why it is still read-only.
	 */
	if (opts->flags & NTFS_MOUNT_REPLAY_JOURNAL) {
		struct ntfs_logfile_analysis rep;
		int rerr = ntfs_logfile_replay_device(dev, &rep);

		platform_log(rerr ? PLATFORM_LOG_ERR : PLATFORM_LOG_WARN,
			     "ntfs: journal replay requested by the user: %s", rep.message);
		(void)rerr;
	}

	/*
	 * Fresh window on the device's own errors for this attempt, so that a
	 * failed mount can tell "the device refused to read" from "this is not
	 * an NTFS volume" (ntfs_fill_super()'s last lines).
	 */
	dev->io_err = 0;
	err = h->fc.ops->get_tree(&h->fc);
	if (err)
		goto err_fc;
	sb = h->fc.root->d_sb;
	vol = NTFS_SB(sb);
	h->sb = sb;
	h->vol = vol;


	/* Inspect the volume as found on disk. */
	h->dirty = !!(vol->vol_flags & VOLUME_IS_DIRTY);
	h->logfile_clean = ntfs_glue_logfile_clean(vol);
	hib = ntfs_glue_hibernated(vol);
	want_rw = !(opts->flags & NTFS_MOUNT_RDONLY);

	/*
	 * Discard the saved Windows session, if asked and if that is the only
	 * thing in the way. It has to happen before the read-only decision
	 * below, and needs the volume writable -- so the volume is switched
	 * read-write first, which ntfs_glue_make_rw() refuses for a dirty
	 * volume, an unclean journal or one with errors. A hibernated volume
	 * that is otherwise clean is exactly the case this exists for.
	 */
	if (hib > 0 && want_rw && !dev->read_only && h->logfile_clean && !h->dirty &&
	    (opts->flags & NTFS_MOUNT_DISCARD_HIBERNATION)) {
		/*
		 * super.c's check_windows_hibernation_status() sets NV_Errors for
		 * the sole purpose of stopping a hibernated volume going
		 * read-write, so clearing it is what makes the discard possible.
		 * Safe only because every other reason to stay read-only has been
		 * ruled out just above (not dirty, journal clean, device
		 * writable); it is put back if the discard does not complete.
		 */
		bool had_errors = NVolErrors(vol);

		NVolClearErrors(vol);
		err = ntfs_glue_make_rw(vol);
		if (err) {
			if (had_errors)
				NVolSetErrors(vol);
			platform_log(PLATFORM_LOG_WARN,
				     "ntfs: %s: cannot discard the hibernation image; staying read-only (%d)",
				     sb->s_id, err);
		} else {
			made_rw = true;
			err = ntfs_glue_zero_hiberfil(vol);
			if (err) {
				/* Writable now but still hibernated: put the flag
				 * back and let the decision below keep it
				 * read-only, so nothing reaches the volume. */
				if (had_errors)
					NVolSetErrors(vol);
				platform_log(PLATFORM_LOG_ERR,
					     "ntfs: %s: could not discard the hibernation image (%d); staying read-only",
					     sb->s_id, err);
			} else {
				hib = 0;
				h->fc.sb_flags = sb->s_flags;
				platform_log(PLATFORM_LOG_WARN,
					     "ntfs: %s: discarded the saved Windows hibernation image on request; Windows will boot instead of resuming",
					     sb->s_id);
			}
		}
	}
	h->hibernated = hib != 0;

	h->ro_reason = NTFS_RO_NONE;
	if (!want_rw)
		h->ro_reason = NTFS_RO_REQUESTED;
	else if (dev->read_only)
		h->ro_reason = NTFS_RO_DEVICE;
	else if (hib)
		h->ro_reason = NTFS_RO_HIBERNATED;
	else if (h->dirty)
		h->ro_reason = NTFS_RO_DIRTY;
	else if (vol->vol_flags & VOLUME_MUST_MOUNT_RO_MASK)
		h->ro_reason = NTFS_RO_UNSUPPORTED;
	else if (!h->logfile_clean)
		h->ro_reason = NTFS_RO_LOGFILE;
	else if (NVolErrors(vol))
		h->ro_reason = NTFS_RO_ERRORS;

	if (want_rw && h->ro_reason == NTFS_RO_NONE && made_rw) {
		err = 0;                     /* switched above, for the discard */
	} else if (want_rw && h->ro_reason == NTFS_RO_NONE) {
		/* Clean: switch read-write. */
		err = ntfs_glue_make_rw(vol);
		if (!err) {
			h->fc.sb_flags = sb->s_flags;
		} else {
			platform_log(PLATFORM_LOG_WARN,
				     "ntfs: %s: read-write remount refused (%d)",
				     sb->s_id, err);
			h->ro_reason = NTFS_RO_ERRORS;
		}
	}
	if (want_rw && h->ro_reason != NTFS_RO_NONE) {
		static const char *const why[] = {
			[NTFS_RO_DEVICE] = "the device is read-only",
			[NTFS_RO_DIRTY] = "the volume is marked dirty (run chkdsk in Windows)",
			[NTFS_RO_HIBERNATED] = "Windows is hibernated on it (Fast Startup)",
			[NTFS_RO_LOGFILE] = "$LogFile is not clean",
			[NTFS_RO_UNSUPPORTED] = "it has unsupported volume flags",
			[NTFS_RO_ERRORS] = "errors were found while loading it",
		};
		const char *r = h->ro_reason < (int)ARRAY_SIZE(why) && why[h->ro_reason] ?
			why[h->ro_reason] : "of its state";

		if (!(opts->flags & NTFS_MOUNT_RDONLY_FALLBACK)) {
			platform_log(PLATFORM_LOG_ERR,
				     "ntfs: %s: cannot mount read-write because %s",
				     sb->s_id, r);
			err = -EROFS;
			goto err_sb;
		}
		platform_log(PLATFORM_LOG_WARN, "ntfs: %s: mounted read-only because %s",
			     sb->s_id, r);
	}

	platform_log(PLATFORM_LOG_INFO, "ntfs: %s: mounted %s (label \"%s\", NTFS %u.%u)",
		     sb->s_id, sb_rdonly(sb) ? "read-only" : "read-write",
		     vol->volume_label ? (char *)vol->volume_label : "",
		     vol->major_ver, vol->minor_ver);
	*vol_out = h;
	return 0;

err_sb:
	ntfs_glue_teardown(h);
	return err;
err_fc:
	if (h->fc.ops && h->fc.ops->free)
		h->fc.ops->free(&h->fc);
err_free:
	kfree(h);
	return err;
}

int ntfs_unmount(ntfs_volume_t *h)
{
	int err;

	if (!h)
		return -EINVAL;
	err = sb_rdonly(h->sb) ? 0 : sync_filesystem(h->sb);
	ntfs_glue_teardown(h);
	return err;
}

int ntfs_volume_sync(ntfs_volume_t *h)
{
	if (!h)
		return -EINVAL;
	if (sb_rdonly(h->sb))
		return 0;
	return sync_filesystem(h->sb);
}

int ntfs_volume_root(ntfs_volume_t *h, ntfs_inode_t **root_out)
{
	if (!h)
		return -EINVAL;
	ihold(h->vol->root_ino);
	*root_out = (ntfs_inode_t *)h->vol->root_ino;
	return 0;
}

int ntfs_volume_get_info(ntfs_volume_t *h, struct ntfs_volume_info *info)
{
	struct ntfs_volume *vol;
	struct kstatfs st;
	int err;

	if (!h || !info)
		return -EINVAL;
	vol = h->vol;
	memset(info, 0, sizeof(*info));
	if (vol->volume_label)
		strlcpy(info->label, (const char *)vol->volume_label, sizeof(info->label));
	info->serial = vol->serial_no;
	info->major_ver = vol->major_ver;
	info->minor_ver = vol->minor_ver;
	info->cluster_size = vol->cluster_size;
	info->sector_size = vol->sector_size;
	info->mft_record_size = vol->mft_record_size;
	info->total_clusters = vol->nr_clusters;
	memset(&st, 0, sizeof(st));
	err = h->sb->s_op->statfs(h->sb->s_root, &st);
	if (err)
		return err;
	info->free_clusters = st.f_bfree;
	info->total_mft_records = st.f_files;
	info->free_mft_records = st.f_ffree;
	info->read_only = sb_rdonly(h->sb);
	/*
	 * The reason ladder in ntfs_mount() runs once, before anything has had a
	 * chance to fail. A volume that mounted read-write and is read-only now
	 * got there through ntfs_handle_error(), which is what the errors=
	 * remount-ro option does after a metadata write fails, so name that:
	 * reporting NTFS_RO_NONE tells the user their disk went read-only for no
	 * stated reason when the real answer is failing hardware and "copy your
	 * data off now". Latched into the handle because the switch is one-way
	 * for the life of the mount.
	 */
	if (info->read_only && h->ro_reason == NTFS_RO_NONE)
		h->ro_reason = NTFS_RO_ERRORS;
	info->ro_reason = info->read_only ? h->ro_reason : NTFS_RO_NONE;
	info->dirty = h->dirty;
	info->hibernated = h->hibernated;
	info->logfile_clean = h->logfile_clean;
	return 0;
}

int ntfs_volume_set_label(ntfs_volume_t *h, const char *label_utf8)
{
	char *label;
	int err;

	if (!h || !label_utf8)
		return -EINVAL;
	if (sb_rdonly(h->sb))
		return -EROFS;
	label = kstrdup(label_utf8, GFP_KERNEL);
	if (!label)
		return -ENOMEM;
	err = ntfs_write_volume_label(h->vol, label);
	kfree(label);
	return err;
}

int ntfs_logfile_check(ntfs_volume_t *h, bool *clean)
{
	if (!h || !clean)
		return -EINVAL;
	*clean = h->logfile_clean;
	return 0;
}

int ntfs_volume_replay_journal(ntfs_volume_t *h)
{
	(void)h;
	return -ENOSYS;		/* phase 4 (core/logfile) */
}
