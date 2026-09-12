/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ntfs_logfile.h - NTFS $LogFile (LFS journal) parser, analyzer and replay.
 *
 * Standalone module: depends only on libc. The caller supplies the bytes of
 * $LogFile through ntfs_log_io and, for replay, access to MFT records and
 * clusters through ntfs_log_apply. The module never touches a device.
 *
 * Typical use at mount:
 *
 *   ntfs_logfile_open(&io, &geom, &ctx)      -> parse restart pages
 *   if (ntfs_logfile_is_clean(ctx)) { done }
 *   ntfs_logfile_replay(ctx, &apply, true)   -> dry run: analysis + plan
 *   if plan coherent: ntfs_logfile_replay(ctx, &apply, false)
 *   ntfs_logfile_mark_clean(ctx)             -> write clean restart pages
 *   ntfs_logfile_close(ctx)
 *
 * All functions return 0 or a negative errno. -EINVAL means the journal is
 * inconsistent (never apply), -ENOTSUP an unsupported version/feature.
 */
#ifndef NTFS_LOGFILE_H
#define NTFS_LOGFILE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Input: the $LogFile bytes ---------------------------------------- */

struct ntfs_log_io {
	void *ctx;
	uint64_t size;			/* data size of $LogFile in bytes */
	/* Read @len bytes at @off. Must fill the buffer completely. */
	int (*read)(void *ctx, uint64_t off, void *buf, size_t len);
	/* Optional; needed by ntfs_logfile_mark_clean(). */
	int (*write)(void *ctx, uint64_t off, const void *buf, size_t len);
};

/* Volume geometry the log math depends on. */
struct ntfs_log_geometry {
	uint32_t cluster_size;
	uint32_t mft_record_size;
	uint32_t sector_size;		/* 512 or 4096; log fixups always use 512 */
};

typedef struct ntfs_logfile ntfs_logfile_t;

enum ntfs_logfile_state {
	NTFS_LOG_EMPTY = 0,		/* all 0xff: freshly formatted, clean */
	NTFS_LOG_CLEAN,			/* closed, or open with VOLUME_IS_CLEAN */
	NTFS_LOG_DIRTY,			/* open, not clean: replay required */
	NTFS_LOG_CHKDSK,		/* restart page written by chkdsk */
	NTFS_LOG_CORRUPT,		/* no usable restart page */
	NTFS_LOG_UNSUPPORTED,		/* version we refuse to touch */
};

struct ntfs_logfile_info {
	int state;			/* enum ntfs_logfile_state */
	uint16_t major_ver, minor_ver;
	uint32_t system_page_size, log_page_size;
	uint64_t file_size;		/* usable size from the restart area */
	uint64_t current_lsn;		/* restart area's current lsn */
	uint64_t last_lsn;		/* true end of log after the tail scan (replay) */
	uint64_t oldest_lsn;
	uint64_t client_restart_lsn;	/* NTFS client's checkpoint record lsn */
	uint64_t checkpoint_lsn;	/* start of checkpoint from NTFS_RESTART */
	uint32_t seq_number_bits;
	uint32_t open_log_count;
	uint16_t restart_area_flags;
	uint16_t client_in_use, client_free;
	bool restart_page_used[2];	/* which of the two restart pages is current */
	bool volume_is_clean_flag;
	uint32_t table_transactions, table_dirty_pages, table_open_attrs;
	bool tail_scanned;		/* last_lsn/next_page reflect the page scan */
	uint32_t tail_overrides;	/* tail-copy pages that supersede on-disk pages */
	uint32_t next_page;		/* file offset of the next page the log would write */
	bool reuse_tail;		/* next_page still has room */
	uint32_t client_restart_version;/* NTFS_RESTART major version (0 or 1) */
	uint32_t attr_names_bytes;
};

int ntfs_logfile_open(const struct ntfs_log_io *io,
		      const struct ntfs_log_geometry *geom,
		      ntfs_logfile_t **out);
void ntfs_logfile_close(ntfs_logfile_t *log);
void ntfs_logfile_get_info(const ntfs_logfile_t *log, struct ntfs_logfile_info *info);

/* Same decision the Linux drivers make: true if the volume can be mounted
 * read-write without replay. Empty, closed, or open-with-clean-flag. */
bool ntfs_logfile_is_clean(const ntfs_logfile_t *log);

/* Last diagnostic message (static buffer inside @log). */
const char *ntfs_logfile_last_error(const ntfs_logfile_t *log);

/* Diagnostics sink. level: 0 error, 1 warn, 2 info, 3 debug. */
typedef void (*ntfs_log_msg_fn)(int level, const char *msg, void *ctx);
void ntfs_logfile_set_logger(ntfs_logfile_t *log, ntfs_log_msg_fn fn, void *ctx);

/* ---- Analysis --------------------------------------------------------- */

struct ntfs_log_record {
	uint64_t lsn;
	uint64_t client_prev_lsn;
	uint64_t client_undo_next_lsn;
	uint32_t transaction_id;	/* offset into transaction table */
	uint32_t record_type;		/* LFS_RECORD_TYPE_* */
	uint16_t flags;			/* LFS record header flags */
	uint32_t data_len;		/* client data length */
	/* NTFS client record (valid when record_type == client): */
	uint16_t redo_op, undo_op;
	uint16_t target_attr;		/* offset into open attribute table */
	uint16_t lcns_follow;
	uint16_t record_off, attr_off, cluster_off;
	uint64_t target_vcn;
	const uint64_t *page_lcns;	/* lcns_follow entries (little-endian) */
	const void *redo, *undo;
	uint16_t redo_len, undo_len;
	const void *raw;		/* client data; valid during callback */
	uint16_t client_seq, client_idx;/* LFS client id in the record header */
	uint32_t hdr_len;		/* NTFS client header incl. page_lcns */
};

/* Return 0 to continue, nonzero to stop enumeration. */
typedef int (*ntfs_log_record_cb)(const struct ntfs_log_record *rec, void *ctx);

/* Walk every record from @from_lsn (0 = the checkpoint / client restart
 * lsn) to the end of the log. Does not modify anything. Runs the tail scan
 * first so records written after the last restart-area flush are seen. */
int ntfs_logfile_walk(ntfs_logfile_t *log, uint64_t from_lsn,
		      ntfs_log_record_cb cb, void *ctx);

/* Human-readable op name. */
const char *ntfs_log_op_name(unsigned op);

/* Multi-sector transfer protection helpers (update sequence array), for
 * callers that hand raw MFT records / index blocks to the module. @sector
 * is the fixup stride (512 for NTFS). post_read returns -EINVAL for a
 * malformed array and sets *torn when a sector tail mismatches. */
int ntfs_log_fixup_post_read(void *rec, uint32_t bytes, uint32_t sector, bool *torn);
int ntfs_log_fixup_pre_write(void *rec, uint32_t bytes, uint32_t sector);

/*
 * Load the NTFS client restart record (checkpoint) and the three restart
 * tables it references, without walking the records after it. Runs the
 * tail scan first. Returns 0, -ENOENT when the client has no checkpoint
 * (nothing was ever logged), -EINVAL when the checkpoint is inconsistent.
 * ntfs_logfile_walk/replay call this implicitly.
 */
int ntfs_logfile_load_checkpoint(ntfs_logfile_t *log);

/* Decoded restart table entries. @id is the entry's byte offset inside its
 * table, which is how log records refer to it (transaction_id /
 * target_attr). Pointers are valid during the callback only. */
struct ntfs_log_transaction {
	uint32_t id;
	uint8_t state;			/* TRANSACTION_* (0 uninit, 1 active, 2 prepared, 3 committed) */
	uint64_t first_lsn, prev_lsn, undo_next_lsn;
	uint32_t undo_records, undo_bytes;
};
struct ntfs_log_dirty_page {
	uint32_t id;
	uint32_t target_attr, transfer_len, lcns_follow;
	uint64_t vcn, oldest_lsn;
	const uint64_t *page_lcns;	/* lcns_follow entries, little-endian */
};
struct ntfs_log_open_attr {
	uint32_t id;
	uint32_t type;			/* attribute type code */
	uint64_t mft_no;
	uint16_t mft_seq;
	uint32_t bytes_per_index;
	uint64_t open_record_lsn;
	uint8_t dirty_pages;
	uint16_t name_len;		/* UTF-16 units */
	const uint16_t *name;		/* from the attribute names dump; may be NULL */
};
struct ntfs_log_table_cbs {
	int (*transaction)(const struct ntfs_log_transaction *t, void *ctx);
	int (*dirty_page)(const struct ntfs_log_dirty_page *d, void *ctx);
	int (*open_attr)(const struct ntfs_log_open_attr *a, void *ctx);
};
/* Enumerate the tables as currently held: after
 * ntfs_logfile_load_checkpoint() these are the checkpoint dumps; after
 * ntfs_logfile_replay() they reflect the analysis pass. Any callback may be
 * NULL. A callback returning nonzero stops enumeration (returned). */
int ntfs_logfile_tables(ntfs_logfile_t *log, const struct ntfs_log_table_cbs *cbs, void *ctx);

/* ---- Replay ----------------------------------------------------------- */

/*
 * Access to the volume. Buffers carry raw on-disk bytes: MFT records and
 * index blocks are returned with their update sequence arrays applied
 * (protected), and must be written back protected. The module handles the
 * fixups. This keeps the in-memory test volume a flat byte array.
 */
struct ntfs_log_apply {
	void *ctx;
	int (*read_mft_record)(void *ctx, uint64_t mft_no, void *buf);
	int (*write_mft_record)(void *ctx, uint64_t mft_no, const void *buf);
	int (*read_clusters)(void *ctx, uint64_t lcn, uint32_t count, void *buf);
	int (*write_clusters)(void *ctx, uint64_t lcn, uint32_t count, const void *buf);
	/* Optional: called once after all writes. */
	int (*sync)(void *ctx);
};

enum ntfs_log_plan_kind {
	NTFS_PLAN_MFT_RECORD = 1,	/* write of one MFT record */
	NTFS_PLAN_CLUSTERS,		/* write of a cluster range */
	NTFS_PLAN_SKIPPED,		/* record examined, nothing to do */
};

struct ntfs_log_plan_entry {
	int kind;
	bool undo;			/* applied during the undo pass */
	uint64_t lsn;			/* log record that caused it */
	uint16_t op;
	uint64_t mft_no;		/* NTFS_PLAN_MFT_RECORD */
	uint64_t lcn;			/* NTFS_PLAN_CLUSTERS */
	uint32_t count;
	uint32_t target_attr;
	uint64_t target_vcn;
	char why[64];			/* for NTFS_PLAN_SKIPPED */
};

struct ntfs_log_replay_result {
	uint32_t records_analyzed;
	uint32_t records_redone;
	uint32_t records_undone;
	uint32_t transactions_active;	/* rolled back */
	uint32_t transactions_committed;
	uint32_t dirty_pages;
	uint32_t mft_records_written;
	uint32_t clusters_written;
	uint64_t redo_lsn;
	bool needs_chkdsk;		/* a record could not be applied safely */
	/* Plan (dry run and real run). Owned by @log until close. */
	const struct ntfs_log_plan_entry *plan;
	uint32_t plan_len;
};

/*
 * Analysis pass, redo pass, undo pass. With @dry_run nothing is written:
 * all modifications go to an in-memory overlay and the plan describes them.
 * Without @dry_run the same happens and, only if every step succeeded and
 * needs_chkdsk is false, the overlay is flushed through @apply in order
 * (MFT records, then clusters), then sync().
 */
int ntfs_logfile_replay(ntfs_logfile_t *log, const struct ntfs_log_apply *apply,
			bool dry_run, struct ntfs_log_replay_result *result);

/*
 * Write both restart pages marking the log closed and clean (what Windows
 * writes at dismount), so the next mount needs no replay. Requires io->write.
 * Also rewrites any tail-copy page the scan decided supersedes an on-disk
 * page. Does not zero the log records (the driver may do that separately).
 */
int ntfs_logfile_mark_clean(ntfs_logfile_t *log);

#ifdef __cplusplus
}
#endif
#endif /* NTFS_LOGFILE_H */
