// SPDX-License-Identifier: GPL-2.0
/*
 * Restart tables (transaction, dirty page, open attribute) and the
 * checkpoint (NTFS_RESTART client restart record) that references them.
 *
 * A restart table is a header followed by fixed-size entries. Allocated
 * entries begin with 0xFFFFFFFF; free entries begin with the offset of the
 * next free entry (0 = end). Tables are dumped into the log verbatim, so
 * the in-memory format equals the on-disk format.
 */
#include "logfile_internal.h"

static uint32_t table_bytes(const uint8_t *rt)
{
	return RT_HEADER_SIZE + (uint32_t)lf_get16(rt + RT_NUMBER_ENTRIES) * lf_get16(rt + RT_ENTRY_SIZE);
}

bool lfs_table_check(const uint8_t *rt, uint32_t bytes)
{
	uint16_t rsize, ne;
	uint32_t ts, ff, lf, i, off, guard;

	if (bytes < RT_HEADER_SIZE)
		return false;
	rsize = lf_get16(rt + RT_ENTRY_SIZE);
	ne = lf_get16(rt + RT_NUMBER_ENTRIES);
	ff = lf_get32(rt + RT_FIRST_FREE);
	lf = lf_get32(rt + RT_LAST_FREE);
	ts = RT_HEADER_SIZE + (uint32_t)rsize * ne;
	if (!rsize || rsize < 4 || rsize > bytes || bytes < ts ||
	    lf_get16(rt + RT_NUMBER_ALLOCATED) > ne ||
	    ff > ts - 4 || lf > ts - 4 ||
	    (ff && ff < RT_HEADER_SIZE) || (lf && lf < RT_HEADER_SIZE))
		return false;
	for (i = 0; i < ne; i++) {
		off = lf_get32(rt + RT_HEADER_SIZE + (uint32_t)i * rsize);
		if (off != RT_ENTRY_ALLOCATED && off &&
		    (off < RT_HEADER_SIZE || (off - RT_HEADER_SIZE) % rsize))
			return false;
	}
	/* Walk the free list; must not hit an allocated entry or loop. */
	for (off = ff, guard = 0; off; guard++) {
		if (off == RT_ENTRY_ALLOCATED || guard > ne)
			return false;
		off = lf_get32(rt + off);
		if (off > ts - 4)
			return false;
	}
	return true;
}

int lfs_table_copy(struct lfs_table *t, const uint8_t *src, uint32_t bytes)
{
	uint32_t need = table_bytes(src);

	if (need > bytes)
		return -EINVAL;
	lfs_table_free(t);
	t->buf = malloc(need);
	if (!t->buf)
		return -ENOMEM;
	memcpy(t->buf, src, need);
	t->bytes = need;
	return 0;
}

int lfs_table_init(struct lfs_table *t, uint16_t esize, uint16_t used)
{
	uint32_t bytes = RT_HEADER_SIZE + (uint32_t)esize * used;
	uint32_t off, lf = RT_HEADER_SIZE + (uint32_t)(used - 1) * esize;
	uint32_t i;

	lfs_table_free(t);
	t->buf = calloc(1, bytes);
	if (!t->buf)
		return -ENOMEM;
	t->bytes = bytes;
	lf_put16(t->buf + RT_ENTRY_SIZE, esize);
	lf_put16(t->buf + RT_NUMBER_ENTRIES, used);
	lf_put32(t->buf + RT_FREE_GOAL, 0xffffffffu);
	lf_put32(t->buf + RT_FIRST_FREE, RT_HEADER_SIZE);
	lf_put32(t->buf + RT_LAST_FREE, lf);
	for (i = 0, off = RT_HEADER_SIZE; i + 1 < used; i++, off += esize)
		lf_put32(t->buf + off, off + esize);
	return 0;
}

void lfs_table_free(struct lfs_table *t)
{
	free(t->buf);
	t->buf = NULL;
	t->bytes = 0;
}

/* Next allocated entry after @cur (NULL = first). */
uint8_t *lfs_table_next(const struct lfs_table *t, uint8_t *cur)
{
	uint16_t rsize;
	uint8_t *e;

	if (!t->buf || !lfs_table_total(t))
		return NULL;
	rsize = lfs_table_esize(t);
	e = cur ? cur + rsize : t->buf + RT_HEADER_SIZE;
	for (; (uint32_t)(e - t->buf) + rsize <= t->bytes; e += rsize) {
		if (lf_get32(e) == RT_ENTRY_ALLOCATED)
			return e;
	}
	return NULL;
}

static int table_extend(struct lfs_table *t, uint32_t add, uint32_t free_goal_entry)
{
	uint16_t esize = lfs_table_esize(t);
	uint32_t used = lf_get16(t->buf + RT_NUMBER_ENTRIES);
	uint32_t osize = t->bytes;
	struct lfs_table nt = { 0 };
	int err;

	if (used + add > 0xffff)
		return -E2BIG;
	err = lfs_table_init(&nt, esize, (uint16_t)(used + add));
	if (err)
		return err;
	memcpy(nt.buf + RT_HEADER_SIZE, t->buf + RT_HEADER_SIZE, (size_t)esize * used);
	lf_put32(nt.buf + RT_FREE_GOAL, free_goal_entry == 0xffffffffu ? 0xffffffffu :
		 RT_HEADER_SIZE + free_goal_entry * esize);
	if (lf_get32(t->buf + RT_FIRST_FREE)) {
		lf_put32(nt.buf + RT_FIRST_FREE, lf_get32(t->buf + RT_FIRST_FREE));
		lf_put32(nt.buf + lf_get32(t->buf + RT_LAST_FREE), osize);
	} else {
		lf_put32(nt.buf + RT_FIRST_FREE, osize);
	}
	lf_put16(nt.buf + RT_NUMBER_ALLOCATED, lf_get16(t->buf + RT_NUMBER_ALLOCATED));
	lfs_table_free(t);
	*t = nt;
	return 0;
}

uint8_t *lfs_table_alloc(struct lfs_table *t)
{
	uint32_t off;
	uint8_t *e;

	if (!lf_get32(t->buf + RT_FIRST_FREE)) {
		if (table_extend(t, 16, 0xffffffffu))
			return NULL;
	}
	off = lf_get32(t->buf + RT_FIRST_FREE);
	if (off < RT_HEADER_SIZE || off + lfs_table_esize(t) > t->bytes)
		return NULL;
	e = t->buf + off;
	lf_put32(t->buf + RT_FIRST_FREE, lf_get32(e));
	memset(e, 0, lfs_table_esize(t));
	lf_put32(e, RT_ENTRY_ALLOCATED);
	if (!lf_get32(t->buf + RT_FIRST_FREE))
		lf_put32(t->buf + RT_LAST_FREE, 0);
	lf_put16(t->buf + RT_NUMBER_ALLOCATED, lf_get16(t->buf + RT_NUMBER_ALLOCATED) + 1);
	return e;
}

/* Allocate the entry at a specific offset (transaction ids are offsets). */
uint8_t *lfs_table_alloc_at(struct lfs_table *t, uint32_t vbo)
{
	uint16_t esize = lfs_table_esize(t);
	uint32_t off, prev_off;
	uint8_t *e;

	if (vbo < RT_HEADER_SIZE || (vbo - RT_HEADER_SIZE) % esize)
		return NULL;
	if (vbo >= t->bytes) {
		uint32_t add = (vbo - t->bytes) / esize + 1;
		uint32_t old_entries = lf_get16(t->buf + RT_NUMBER_ENTRIES);
		if (table_extend(t, add, old_entries))
			return NULL;
	}
	e = t->buf + vbo;
	if (lf_get32(e) == RT_ENTRY_ALLOCATED)
		return e;
	/* Unlink from the free list. */
	off = lf_get32(t->buf + RT_FIRST_FREE);
	if (off == vbo) {
		lf_put32(t->buf + RT_FIRST_FREE, lf_get32(e));
	} else {
		uint32_t guard = 0;
		prev_off = off;
		while (prev_off) {
			off = lf_get32(t->buf + prev_off);
			if (off == vbo)
				break;
			prev_off = off;
			if (++guard > 0x10000)
				return NULL;
		}
		if (!prev_off)
			return NULL;	/* not on the free list: corrupt */
		lf_put32(t->buf + prev_off, lf_get32(e));
		if (lf_get32(t->buf + RT_LAST_FREE) == vbo)
			lf_put32(t->buf + RT_LAST_FREE, prev_off);
	}
	if (!lf_get32(t->buf + RT_FIRST_FREE))
		lf_put32(t->buf + RT_LAST_FREE, 0);
	memset(e, 0, esize);
	lf_put32(e, RT_ENTRY_ALLOCATED);
	lf_put16(t->buf + RT_NUMBER_ALLOCATED, lf_get16(t->buf + RT_NUMBER_ALLOCATED) + 1);
	return e;
}

void lfs_table_free_idx(struct lfs_table *t, uint32_t off)
{
	uint8_t *e = t->buf + off;
	uint32_t lf = lf_get32(t->buf + RT_LAST_FREE);

	if (!lfs_table_entry_ok(t, off))
		return;
	if (off < lf_get32(t->buf + RT_FREE_GOAL)) {
		lf_put32(e, lf_get32(t->buf + RT_FIRST_FREE));
		lf_put32(t->buf + RT_FIRST_FREE, off);
		if (!lf)
			lf_put32(t->buf + RT_LAST_FREE, off);
	} else {
		if (lf)
			lf_put32(t->buf + lf, off);
		else
			lf_put32(t->buf + RT_FIRST_FREE, off);
		lf_put32(t->buf + RT_LAST_FREE, off);
		lf_put32(e, 0);
	}
	lf_put16(t->buf + RT_NUMBER_ALLOCATED, lf_get16(t->buf + RT_NUMBER_ALLOCATED) - 1);
}

/* ---- Checkpoint ------------------------------------------------------- */

/*
 * Read the record at @lsn, verify it is a client record carrying a restart
 * table in its redo data, and copy the table into @t.
 */
static int load_table_record(ntfs_logfile_t *log, uint64_t lsn, struct lfs_table *t, const char *what)
{
	struct lfs_record rec;
	uint16_t redo_off;
	int err;

	err = lfs_read_record(log, lsn, &rec);
	if (err)
		return lfs_seterr(log, err, "%s table: cannot read record at lsn 0x%llx", what,
				  (unsigned long long)lsn);
	if (!lfs_check_client_rec(&rec, log->bytes_per_attr_entry, NULL)) {
		lfs_free_record(&rec);
		return lfs_seterr(log, -EINVAL, "%s table: malformed client record", what);
	}
	redo_off = lf_get16(rec.data + NR_REDO_OFFSET);
	if (redo_off >= rec.data_len || !lfs_table_check(rec.data + redo_off, rec.data_len - redo_off)) {
		lfs_free_record(&rec);
		return lfs_seterr(log, -EINVAL, "%s table: invalid restart table", what);
	}
	err = lfs_table_copy(t, rec.data + redo_off, rec.data_len - redo_off);
	lfs_free_record(&rec);
	return err;
}

static int load_attr_names(ntfs_logfile_t *log, uint64_t lsn)
{
	struct lfs_record rec;
	uint32_t hdr_len, lcns;
	int err;

	err = lfs_read_record(log, lsn, &rec);
	if (err)
		return lfs_seterr(log, err, "attribute names: cannot read record");
	if (!lfs_check_client_rec(&rec, log->bytes_per_attr_entry, NULL)) {
		lfs_free_record(&rec);
		return lfs_seterr(log, -EINVAL, "attribute names: malformed client record");
	}
	lcns = lf_get16(rec.data + NR_LCNS_TO_FOLLOW);
	hdr_len = NR_HEADER_SIZE + 8u * (lcns ? lcns : 1);
	free(log->attr_names);
	log->attr_names_len = rec.data_len - hdr_len;
	log->attr_names = malloc(log->attr_names_len ? log->attr_names_len : 1);
	if (!log->attr_names) {
		lfs_free_record(&rec);
		return -ENOMEM;
	}
	memcpy(log->attr_names, rec.data + hdr_len, log->attr_names_len);
	lfs_free_record(&rec);
	return 0;
}

/*
 * Convert a version-0 dirty page table (DIR_PAGE_ENTRY_32 layout) into the
 * version-1 layout in place, as fslog.c does. The v0 entry is 0x2c bytes
 * for the header vs 0x20, with vcn/oldest_lsn/lcns shifted.
 */
static void convert_dptbl_v0(struct lfs_table *t)
{
	uint8_t *dp = NULL;

	while ((dp = lfs_table_next(t, dp))) {
		uint32_t lcns = lf_get32(dp + DP1_LCNS_FOLLOW);
		uint32_t esize = lfs_table_esize(t);
		uint32_t n = 16 + 8 * lcns;
		if (DP1_VCN + n > esize)
			n = esize - DP1_VCN;
		if (DP0_VCN + n > esize)
			n = esize - DP0_VCN;
		memmove(dp + DP1_VCN, dp + DP0_VCN, n);
	}
}

/*
 * Remove duplicate (target_attr, vcn) dirty page entries keeping the
 * oldest lsn. fslog.c only does this when the cluster size exceeds the log
 * page size (the case where Windows can produce them), but duplicates are
 * redundant in any case and find_dp() must see the oldest lsn.
 */
static void dedupe_dptbl(struct lfs_table *t)
{
	uint8_t *dp = NULL;

	while ((dp = lfs_table_next(t, dp))) {
		uint8_t *next = dp;
		while ((next = lfs_table_next(t, next))) {
			if (lf_get32(next + DP1_TARGET_ATTR) == lf_get32(dp + DP1_TARGET_ATTR) &&
			    lf_get64(next + DP1_VCN) == lf_get64(dp + DP1_VCN)) {
				if (lf_get64(next + DP1_OLDEST_LSN) < lf_get64(dp + DP1_OLDEST_LSN))
					lf_put64(dp + DP1_OLDEST_LSN, lf_get64(next + DP1_OLDEST_LSN));
				lfs_table_free_idx(t, lfs_table_off(t, next));
			}
		}
	}
}

/*
 * Locate the NTFS client restart record via the client's restart lsn and
 * load the checkpoint tables it references.
 */
int lfs_load_checkpoint(ntfs_logfile_t *log)
{
	struct lfs_record rec;
	uint64_t lsn = log->client_restart_lsn;
	const uint8_t *c;
	int err;

	if (log->crst)
		return 0;
	if (!lsn) {
		lfs_msg(log, 2, "client has no restart record: nothing to replay");
		return -ENOENT;
	}
	err = lfs_tail_scan(log);
	if (err)
		return err;
	err = lfs_read_record(log, lsn, &rec);
	if (err)
		return lfs_seterr(log, err, "client restart record at lsn 0x%llx unreadable",
				  (unsigned long long)lsn);
	if (lf_get32(rec.hdr + LR_RECORD_TYPE) != LFS_RECORD_TYPE_CLIENT_RESTART) {
		lfs_free_record(&rec);
		return lfs_seterr(log, -EINVAL, "record at client restart lsn is not a restart record");
	}
	if (!rec.data_len) {
		lfs_free_record(&rec);
		return -ENOENT;
	}
	if (rec.data_len < CRST_SIZE) {
		lfs_free_record(&rec);
		return lfs_seterr(log, -EINVAL, "client restart record too short (%u)", rec.data_len);
	}
	log->crst = rec.data;
	log->crst_len = rec.data_len;
	rec.data = NULL;
	c = log->crst;
	log->crst_major = lf_get32(c + CRST_MAJOR_VER);
	log->bytes_per_attr_entry = log->crst_major ? OA1_SIZE : OA0_SIZE;
	log->checkpoint_lsn = lf_get64(c + CRST_START_OF_CHECKPOINT);
	if (!log->checkpoint_lsn)
		log->checkpoint_lsn = lsn;

	if (lf_get32(c + CRST_TRANSACTION_TABLE_LEN)) {
		err = load_table_record(log, lf_get64(c + CRST_TRANSACTION_TABLE_LSN), &log->trtbl, "transaction");
		if (err)
			return err;
		if (lfs_table_esize(&log->trtbl) != TR_SIZE)
			return lfs_seterr(log, -EINVAL, "transaction table entry size %u", lfs_table_esize(&log->trtbl));
	}
	if (lf_get32(c + CRST_DIRTY_PAGE_TABLE_LEN)) {
		err = load_table_record(log, lf_get64(c + CRST_DIRTY_PAGE_TABLE_LSN), &log->dptbl, "dirty page");
		if (err)
			return err;
		if (!log->crst_major)
			convert_dptbl_v0(&log->dptbl);
		if (lfs_table_esize(&log->dptbl) < DP1_SIZE + 8)
			return lfs_seterr(log, -EINVAL, "dirty page table entry size %u", lfs_table_esize(&log->dptbl));
		/* Entries must describe lcns that fit the entry. */
		{
			uint8_t *dp = NULL;
			while ((dp = lfs_table_next(&log->dptbl, dp))) {
				if (DP1_SIZE + 8ull * lf_get32(dp + DP1_LCNS_FOLLOW) > lfs_table_esize(&log->dptbl))
					return lfs_seterr(log, -EINVAL, "dirty page entry 0x%x: lcns_follow overflows entry",
							  lfs_table_off(&log->dptbl, dp));
			}
		}
		dedupe_dptbl(&log->dptbl);
	}
	if (lf_get32(c + CRST_ATTR_NAMES_LEN)) {
		err = load_attr_names(log, lf_get64(c + CRST_ATTR_NAMES_LSN));
		if (err)
			return err;
	}
	if (lf_get32(c + CRST_OPEN_ATTR_TABLE_LEN)) {
		err = load_table_record(log, lf_get64(c + CRST_OPEN_ATTR_TABLE_LSN), &log->oatbl, "open attribute");
		if (err)
			return err;
		if (lfs_table_esize(&log->oatbl) != log->bytes_per_attr_entry)
			return lfs_seterr(log, -EINVAL, "open attribute entry size %u != %u",
					  lfs_table_esize(&log->oatbl), log->bytes_per_attr_entry);
	}
	lfs_msg(log, 2, "checkpoint: lsn 0x%llx, restart v%u, tables tr=%u dp=%u oa=%u names=%u bytes",
		(unsigned long long)log->checkpoint_lsn, log->crst_major,
		lfs_table_total(&log->trtbl), lfs_table_total(&log->dptbl),
		lfs_table_total(&log->oatbl), log->attr_names_len);
	return 0;
}

/* ---- Public table access ----------------------------------------------- */

int ntfs_logfile_load_checkpoint(ntfs_logfile_t *log)
{
	int err;

	if (log->state == NTFS_LOG_EMPTY)
		return -ENOENT;
	if (log->state == NTFS_LOG_CORRUPT || log->state == NTFS_LOG_UNSUPPORTED)
		return -EINVAL;
	err = lfs_tail_scan(log);
	if (err)
		return err;
	return lfs_load_checkpoint(log);
}

static const uint16_t *find_attr_name(const ntfs_logfile_t *log, uint32_t off, uint16_t *len)
{
	uint32_t p = 0;

	*len = 0;
	if (!log->attr_names)
		return NULL;
	while (p + AN_NAME <= log->attr_names_len) {
		uint16_t eo = lf_get16(log->attr_names + p + AN_OFFSET);
		uint16_t nb = lf_get16(log->attr_names + p + AN_NAME_BYTES);
		if (!eo || p + AN_NAME + nb > log->attr_names_len)
			break;
		if (eo == off) {
			*len = nb / 2;
			return (const uint16_t *)(log->attr_names + p + AN_NAME);
		}
		p += AN_NAME + nb;
	}
	return NULL;
}

int ntfs_logfile_tables(ntfs_logfile_t *log, const struct ntfs_log_table_cbs *cbs, void *ctx)
{
	uint8_t *e;
	int rc;

	if (cbs->transaction) {
		for (e = NULL; (e = lfs_table_next(&log->trtbl, e));) {
			struct ntfs_log_transaction t;
			t.id = lfs_table_off(&log->trtbl, e);
			t.state = e[TR_STATE];
			t.first_lsn = lf_get64(e + TR_FIRST_LSN);
			t.prev_lsn = lf_get64(e + TR_PREV_LSN);
			t.undo_next_lsn = lf_get64(e + TR_UNDO_NEXT_LSN);
			t.undo_records = lf_get32(e + TR_UNDO_RECORDS);
			t.undo_bytes = lf_get32(e + TR_UNDO_BYTES);
			if ((rc = cbs->transaction(&t, ctx)))
				return rc;
		}
	}
	if (cbs->dirty_page) {
		for (e = NULL; (e = lfs_table_next(&log->dptbl, e));) {
			struct ntfs_log_dirty_page d;
			d.id = lfs_table_off(&log->dptbl, e);
			d.target_attr = lf_get32(e + DP1_TARGET_ATTR);
			d.transfer_len = lf_get32(e + DP1_TRANSFER_LEN);
			d.lcns_follow = lf_get32(e + DP1_LCNS_FOLLOW);
			d.vcn = lf_get64(e + DP1_VCN);
			d.oldest_lsn = lf_get64(e + DP1_OLDEST_LSN);
			d.page_lcns = (const uint64_t *)(e + DP1_PAGE_LCNS);
			if ((rc = cbs->dirty_page(&d, ctx)))
				return rc;
		}
	}
	if (cbs->open_attr) {
		for (e = NULL; (e = lfs_table_next(&log->oatbl, e));) {
			struct ntfs_log_open_attr a;
			const uint8_t *ref;
			a.id = lfs_table_off(&log->oatbl, e);
			if (log->crst_major) {
				a.type = lf_get32(e + OA1_TYPE);
				a.bytes_per_index = lf_get32(e + OA1_BYTES_PER_INDEX);
				a.open_record_lsn = lf_get64(e + OA1_OPEN_RECORD_LSN);
				a.dirty_pages = e[OA1_DIRTY_PAGES];
				ref = e + OA1_FILE_REF;
			} else {
				a.type = lf_get32(e + OA0_TYPE);
				a.bytes_per_index = lf_get32(e + OA0_BYTES_PER_INDEX);
				a.open_record_lsn = lf_get64(e + OA0_OPEN_RECORD_LSN);
				a.dirty_pages = e[OA0_DIRTY_PAGES];
				ref = e + OA0_FILE_REF;
			}
			a.mft_no = lf_get32(ref) | ((uint64_t)lf_get16(ref + 4) << 32);
			a.mft_seq = lf_get16(ref + 6);
			a.name = find_attr_name(log, a.id, &a.name_len);
			if ((rc = cbs->open_attr(&a, ctx)))
				return rc;
		}
	}
	return 0;
}
