// SPDX-License-Identifier: GPL-2.0
/*
 * Log record pages: multi-sector protection, page reads with tail-copy
 * overrides, LSN <-> offset arithmetic, and the scan that finds the true
 * end of the log (the restart area's current_lsn lags behind the pages
 * actually written; NTFS also keeps two "tail copy" pages that may hold a
 * newer version of the last page).
 */
#include "logfile_internal.h"

/*
 * Remove the update sequence array from a record. Returns -EINVAL if the
 * usa is malformed; sets *torn if a sector's tail does not match (the
 * record is then partially written and its contents are garbage).
 */
int lfs_fixup_post_read(uint8_t *rec, uint32_t bytes, uint32_t sector, bool *torn)
{
	uint16_t usa_ofs = lf_get16(rec + 4);
	uint16_t usa_count = lf_get16(rec + 6);
	uint16_t usn;
	uint32_t i;

	if (torn)
		*torn = false;
	if ((usa_ofs & 1) || usa_count < 2 || usa_ofs + usa_count * 2u > sector ||
	    (uint32_t)(usa_count - 1) * sector != bytes)
		return -EINVAL;
	usn = lf_get16(rec + usa_ofs);
	for (i = 1; i < usa_count; i++) {
		uint8_t *p = rec + i * sector - 2;
		if (lf_get16(p) != usn) {
			if (torn)
				*torn = true;
		}
		lf_put16(p, lf_get16(rec + usa_ofs + i * 2));
	}
	return 0;
}

/* Apply the usa before writing: bump usn, stash sector tails. */
int lfs_fixup_pre_write(uint8_t *rec, uint32_t bytes, uint32_t sector)
{
	uint16_t usa_ofs = lf_get16(rec + 4);
	uint16_t usa_count = lf_get16(rec + 6);
	uint16_t usn;
	uint32_t i;

	if ((usa_ofs & 1) || usa_count < 2 || usa_ofs + usa_count * 2u > sector ||
	    (uint32_t)(usa_count - 1) * sector != bytes)
		return -EINVAL;
	usn = lf_get16(rec + usa_ofs) + 1;
	if (usn == 0xffff || usn == 0)
		usn = 1;
	lf_put16(rec + usa_ofs, usn);
	for (i = 1; i < usa_count; i++) {
		uint8_t *p = rec + i * sector - 2;
		lf_put16(rec + usa_ofs + i * 2, lf_get16(p));
		lf_put16(p, usn);
	}
	return 0;
}

int lfs_read_page(ntfs_logfile_t *log, uint32_t vbo, uint8_t *page, bool *torn)
{
	uint32_t pvbo = vbo & ~log->page_mask;
	int i, err;

	if (torn)
		*torn = false;
	if (pvbo >= log->l_size || pvbo + log->page_size > log->orig_size)
		return -EINVAL;
	for (i = 0; i < log->n_ovr; i++) {
		if (log->ovr[i].vbo == pvbo) {
			memcpy(page, log->ovr[i].page, log->page_size);
			return 0;
		}
	}
	err = log->io.read(log->io.ctx, pvbo, page, log->page_size);
	if (err)
		return err < 0 ? err : -EIO;
	if (lf_get32(page) == LFS_MAGIC_EMPTY)
		return 0;
	if (lf_get32(page) == LFS_MAGIC_RCRD || lf_get32(page) == LFS_MAGIC_RSTR ||
	    lf_get32(page) == LFS_MAGIC_CHKD) {
		bool t = false;
		err = lfs_fixup_post_read(page, log->page_size, LFS_SECTOR_SIZE, &t);
		if (err)
			return err;
		if (t) {
			if (!torn)
				return -EINVAL;
			*torn = true;
		}
	}
	return 0;
}

/*
 * Byte offset just past the client data of the record at @lsn whose
 * client data is @data_len bytes long, walking page boundaries (records
 * continue at data_off of the next page) and the file wrap.
 */
uint32_t lfs_final_log_off(const ntfs_logfile_t *log, uint64_t lsn, uint32_t data_len)
{
	uint32_t base_vbo = (uint32_t)(lsn << 3);
	uint32_t final = (base_vbo & ((8u << log->file_data_bits) - 1)) & ~log->page_mask;
	uint32_t page_off = base_vbo & log->page_mask;
	uint32_t tail = log->page_size - page_off;

	page_off -= 1;
	data_len += log->record_header_len;
	if (data_len > tail) {
		data_len -= tail;
		tail = log->page_size - log->data_off;
		page_off = log->data_off - 1;
		for (;;) {
			final = lfs_next_page_off(log, final);
			if (data_len <= tail)
				break;
			data_len -= tail;
		}
	}
	return final + data_len + page_off;
}

/* ---- Tail scan --------------------------------------------------------- */

/*
 * Tail copies. Before the LFS rewrites the last log page in place (to append
 * records to a partially filled page) it writes a copy of the new page
 * contents to a "tail" page: v1.x keeps two of them at pages 2 and 3, v2.0
 * keeps 16 + 16 at pages 0x02.. and 0x12.. (fslog.c). A copy records the
 * offset of the page it duplicates: v1.x overloads the 64-bit lsn field at
 * 0x08 with that file offset (so the copy has no usable last_lsn), v2.0 has
 * a dedicated file_off field at 0x3c. After a crash between the two writes
 * the copy is the newer state of that page.
 */
struct tail_copy {
	bool valid, has_end;
	uint32_t vbo;		/* where the copy lives */
	uint32_t file_off;	/* the page it is a copy of */
	uint64_t last_lsn;	/* v2 only: last record header on the page */
	uint64_t last_end_lsn;	/* last record ending on the page (if has_end) */
	uint64_t end;		/* the lsn we compare pages by */
	uint64_t seq;		/* sequence number the page belongs to */
	uint8_t *page;
};

static bool page_is_rcrd(const uint8_t *page)
{
	return lf_get32(page + PG_MAGIC) == LFS_MAGIC_RCRD;
}

static uint32_t page_file_off(const ntfs_logfile_t *log, const uint8_t *page)
{
	if (log->major_ver < 2)
		return (uint32_t)lf_get64(page + PG_LAST_LSN);
	return lf_get32(page + PG_FILE_OFFSET);
}

static int add_override(ntfs_logfile_t *log, uint32_t vbo, const uint8_t *page)
{
	int i;
	uint8_t *copy;

	for (i = 0; i < log->n_ovr; i++)
		if (log->ovr[i].vbo == vbo)
			break;
	if (i == log->n_ovr) {
		if (log->n_ovr >= LFS_MAX_TAIL_OVERRIDES)
			return -E2BIG;
		log->ovr[i].page = malloc(log->page_size);
		if (!log->ovr[i].page)
			return -ENOMEM;
		log->ovr[i].vbo = vbo;
		log->n_ovr++;
	}
	copy = log->ovr[i].page;
	memcpy(copy, page, log->page_size);
	/*
	 * Make the copy look like an in-place page for the record readers:
	 * v1.x copies carry the file offset where last_lsn belongs (restore
	 * last_end_lsn there, as fslog.c does when it writes the page back);
	 * v2.0 copies keep their lsn and drop file_off.
	 */
	if (log->major_ver < 2)
		lf_put64(copy + PG_LAST_LSN, lf_get64(copy + PG_LAST_END_LSN));
	else
		lf_put32(copy + PG_FILE_OFFSET, 0);
	lf_put16(copy + PG_PAGE_COUNT, 1);
	lf_put16(copy + PG_PAGE_POSITION, 1);
	return 0;
}

static void load_tail(ntfs_logfile_t *log, uint32_t vbo, struct tail_copy *t)
{
	bool torn = false;

	memset(t, 0, sizeof(*t));
	t->page = malloc(log->page_size);
	if (!t->page)
		return;
	if (lfs_read_page(log, vbo, t->page, &torn) || torn || !page_is_rcrd(t->page)) {
		free(t->page);
		t->page = NULL;
		return;
	}
	t->vbo = vbo;
	t->file_off = page_file_off(log, t->page);
	t->has_end = lf_get32(t->page + PG_FLAGS) & LFS_PAGE_LOG_RECORD_END;
	t->last_end_lsn = lf_get64(t->page + PG_LAST_END_LSN);
	t->valid = t->file_off >= log->first_page && t->file_off < log->l_size &&
		   !(t->file_off & log->page_mask);
	if (log->major_ver < 2) {
		/* No last_lsn on v1 copies: only a page holding a record end is usable. */
		t->valid = t->valid && t->has_end;
		t->end = t->last_end_lsn;
		t->seq = lfs_lsn_seq(log, t->last_end_lsn);
	} else {
		/* fslog.c base_lsn(): the page belongs to the next sequence when the
		 * last record header on it lies behind it in the file (wrap). */
		t->last_lsn = lf_get64(t->page + PG_LAST_LSN);
		t->end = t->has_end ? t->last_end_lsn : t->last_lsn;
		t->seq = lfs_lsn_seq(log, t->last_lsn) +
			 (t->file_off < (lfs_lsn_to_vbo(log, t->last_lsn) & ~log->page_mask) ? 1 : 0);
	}
}

/*
 * Record what the scan learned from a page that ends a record.
 */
static void note_record_end(ntfs_logfile_t *log, const uint8_t *page, uint32_t cur, uint32_t next,
			    uint64_t seq, bool wrapped_file)
{
	uint32_t nro = lf_get16(page + PG_NEXT_RECORD_OFFSET);

	log->seq_num = seq;
	log->no_last_lsn = false;
	log->last_lsn = lf_get64(page + PG_LAST_END_LSN);
	if (nro <= log->page_size && log->record_header_len <= log->page_size - nro) {
		log->reuse_tail = true;
		log->next_page = cur;
	} else {
		log->reuse_tail = false;
		log->next_page = next;
	}
	if (wrapped_file)
		log->wrapped = true;
}

/* fslog.c check_subseq_log_page(): was @page written after sequence @seq began? */
static bool page_is_subsequent(const ntfs_logfile_t *log, const uint8_t *page, uint32_t vbo, uint64_t seq)
{
	uint64_t lsn = lf_get64(page + PG_LAST_LSN);
	uint64_t s = lfs_lsn_seq(log, lsn);

	return s >= seq ||
	       (s == seq - 1 && log->first_page == vbo &&
		vbo != (lfs_lsn_to_vbo(log, lsn) & ~log->page_mask));
}

/*
 * Walk forward from next_page validating sequence continuity and
 * multi-page transfer bookkeeping, extending last_lsn as pages are
 * confirmed (fslog.c last_log_lsn()). A tail copy of the page being
 * examined stands in for it when the on-disk page is unreadable, or when
 * the copy ends with a later lsn than the page's last record header (the
 * copy was written after the in-place page, so it is the newer state).
 *
 * Heuristic H4 (from fslog.c): once the scan stops at page S, the I/O
 * transfer S belongs to is delimited (from the page position bookkeeping,
 * or from S+1 when S would start a transfer of unknown length) and the page
 * following that transfer must not carry a sequence number >= the one
 * expected there. If it does, the LFS wrote beyond a torn transfer, the log
 * tail cannot be trusted and the function fails with -EINVAL so the caller
 * never replays.
 *
 * Simplification vs fslog.c: v2.0 multi-page tail transfers are matched
 * page by page through file_off instead of being reassembled as one 16-page
 * buffer; a copy without a record end is only used when its sequence and
 * position bookkeeping fit.
 */
int lfs_tail_scan(ntfs_logfile_t *log)
{
	uint8_t *page;
	struct tail_copy tails[2 * 16];
	int ntails = 0, i, err = 0;
	uint32_t cur, next, pages_seen = 0, max_pages;
	uint64_t expected_seq, last_ok_lsn;
	uint32_t page_cnt = 1, page_pos = 1;
	bool wrapped_file, reuse = log->reuse_tail, torn;
	uint32_t stop_cur = 0;
	int stop_remaining = 0;
	bool stop_replaced = false, stop_new_transfer = false;

	if (log->tail_scanned)
		return 0;
	log->tail_scanned = true;
	if (log->state == NTFS_LOG_EMPTY || log->cur_rst < 0)
		return 0;

	page = malloc(log->page_size);
	if (!page)
		return -ENOMEM;
	max_pages = (log->l_size - log->first_page) >> log->page_bits;

	if (log->major_ver < 2) {
		load_tail(log, 2 * log->page_size, &tails[ntails++]);
		load_tail(log, 3 * log->page_size, &tails[ntails++]);
	} else {
		for (i = 0; i < 16; i++)
			load_tail(log, (0x02 + i) * log->page_size, &tails[ntails++]);
		for (i = 0; i < 16; i++)
			load_tail(log, (0x12 + i) * log->page_size, &tails[ntails++]);
	}

	last_ok_lsn = reuse ? log->last_lsn : 0;
	cur = log->next_page;
	wrapped_file = cur == log->first_page && !log->no_last_lsn && !reuse;
	expected_seq = wrapped_file ? log->seq_num + 1 : log->seq_num;

	for (;;) {
		uint64_t lsn_cur;
		bool wrapped, replaced = false, usable;
		struct tail_copy *best = NULL;

		next = lfs_next_page_off(log, cur);
		wrapped = next == log->first_page;

		/* Newest tail copy of this very page from the expected sequence. */
		for (i = 0; i < ntails; i++) {
			struct tail_copy *t = &tails[i];
			if (!t->valid || t->file_off != cur || t->seq != expected_seq)
				continue;
			if (t->end < last_ok_lsn)
				continue;
			if (!best || t->end > best->end)
				best = t;
		}

		err = lfs_read_page(log, cur, page, &torn);
		if (err && err != -EINVAL) {
			free(page);
			return err;
		}
		usable = !err && !torn && page_is_rcrd(page);
		err = 0;
		if (usable && log->major_ver >= 2 && lf_get32(page + PG_FILE_OFFSET) &&
		    lf_get32(page + PG_FILE_OFFSET) != cur)
			usable = false;	/* a stray tail copy sitting in the log area */
		if (best && (!usable || best->end > lf_get64(page + PG_LAST_LSN))) {
			err = add_override(log, cur, best->page);
			if (err)
				goto out;
			for (i = 0; i < log->n_ovr; i++)
				if (log->ovr[i].vbo == cur)
					memcpy(page, log->ovr[i].page, log->page_size);
			lfs_msg(log, 2, "tail copy @%u supersedes %s page @%u", best->vbo,
				usable ? "older" : "unreadable", cur);
			replaced = true;
			usable = true;
		}
		if (!usable) {
			stop_cur = cur;
			stop_remaining = (int)page_cnt - (int)page_pos - 1;
			stop_new_transfer = page_cnt == page_pos;
			break;
		}

		lsn_cur = lf_get64(page + PG_LAST_LSN);
		if (last_ok_lsn != lsn_cur && expected_seq != lfs_lsn_seq(log, lsn_cur)) {
			stop_cur = cur;
			stop_remaining = (int)page_cnt - (int)page_pos - 1;
			stop_new_transfer = page_cnt == page_pos;
			break;
		}
		/* Multi-page transfer bookkeeping. */
		if (page_cnt == page_pos) {
			uint16_t pp = lf_get16(page + PG_PAGE_POSITION);
			if (pp != 1 && (!reuse || pp != lf_get16(page + PG_PAGE_COUNT))) {
				stop_cur = cur;
				stop_remaining = 0;
				stop_new_transfer = true;
				break;
			}
		} else if (lf_get16(page + PG_PAGE_COUNT) != page_cnt ||
			   lf_get16(page + PG_PAGE_POSITION) != page_pos + 1) {
			stop_cur = cur;
			stop_remaining = (int)page_cnt - (int)page_pos - 1;
			break;
		}
		if (lf_get32(page + PG_FLAGS) & LFS_PAGE_LOG_RECORD_END)
			note_record_end(log, page, cur, next, expected_seq, wrapped_file);
		page_cnt = lf_get16(page + PG_PAGE_COUNT);
		page_pos = lf_get16(page + PG_PAGE_POSITION);
		if (!page_cnt || !page_pos || page_pos > page_cnt) {
			stop_cur = cur;
			stop_remaining = 0;
			stop_new_transfer = true;
			break;
		}
		last_ok_lsn = lsn_cur;
		stop_replaced = replaced;
		if (wrapped) {
			expected_seq++;
			wrapped_file = true;
		}
		cur = next;
		reuse = false;
		if (++pages_seen > max_pages) {
			/* Every page validated: the log is full and consistent. */
			stop_cur = 0;
			break;
		}
	}

	if (stop_cur) {
		uint32_t probe = stop_cur;
		uint64_t seq = expected_seq;
		int n, remaining = stop_remaining > 0 ? stop_remaining : 0;

		if (stop_replaced || log->single_page_io) {
			remaining = 0;
		} else if (stop_new_transfer) {
			/* S would start a transfer: S+1 tells its length if it belongs to it. */
			uint32_t nx = lfs_next_page_off(log, stop_cur);
			uint64_t s2 = seq + (nx == log->first_page ? 1 : 0);
			remaining = 0;
			if (nx != log->first_page &&
			    !lfs_read_page(log, nx, page, &torn) && !torn && page_is_rcrd(page) &&
			    page_is_subsequent(log, page, nx, s2)) {
				if (lf_get16(page + PG_PAGE_POSITION) == 2 && lf_get16(page + PG_PAGE_COUNT) >= 2)
					remaining = lf_get16(page + PG_PAGE_COUNT) - 1;
				else {
					err = lfs_seterr(log, -EINVAL,
							 "log tail corrupt: page @%u written after torn page @%u",
							 nx, stop_cur);
					goto out;
				}
			}
		}
		for (n = 0; n <= remaining; n++) {
			uint32_t nx = lfs_next_page_off(log, probe);
			if (nx == log->first_page)
				seq++;
			probe = nx;
		}
		if (!lfs_read_page(log, probe, page, &torn) && !torn && page_is_rcrd(page) &&
		    page_is_subsequent(log, page, probe, seq)) {
			err = lfs_seterr(log, -EINVAL,
					 "log tail corrupt: page @%u (seq %llu) written after torn transfer at @%u",
					 probe, (unsigned long long)lfs_lsn_seq(log, lf_get64(page + PG_LAST_LSN)),
					 stop_cur);
			goto out;
		}
	}
	lfs_msg(log, 2, "tail scan: last_lsn 0x%llx (restart area had 0x%llx), next page @%u%s, %d override(s)",
		(unsigned long long)log->last_lsn, (unsigned long long)log->current_lsn,
		log->next_page, log->reuse_tail ? " (reuse)" : "", log->n_ovr);
out:
	for (i = 0; i < ntails; i++)
		free(tails[i].page);
	free(page);
	return err;
}
