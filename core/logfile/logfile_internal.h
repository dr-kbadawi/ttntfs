/* SPDX-License-Identifier: GPL-2.0 */
/* Internal state shared by the logfile module's source files. */
#ifndef NTFS_LOGFILE_INTERNAL_H
#define NTFS_LOGFILE_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include "ntfs_logfile.h"
#include "logfile_layout.h"

#define LFS_MAX_TAIL_OVERRIDES 32

struct lfs_page_override {
	uint32_t vbo;			/* real page offset this replaces */
	uint8_t *page;			/* page_size bytes, deprotected */
};

/* A loaded log record: header copy plus contiguous client data. */
struct lfs_record {
	uint8_t hdr[LR_HEADER_SIZE];
	uint8_t *data;			/* client_data_len bytes */
	uint32_t data_len;
	uint64_t lsn;
};

/* Restart table in memory: raw bytes, kernel-style free-list layout. */
struct lfs_table {
	uint8_t *buf;
	uint32_t bytes;			/* RT_HEADER_SIZE + entry_size * entries */
};

struct ntfs_logfile {
	struct ntfs_log_io io;
	struct ntfs_log_geometry geom;
	ntfs_log_msg_fn msg;
	void *msg_ctx;
	char errbuf[256];

	/* Geometry. */
	uint32_t orig_size;		/* min(io.size, 4G) */
	uint32_t l_size;		/* usable size (page multiple / ra file_size) */
	uint32_t page_size, page_bits, page_mask;
	uint32_t sys_page_size;
	uint16_t major_ver, minor_ver;
	uint32_t first_page;		/* first log record page offset */
	uint32_t data_off;		/* data offset within a record page */
	uint32_t record_header_len;
	uint32_t seq_num_bits, file_data_bits;
	uint32_t clst_per_page;

	/* Restart pages as found. */
	struct {
		bool present, valid, chkdsk;
		uint32_t vbo;
		uint64_t lsn;		/* current_lsn or chkdsk_lsn */
		uint8_t *page;		/* sys_page_size bytes, deprotected */
	} rst[2];
	int cur_rst;			/* index of the restart page in use, -1 none */
	uint8_t *ra;			/* copy of restart area incl. client array */
	uint32_t ra_len;
	uint32_t ra_ofs;		/* offset of ra within page */
	uint16_t client_idx;		/* NTFS client index */
	uint16_t client_seq;
	uint64_t client_oldest_lsn, client_restart_lsn;

	/* Log state. */
	int state;			/* enum ntfs_logfile_state */
	bool initialized;		/* any non-0xff page seen */
	uint64_t current_lsn;		/* from restart area */
	uint64_t last_lsn;		/* after tail scan */
	uint64_t oldest_lsn;
	uint64_t seq_num;
	uint32_t next_page;
	bool wrapped, reuse_tail, no_last_lsn, single_page_io;
	bool tail_scanned;
	int tail_err;			/* result of the scan, returned on repeat calls */
	uint32_t open_log_count;

	/* Tail-copy overrides discovered by the scan. */
	struct lfs_page_override ovr[LFS_MAX_TAIL_OVERRIDES];
	int n_ovr;

	/* Client restart (checkpoint) data. */
	uint8_t *crst;			/* NTFS_RESTART bytes */
	uint32_t crst_len;
	uint64_t checkpoint_lsn;
	uint32_t bytes_per_attr_entry;	/* 0x2c (v0) or 0x28 (v1) */
	uint32_t crst_major;

	/* Tables (owned). */
	struct lfs_table trtbl, dptbl, oatbl;
	uint8_t *attr_names;
	uint32_t attr_names_len;

	/* Replay plan (owned). */
	struct ntfs_log_plan_entry *plan;
	uint32_t plan_len, plan_cap;
};

/* logfile_open.c */
void lfs_msg(ntfs_logfile_t *log, int level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
int lfs_seterr(ntfs_logfile_t *log, int err, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* logfile_pages.c */
int lfs_fixup_post_read(uint8_t *rec, uint32_t bytes, uint32_t sector, bool *torn);
int lfs_fixup_pre_write(uint8_t *rec, uint32_t bytes, uint32_t sector);
/* Read and deprotect the record page containing @vbo into a page_size
 * buffer (page-aligned start). *torn set on USA mismatch. */
int lfs_read_page(ntfs_logfile_t *log, uint32_t vbo, uint8_t *page, bool *torn);
static inline uint32_t lfs_lsn_to_vbo(const ntfs_logfile_t *log, uint64_t lsn)
{
	return (uint32_t)((lsn << log->seq_num_bits) >> (log->seq_num_bits - 3));
}
static inline uint64_t lfs_vbo_to_lsn(const ntfs_logfile_t *log, uint32_t vbo, uint64_t seq)
{
	return ((uint64_t)vbo >> 3) + (seq << log->file_data_bits);
}
static inline uint64_t lfs_lsn_seq(const ntfs_logfile_t *log, uint64_t lsn)
{
	return lsn >> log->file_data_bits;
}
static inline uint32_t lfs_next_page_off(const ntfs_logfile_t *log, uint32_t off)
{
	off = (off & ~log->page_mask) + log->page_size;
	return off >= log->l_size ? log->first_page : off;
}
static inline bool lfs_lsn_in_file(const ntfs_logfile_t *log, uint64_t lsn)
{
	return lsn && lsn >= log->oldest_lsn && lsn <= log->last_lsn;
}
uint32_t lfs_final_log_off(const ntfs_logfile_t *log, uint64_t lsn, uint32_t data_len);
int lfs_tail_scan(ntfs_logfile_t *log);

/* logfile_records.c */
int lfs_read_record(ntfs_logfile_t *log, uint64_t lsn, struct lfs_record *rec);
void lfs_free_record(struct lfs_record *rec);
int lfs_next_lsn(ntfs_logfile_t *log, const struct lfs_record *rec, uint64_t *next);
bool lfs_check_client_rec(const struct lfs_record *rec, uint32_t bytes_per_attr_entry);
int lfs_decode_record(const struct lfs_record *rec, struct ntfs_log_record *out);

/* logfile_tables.c */
bool lfs_table_check(const uint8_t *rt, uint32_t bytes);
int lfs_table_copy(struct lfs_table *t, const uint8_t *src, uint32_t bytes);
int lfs_table_init(struct lfs_table *t, uint16_t esize, uint16_t used);
void lfs_table_free(struct lfs_table *t);
uint8_t *lfs_table_next(const struct lfs_table *t, uint8_t *cur);
uint8_t *lfs_table_alloc(struct lfs_table *t);
uint8_t *lfs_table_alloc_at(struct lfs_table *t, uint32_t off);
void lfs_table_free_idx(struct lfs_table *t, uint32_t off);
static inline uint16_t lfs_table_esize(const struct lfs_table *t) { return lf_get16(t->buf + RT_ENTRY_SIZE); }
static inline uint16_t lfs_table_total(const struct lfs_table *t) { return t->buf ? lf_get16(t->buf + RT_NUMBER_ALLOCATED) : 0; }
static inline uint32_t lfs_table_off(const struct lfs_table *t, const uint8_t *e) { return (uint32_t)(e - t->buf); }
static inline bool lfs_table_entry_ok(const struct lfs_table *t, uint32_t off)
{
	return t->buf && off >= RT_HEADER_SIZE && off + lfs_table_esize(t) <= t->bytes &&
	       lf_get32(t->buf + off) == RT_ENTRY_ALLOCATED;
}
int lfs_load_checkpoint(ntfs_logfile_t *log);

/* logfile_replay.c */
int lfs_plan_add(ntfs_logfile_t *log, const struct ntfs_log_plan_entry *e);

#endif /* NTFS_LOGFILE_INTERNAL_H */
