/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * ntfscore.h - public C ABI of the NTFS core. CONTRACT HEADER.
 *
 * This is the only header the FSKit extension (Swift) and tools/ntfscli
 * include. It exposes an inode-oriented API shaped like FSKit's FSVolume
 * operations, so the Swift layer is a thin translation.
 *
 * Conventions:
 *  - All functions return 0 on success or a negative errno.
 *  - Names are UTF-8, NUL-terminated, at most NTFS_MAX_NAME_BYTES.
 *  - Handles are reference counted; every *_get must be matched by a *_put.
 *  - Thread safety: all calls may be made concurrently from any thread.
 *    Ordering guarantees follow POSIX semantics per inode.
 *  - Implemented by core/vfs/. Implementation must not expose any kernel
 *    compat types through this header.
 */
#ifndef NTFSCORE_H
#define NTFSCORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

/* NTFS_XATTR_CREATE / NTFS_XATTR_REPLACE for ntfs_setxattr(). Its own header
 * because platform/include/linux/xattr.h derives from it and cannot include
 * this one; see the file itself for why the values are Linux's, not Darwin's. */
#include <ntfs_xattr_flags.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NTFS_MAX_NAME_BYTES 1020	/* 255 UTF-16 units, worst-case UTF-8 */

typedef struct ntfs_volume_handle ntfs_volume_t;
typedef struct ntfs_inode_handle ntfs_inode_t;
struct ntfs_bdev;

/* ---- Mount ------------------------------------------------------------ */

enum ntfs_mount_flags {
	NTFS_MOUNT_RDONLY		= 1u << 0,
	/* Fall back to read-only instead of failing when the volume is dirty
	 * or hibernated. The reason is reported in ntfs_volume_info.ro_reason. */
	NTFS_MOUNT_RDONLY_FALLBACK	= 1u << 1,
	/* Treat names case-sensitively (default: case-insensitive). */
	NTFS_MOUNT_CASE_SENSITIVE	= 1u << 2,
	/* Show system files ($MFT, $Bitmap, ...) in the root directory. */
	NTFS_MOUNT_SHOW_SYSTEM		= 1u << 3,
	/* Show files with the HIDDEN attribute. Default: shown (Finder has
	 * its own notion of hidden). */
	NTFS_MOUNT_HIDE_HIDDEN		= 1u << 4,
	/* Allow names Windows rejects (see docs/PORTING.md §6). Default: reject. */
	NTFS_MOUNT_ALLOW_WINDOWS_ILLEGAL = 1u << 5,
	/* Issue discards for freed clusters. */
	NTFS_MOUNT_DISCARD		= 1u << 6,
	/* Replay the journal before mounting, so a volume Windows left dirty can
	 * be mounted read-write. Writes to the filesystem using an unverified
	 * on-disk layout: only ever set from an explicit user instruction. See
	 * ntfs_logfile_replay_device(). */
	NTFS_MOUNT_REPLAY_JOURNAL	= 1u << 8,
	/* Zero the saved Windows hibernation image (hiberfil.sys) so the volume
	 * can be mounted read-write. Windows will do a full boot instead of
	 * resuming, losing whatever was open in the suspended session; the
	 * filesystem itself is untouched. Only honoured when the volume is
	 * otherwise clean -- a dirty volume or an unclean journal still refuses.
	 * Destructive to the user's Windows session: ask first. */
	NTFS_MOUNT_DISCARD_HIBERNATION	= 1u << 7,
	/* Write every symlink with the WSL reparse tag (IO_REPARSE_TAG_LX_SYMLINK)
	 * instead of choosing per link. The default writes the native Windows tag
	 * whenever the target can be expressed in it, which is what Windows, WSL,
	 * Linux ntfs3 and ntfs-3g all follow; the WSL tag is refused by Windows and
	 * unreadable by ntfs3. Set this only for a volume that must match what
	 * ntfsplus or ntfsprogs-plus would write. */
	NTFS_MOUNT_WSL_SYMLINKS		= 1u << 9,
	/* Mark names beginning with '.' as Windows-hidden when they are created,
	 * so .DS_Store, ._resource forks, .Spotlight-V100 and .fseventsd do not
	 * litter Explorer on a disk shared with Windows. Only affects files this
	 * driver creates; existing files are left alone. */
	NTFS_MOUNT_HIDE_DOT_FILES	= 1u << 10,
};

enum ntfs_ro_reason {
	NTFS_RO_NONE = 0,
	NTFS_RO_REQUESTED,		/* caller asked for read-only */
	NTFS_RO_DEVICE,			/* device is read-only */
	NTFS_RO_DIRTY,			/* volume dirty flag set; needs chkdsk or journal replay */
	NTFS_RO_HIBERNATED,		/* Windows hibernated / Fast Startup */
	NTFS_RO_LOGFILE,		/* $LogFile not clean and could not be replayed */
	NTFS_RO_UNSUPPORTED,		/* feature we cannot write safely (e.g. old version) */
	NTFS_RO_ERRORS,			/* errors detected while mounted; switched to ro */
};

struct ntfs_mount_options {
	uint32_t flags;			/* enum ntfs_mount_flags */
	uid_t uid;			/* owner reported for all files */
	gid_t gid;
	uint16_t fmask;			/* permission bits to clear on files (octal) */
	uint16_t dmask;			/* ... on directories */
};

struct ntfs_volume_info {
	char label[256];		/* UTF-8 */
	uint64_t serial;
	uint8_t major_ver, minor_ver;
	uint32_t cluster_size;
	uint32_t sector_size;
	uint32_t mft_record_size;
	uint64_t total_clusters;
	uint64_t free_clusters;
	uint64_t total_mft_records;
	uint64_t free_mft_records;
	bool read_only;
	int ro_reason;			/* enum ntfs_ro_reason */
	bool dirty;			/* volume dirty flag as found on disk */
	bool hibernated;
	bool logfile_clean;
};

/* Probe: returns 0 if @dev holds an NTFS volume, -ENXIO otherwise, or
 * another negative errno on I/O failure. Fills @info label/serial/sizes
 * when non-NULL.
 *
 * This one mounts the volume read-only and unmounts it again, because
 * free space, the hibernation state and the journal state are only knowable
 * that way. On a 1 TB disk it costs a couple of seconds, so use it only where
 * those fields are actually wanted -- a check task, not a probe. */
int ntfs_probe(struct ntfs_bdev *dev, struct ntfs_volume_info *info);

/* Probe without mounting: boot sector plus $Volume (one MFT record). Fills
 * label, serial, geometry, version and @dirty. Leaves free space at zero and
 * @hibernated / @logfile_clean false, since both need the full volume. This is
 * what identifying a volume needs, and Disk Arbitration asks twice per mount. */
int ntfs_probe_light(struct ntfs_bdev *dev, struct ntfs_volume_info *info);

int ntfs_mount(struct ntfs_bdev *dev, const struct ntfs_mount_options *opts,
	       ntfs_volume_t **vol_out);
/* Flush everything and release. The bdev is not closed. */
int ntfs_unmount(ntfs_volume_t *vol);
int ntfs_volume_get_info(ntfs_volume_t *vol, struct ntfs_volume_info *info);
/* Flush all dirty metadata and data, then flush the device cache. */
int ntfs_volume_sync(ntfs_volume_t *vol);
/* Root directory inode, referenced. */
int ntfs_volume_root(ntfs_volume_t *vol, ntfs_inode_t **root_out);
int ntfs_volume_set_label(ntfs_volume_t *vol, const char *label_utf8);

/* ---- Inodes / attributes --------------------------------------------- */

enum ntfs_item_type {
	NTFS_ITEM_FILE = 1,
	NTFS_ITEM_DIR,
	NTFS_ITEM_SYMLINK,		/* reparse point we present as a symlink */
	NTFS_ITEM_OTHER,		/* reparse point we cannot interpret; shown as file */
};

struct ntfs_timespec { int64_t sec; int32_t nsec; };

struct ntfs_attr {
	uint64_t inode_no;		/* MFT record number */
	uint32_t generation;		/* MFT sequence number */
	int type;			/* enum ntfs_item_type */
	uint32_t mode;			/* POSIX mode bits incl. type */
	uint32_t nlink;
	uid_t uid;
	gid_t gid;
	uint64_t size;			/* data size of the unnamed stream */
	uint64_t alloc_size;		/* allocated bytes on disk */
	struct ntfs_timespec atime, mtime, ctime, crtime;
	uint32_t file_attributes;	/* raw NTFS FILE_ATTR_* bits */
	uint32_t reparse_tag;		/* 0 if not a reparse point */
	bool compressed, sparse, encrypted, has_ads;
};

/* Fields for ntfs_setattr(). */
enum ntfs_setattr_valid {
	NTFS_SETATTR_MODE	= 1u << 0,
	NTFS_SETATTR_UID	= 1u << 1,
	NTFS_SETATTR_GID	= 1u << 2,
	NTFS_SETATTR_SIZE	= 1u << 3,
	NTFS_SETATTR_ATIME	= 1u << 4,
	NTFS_SETATTR_MTIME	= 1u << 5,
	NTFS_SETATTR_CTIME	= 1u << 6,
	NTFS_SETATTR_CRTIME	= 1u << 7,
	NTFS_SETATTR_FLAGS	= 1u << 8,	/* file_attributes: hidden/system/readonly/archive */
};

int ntfs_inode_get(ntfs_volume_t *vol, uint64_t inode_no, ntfs_inode_t **out);
void ntfs_inode_ref(ntfs_inode_t *ni);
void ntfs_inode_put(ntfs_inode_t *ni);
uint64_t ntfs_inode_number(ntfs_inode_t *ni);
int ntfs_getattr(ntfs_inode_t *ni, struct ntfs_attr *attr);
int ntfs_setattr(ntfs_inode_t *ni, const struct ntfs_attr *attr, uint32_t valid);

/* ---- Namespace -------------------------------------------------------- */

int ntfs_lookup(ntfs_inode_t *dir, const char *name, ntfs_inode_t **out);
int ntfs_create(ntfs_inode_t *dir, const char *name, uint32_t mode,
		ntfs_inode_t **out);
int ntfs_mkdir(ntfs_inode_t *dir, const char *name, uint32_t mode,
	       ntfs_inode_t **out);
int ntfs_symlink(ntfs_inode_t *dir, const char *name, const char *target,
		 ntfs_inode_t **out);
int ntfs_readlink(ntfs_inode_t *ni, char *buf, size_t bufsize, size_t *len_out);

/*
 * Maximum number of names one file may have, the original included.
 *
 * Not an on-disk limit: link_count in the MFT record is a __le16 and the
 * FILE_NAME attributes spill into extents through $ATTRIBUTE_LIST, so the
 * structure allows 65535. This is Windows' limit. Microsoft documents it on
 * CreateHardLinkW as "the maximum number of hard links that can be created
 * with this function is 1023 per file", i.e. 1023 on top of the name the file
 * was created with, so 1024 names in total; past it Windows fails with
 * ERROR_TOO_MANY_LINKS. We enforce it because a volume this driver writes has
 * to stay usable on Windows, which is the whole point of the driver.
 *
 * Upstream Linux fs/ntfs enforces nothing here. fs/ntfs3 caps at 4000 (raised
 * from 1024 only to pass xfstests generic/041), and ntfs-3g has no cap at all;
 * both can therefore build volumes Windows cannot extend.
 */
#define NTFS_LINK_MAX 1024

/* Returns -EMLINK if @ni already has NTFS_LINK_MAX names. */
int ntfs_link(ntfs_inode_t *ni, ntfs_inode_t *dir, const char *name);
int ntfs_unlink(ntfs_inode_t *dir, const char *name);
int ntfs_rmdir(ntfs_inode_t *dir, const char *name);
/* POSIX rename; replaces an existing target if present and compatible. */
int ntfs_rename(ntfs_inode_t *old_dir, const char *old_name,
		ntfs_inode_t *new_dir, const char *new_name);

/* Directory enumeration. @cookie is 0 for the first call; on return it
 * holds the position for the next call; *eof is set when done. Callback
 * returns 0 to continue or nonzero to stop (e.g. buffer full: the entry
 * for which it returned nonzero is NOT consumed and will be returned again). */
struct ntfs_dirent {
	uint64_t inode_no;
	int type;			/* enum ntfs_item_type */
	const char *name;		/* UTF-8, not NUL-terminated */
	size_t name_len;
	uint64_t cookie;		/* position of this entry */
	/* Filled when readdir was called with want_attr: */
	bool has_attr;
	struct ntfs_attr attr;
};
typedef int (*ntfs_readdir_cb)(const struct ntfs_dirent *ent, void *ctx);
int ntfs_readdir(ntfs_inode_t *dir, uint64_t *cookie, bool want_attr,
		 ntfs_readdir_cb cb, void *ctx, bool *eof);

/* ---- Data ------------------------------------------------------------- */

/* Read/write the unnamed data stream. Return bytes transferred (short at
 * EOF for reads) or negative errno. Large, aligned I/O goes straight
 * from the run list to the device (docs/PORTING.md §3 rule 3).
 *
 * Directories give -EISDIR and symlinks -EINVAL. A symlink keeps its target
 * in the reparse point, not in $DATA, and ntfs_getattr() reports the target
 * length as the size, so the $DATA stream behind a symlink is not addressable
 * through this ABI: writing it would leave the size the caller sees
 * disagreeing with the bytes the stream holds. POSIX has no write-to-a-symlink
 * operation either -- a write goes to the target, and the layer above resolves
 * it. The same refusal applies to ntfs_truncate(), ntfs_fallocate() and
 * NTFS_SETATTR_SIZE; mode and timestamps on a symlink are still settable. */
ssize_t ntfs_read(ntfs_inode_t *ni, void *buf, size_t count, uint64_t offset);
ssize_t ntfs_write(ntfs_inode_t *ni, const void *buf, size_t count, uint64_t offset);
int ntfs_truncate(ntfs_inode_t *ni, uint64_t size);
/* Preallocate [offset, offset+len). keep_size: do not change i_size. */
int ntfs_fallocate(ntfs_inode_t *ni, uint64_t offset, uint64_t len, bool keep_size);
/* Flush this inode's data and metadata to the device. */
int ntfs_fsync(ntfs_inode_t *ni, bool datasync);
/* Report the next data/hole boundary at or after offset (SEEK_DATA/SEEK_HOLE). */
int ntfs_seek_data_hole(ntfs_inode_t *ni, uint64_t offset, bool want_hole,
			uint64_t *result);

/* ---- Extended attributes / alternate data streams --------------------- */
/*
 * macOS xattrs are stored as NTFS alternate data streams named after the
 * xattr (stream "com.apple.FinderInfo" etc.). listxattr enumerates the
 * inode's named $DATA streams. Names are UTF-8.
 */
/*
 * ntfs_setxattr()'s flags are NTFS_XATTR_CREATE / NTFS_XATTR_REPLACE, defined
 * in ntfs_xattr_flags.h (included above) -- THE CONTRACT DEFINES THESE. Never
 * substitute the system header's XATTR_CREATE / XATTR_REPLACE: on Darwin those
 * are different numbers for the same two names, and the mistake is silent.
 * That header is the only place the values are written down;
 * platform/include/linux/xattr.h derives from it rather than keeping a copy.
 */
int ntfs_getxattr(ntfs_inode_t *ni, const char *name, void *buf, size_t size,
		  size_t *len_out);	/* buf==NULL: size query */
/* flags: 0, NTFS_XATTR_CREATE or NTFS_XATTR_REPLACE (see above). */
int ntfs_setxattr(ntfs_inode_t *ni, const char *name, const void *buf,
		  size_t size, int flags);
int ntfs_removexattr(ntfs_inode_t *ni, const char *name);
/* Names concatenated, each NUL-terminated. buf==NULL: size query. */
int ntfs_listxattr(ntfs_inode_t *ni, char *buf, size_t size, size_t *len_out);

/* ---- Maintenance ------------------------------------------------------ */

/* Validate a name against the mount's policy (Windows-illegal characters,
 * length, reserved names). Returns 0 or -EINVAL / -ENAMETOOLONG. */
int ntfs_validate_name(ntfs_volume_t *vol, const char *name);

/* Journal. Replay is phase 4; until then check reports state only. */
int ntfs_logfile_check(ntfs_volume_t *vol, bool *clean);
int ntfs_volume_replay_journal(ntfs_volume_t *vol);

/*
 * Read-only journal analysis: what replaying $LogFile would do, without
 * touching the volume. Works on a device, mounted or not, and needs only read
 * access -- the replay engine's every write goes to an in-memory overlay.
 *
 * This is offered while ntfs_logfile_replay() is still refused because the two
 * carry completely different risk: the v2.0 log layout used by every dirty
 * volume from a current Windows PC is inferred rather than observed
 * (docs/LOGFILE.md section 5), so applying it could corrupt a filesystem, while
 * reading it cannot -- and what this reports is the evidence needed to decide
 * whether that inference holds.
 *
 * Returns 0 when the analysis ran, filling @out (including the case "this
 * journal cannot be analysed", explained in @message); negative errno if the
 * volume could not be read at all.
 */
struct ntfs_logfile_analysis {
	bool log_present;
	bool clean;			/* no replay needed */
	bool supported;			/* log version understood */
	bool needs_chkdsk;		/* a record could not be applied safely */
	uint16_t log_version_major, log_version_minor;
	char state[32];			/* empty / clean / dirty / chkdsk / ... */
	uint32_t records_analyzed, records_redone, records_undone;
	uint32_t transactions_active, transactions_committed;
	uint32_t mft_records_written, clusters_written;
	char message[256];		/* one sentence for the user */
};
int ntfs_logfile_analyse(struct ntfs_bdev *dev, struct ntfs_logfile_analysis *out);

/*
 * Replay the journal onto @dev. WRITES TO THE FILESYSTEM.
 *
 * Only for journal versions 1.0, 1.1 and 2.0, only when a dry run accounts for
 * every record, and never on a read-only device. Marks the journal clean
 * afterwards so neither this driver nor Windows replays it twice.
 *
 * The on-disk layout this depends on has not been verified against a journal
 * Windows wrote (docs/LOGFILE.md section 5), so a wrong guess does not fail
 * cleanly: it restructures metadata. Call this only on an explicit per-volume
 * instruction from the user, and only before the volume is mounted -- replaying
 * under a live mount writes behind the core's caches.
 *
 * Returns 0 when the journal was replayed and marked clean, or a negative errno
 * with @out->message explaining what was refused and whether anything was
 * written.
 */
int ntfs_logfile_replay_device(struct ntfs_bdev *dev, struct ntfs_logfile_analysis *out);

/* Mark a parseable journal clean by rewriting its two restart pages, instead of
 * erasing the whole log. Returns -ENOTSUP if the log cannot be parsed, in which
 * case the caller must fall back to the full erase. */
int ntfs_logfile_mark_clean_device(struct ntfs_bdev *dev);

/* Our own module's verdict on a device's journal, as an enum ntfs_log_state.
 * The vendored ntfs_check_logfile() cannot distinguish "no restart page" from
 * "a v2.0 header I do not understand"; this can. */
int ntfs_logfile_state_device(struct ntfs_bdev *dev, int *state_out);

/* Diagnostics */
typedef void (*ntfs_log_fn)(int level, const char *msg, void *ctx);
void ntfs_set_logger(ntfs_log_fn fn, void *ctx);
enum { NTFS_LOG_ERROR = 0, NTFS_LOG_WARN, NTFS_LOG_INFO, NTFS_LOG_DEBUG };
const char *ntfs_core_version(void);

#ifdef __cplusplus
}
#endif
#endif /* NTFSCORE_H */
