// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
/*
 * Reading individual log records and walking the LSN chains.
 *
 * A record is an LFS_RECORD_HDR (log->record_header_len bytes, 0x30) at
 * the byte offset encoded in its LSN, followed by client_data_len bytes of
 * client data which may continue on following pages (from data_off) and
 * wrap from the end of the file to first_page.
 */
#include "logfile_internal.h"

void lfs_free_record(struct lfs_record *rec)
{
	free(rec->data);
	rec->data = NULL;
	rec->data_len = 0;
}

/*
 * Copy the client data of a record whose header is at @lsn into @buf.
 */
static int read_record_data(ntfs_logfile_t *log, uint64_t lsn, uint32_t data_len, uint8_t *buf)
{
	uint8_t *page;
	uint32_t vbo = lfs_lsn_to_vbo(log, lsn) & ~log->page_mask;
	uint32_t off = (lfs_lsn_to_vbo(log, lsn) & log->page_mask) + log->record_header_len;
	int err;

	page = malloc(log->page_size);
	if (!page)
		return -ENOMEM;
	for (;;) {
		uint32_t tail = log->page_size - off;
		uint64_t page_lsn;

		if (tail >= data_len)
			tail = data_len;
		data_len -= tail;
		err = lfs_read_page(log, vbo, page, NULL);
		if (err)
			goto out;
		if (lf_get32(page + PG_MAGIC) != LFS_MAGIC_RCRD) {
			err = -EINVAL;
			goto out;
		}
		page_lsn = lf_get64(page + PG_LAST_LSN);
		/* The page's last lsn must be >= the record we copy. */
		if (lsn > page_lsn) {
			err = -EINVAL;
			goto out;
		}
		memcpy(buf, page + off, tail);
		buf += tail;
		if (!data_len) {
			if (!(lf_get32(page + PG_FLAGS) & LFS_PAGE_LOG_RECORD_END) ||
			    lsn > lf_get64(page + PG_LAST_END_LSN)) {
				err = -EINVAL;
				goto out;
			}
			break;
		}
		/* More to come: this page must not end with our lsn. */
		if (page_lsn == lf_get64(page + PG_LAST_END_LSN) || lsn > page_lsn) {
			err = -EINVAL;
			goto out;
		}
		vbo = lfs_next_page_off(log, vbo);
		off = log->data_off;
	}
	err = 0;
out:
	free(page);
	return err;
}

int lfs_read_record(ntfs_logfile_t *log, uint64_t lsn, struct lfs_record *rec)
{
	uint8_t *page;
	uint32_t vbo, poff, len, avail;
	uint64_t total_avail;
	int err;

	memset(rec, 0, sizeof(*rec));
	if (!lfs_lsn_in_file(log, lsn))
		return lfs_seterr(log, -EINVAL, "lsn 0x%llx outside [oldest, last]",
				  (unsigned long long)lsn);
	vbo = lfs_lsn_to_vbo(log, lsn);
	if (vbo < log->first_page || vbo + log->record_header_len > log->l_size)
		return lfs_seterr(log, -EINVAL, "lsn 0x%llx maps outside the log area",
				  (unsigned long long)lsn);
	page = malloc(log->page_size);
	if (!page)
		return -ENOMEM;
	err = lfs_read_page(log, vbo, page, NULL);
	if (err) {
		free(page);
		return lfs_seterr(log, err, "cannot read page for lsn 0x%llx", (unsigned long long)lsn);
	}
	if (lf_get32(page + PG_MAGIC) != LFS_MAGIC_RCRD) {
		free(page);
		return lfs_seterr(log, -EINVAL, "lsn 0x%llx: not a record page", (unsigned long long)lsn);
	}
	poff = vbo & log->page_mask;
	if (poff < log->data_off || poff + log->record_header_len > log->page_size) {
		free(page);
		return lfs_seterr(log, -EINVAL, "lsn 0x%llx: header outside data area",
				  (unsigned long long)lsn);
	}
	memcpy(rec->hdr, page + poff, LR_HEADER_SIZE);
	if (lf_get64(rec->hdr + LR_THIS_LSN) != lsn) {
		free(page);
		return lfs_seterr(log, -EINVAL, "lsn 0x%llx: header lsn mismatch (0x%llx)",
				  (unsigned long long)lsn,
				  (unsigned long long)lf_get64(rec->hdr + LR_THIS_LSN));
	}
	len = lf_get32(rec->hdr + LR_CLIENT_DATA_LENGTH);
	total_avail = (uint64_t)((log->l_size - log->first_page) >> log->page_bits) *
		      (log->page_size - log->data_off);
	if ((uint64_t)len + log->record_header_len >= total_avail) {
		free(page);
		return lfs_seterr(log, -EINVAL, "lsn 0x%llx: data length %u exceeds log",
				  (unsigned long long)lsn, len);
	}
	rec->lsn = lsn;
	rec->data_len = len;
	if (!len) {
		free(page);
		return 0;
	}
	/*
	 * A client record can declare a redo or undo payload that begins at or
	 * past its own client_data_length: a real Windows 10 journal carries
	 * client_data_length 40 with 4 bytes of redo at offset 40, and Windows
	 * replays it without complaint. Refusing those records aborts the whole
	 * replay (finding 18).
	 *
	 * The bytes are NOT there to be read. The record at lsn 0x204c8d is 88
	 * bytes -- 48 of header plus the 40 it declares -- and the next record
	 * begins at lsn 0x204c98, exactly 88 bytes later. Reading past the
	 * declared length therefore picks up the *next record's header*. An
	 * earlier version of this code did precisely that and wrote 0x00204c98,
	 * the next record's LSN, into a user's file, where Windows had written
	 * four zero bytes.
	 *
	 * So the buffer is extended to cover what the record references and the
	 * extension is ZERO-FILLED. That reproduces Windows byte for byte, keeps
	 * lfs_check_client_rec()'s bound honest, and never reads a neighbour.
	 */
	avail = len;
	if (len >= NR_HEADER_SIZE &&
	    lf_get32(rec->hdr + LR_RECORD_TYPE) == LFS_RECORD_TYPE_CLIENT &&
	    poff + log->record_header_len + NR_HEADER_SIZE <= log->page_size) {
		const uint8_t *lr = page + poff + log->record_header_len;
		uint32_t r = (uint32_t)lf_get16(lr + NR_REDO_OFFSET) + lf_get16(lr + NR_REDO_LENGTH);
		uint32_t u = (uint32_t)lf_get16(lr + NR_UNDO_OFFSET) + lf_get16(lr + NR_UNDO_LENGTH);

		if (r > avail)
			avail = r;
		if (u > avail)
			avail = u;
		if ((uint64_t)avail + log->record_header_len >= total_avail)
			avail = len;		/* implausible; leave it to the validator */
	}
	rec->data_avail = avail;
	rec->data = malloc(avail);
	if (!rec->data) {
		free(page);
		return -ENOMEM;
	}
	if (avail > len)
		memset(rec->data + len, 0, avail - len);
	if (lf_get16(rec->hdr + LR_FLAGS) & LFS_RECORD_MULTI_PAGE) {
		err = read_record_data(log, lsn, len, rec->data);
	} else {
		if (poff + log->record_header_len + len > log->page_size) {
			err = -EINVAL;
		} else {
			memcpy(rec->data, page + poff + log->record_header_len, len);
			err = 0;
		}
	}
	free(page);
	if (err) {
		lfs_free_record(rec);
		return lfs_seterr(log, err, "lsn 0x%llx: cannot read record data",
				  (unsigned long long)lsn);
	}
	return 0;
}

/*
 * LSN of the record following @rec in file order, or 0 when the end of
 * the log is reached.
 */
int lfs_next_lsn(ntfs_logfile_t *log, const struct lfs_record *rec, uint64_t *next)
{
	uint64_t this_lsn = rec->lsn;
	uint32_t vbo = lfs_lsn_to_vbo(log, this_lsn);
	uint32_t end = lfs_final_log_off(log, this_lsn, rec->data_len);
	uint32_t hdr_off = end & ~log->page_mask;
	uint64_t seq = lfs_lsn_seq(log, this_lsn);
	uint8_t *page;
	int err;

	*next = 0;
	if (end <= vbo)
		seq++;
	page = malloc(log->page_size);
	if (!page)
		return -ENOMEM;
	err = lfs_read_page(log, hdr_off, page, NULL);
	if (err) {
		free(page);
		return err;
	}
	if (this_lsn == lf_get64(page + PG_LAST_LSN)) {
		/* Last record starting on this page: next begins on next page. */
		hdr_off = lfs_next_page_off(log, hdr_off);
		if (hdr_off == log->first_page)
			seq++;
		vbo = hdr_off + log->data_off;
	} else {
		vbo = LF_ALIGN8(end);
	}
	free(page);
	*next = lfs_vbo_to_lsn(log, vbo, seq);
	if (!lfs_lsn_in_file(log, *next))
		*next = 0;
	return 0;
}

/* Bit table: which ops require a target attribute (from fslog.c). */
static const uint8_t attr_required[5] = { 0xfc, 0xfb, 0xff, 0x10, 0x06 };

static bool op_needs_target(uint16_t op)
{
	return op <= LOP_UpdateRecordDataAllocation && ((attr_required[op >> 3] >> (op & 7)) & 1);
}

/* Sanity checks on an NTFS client record before it is interpreted. */
/*
 * Validate a client record's framing.
 *
 * @why, when non-NULL, receives which check failed. That is not a nicety: on
 * 2026-09-15 this function rejected a record in a real Windows 10 journal that
 * Windows itself then replayed without complaint, and "malformed client record"
 * was all the evidence there was. A validator that can be wrong has to say how.
 */
bool lfs_check_client_rec(const struct lfs_record *rec, uint32_t bytes_per_attr_entry,
			  const char **why)
{
	const uint8_t *lr = rec->data;
	uint16_t redo_off, redo_len, undo_off, undo_len, lcns, target;
	uint32_t hdr_len;

#define REJECT(reason) do { if (why) *why = (reason); return false; } while (0)

	if (why)
		*why = NULL;
	if (rec->data_len < NR_HEADER_SIZE)
		REJECT("shorter than the record header");
	redo_off = lf_get16(lr + NR_REDO_OFFSET);
	redo_len = lf_get16(lr + NR_REDO_LENGTH);
	undo_off = lf_get16(lr + NR_UNDO_OFFSET);
	undo_len = lf_get16(lr + NR_UNDO_LENGTH);
	lcns = lf_get16(lr + NR_LCNS_TO_FOLLOW);
	target = lf_get16(lr + NR_TARGET_ATTRIBUTE);
	if ((redo_off & 7) || (undo_off & 7))
		REJECT("redo or undo offset is not 8-byte aligned");
	if (!target) {
		if (op_needs_target(lf_get16(lr + NR_REDO_OP)) ||
		    op_needs_target(lf_get16(lr + NR_UNDO_OP)))
			REJECT("no target attribute, but the operation needs one");
	}
	if (lcns && target && bytes_per_attr_entry &&
	    (target < RT_HEADER_SIZE || (target - RT_HEADER_SIZE) % bytes_per_attr_entry))
		REJECT("target attribute is not on an open-attribute-table boundary");
	hdr_len = NR_HEADER_SIZE + 8u * (lcns ? lcns : 1);
	if (rec->data_len < hdr_len)
		REJECT("too short for the LCNs it declares");
	/* Against what was actually read. The declared client_data_length is not
	 * an upper bound on what a record references -- see struct lfs_record. */
	if (redo_len && (uint32_t)redo_off + redo_len > rec->data_avail)
		REJECT("redo data runs past the end of the record");
	if (undo_len && (uint32_t)undo_off + undo_len > rec->data_avail)
		REJECT("undo data runs past the end of the record");
	return true;

#undef REJECT
}

int lfs_decode_record(const struct lfs_record *rec, struct ntfs_log_record *out)
{
	const uint8_t *h = rec->hdr;
	const uint8_t *lr = rec->data;

	memset(out, 0, sizeof(*out));
	out->lsn = rec->lsn;
	out->client_prev_lsn = lf_get64(h + LR_CLIENT_PREV_LSN);
	out->client_undo_next_lsn = lf_get64(h + LR_CLIENT_UNDO_NEXT_LSN);
	out->transaction_id = lf_get32(h + LR_TRANSACTION_ID);
	out->record_type = lf_get32(h + LR_RECORD_TYPE);
	out->flags = lf_get16(h + LR_FLAGS);
	out->client_seq = lf_get16(h + LR_CLIENT_SEQ_NUMBER);
	out->client_idx = lf_get16(h + LR_CLIENT_INDEX);
	out->data_len = rec->data_len;
	out->raw = rec->data;
	if (out->record_type != LFS_RECORD_TYPE_CLIENT || rec->data_len < NR_HEADER_SIZE)
		return 0;
	out->hdr_len = NR_HEADER_SIZE + 8u * (lf_get16(lr + NR_LCNS_TO_FOLLOW) ? lf_get16(lr + NR_LCNS_TO_FOLLOW) : 1);
	out->redo_op = lf_get16(lr + NR_REDO_OP);
	out->undo_op = lf_get16(lr + NR_UNDO_OP);
	out->redo_len = lf_get16(lr + NR_REDO_LENGTH);
	out->undo_len = lf_get16(lr + NR_UNDO_LENGTH);
	out->target_attr = lf_get16(lr + NR_TARGET_ATTRIBUTE);
	out->lcns_follow = lf_get16(lr + NR_LCNS_TO_FOLLOW);
	out->record_off = lf_get16(lr + NR_RECORD_OFFSET);
	out->attr_off = lf_get16(lr + NR_ATTRIBUTE_OFFSET);
	out->cluster_off = lf_get16(lr + NR_CLUSTER_BLOCK_OFFSET);
	out->target_vcn = lf_get64(lr + NR_TARGET_VCN);
	out->page_lcns = (const uint64_t *)(lr + NR_PAGE_LCNS);
	if (out->redo_len && lf_get16(lr + NR_REDO_OFFSET) + out->redo_len <= rec->data_len)
		out->redo = lr + lf_get16(lr + NR_REDO_OFFSET);
	if (out->undo_len && lf_get16(lr + NR_UNDO_OFFSET) + out->undo_len <= rec->data_len)
		out->undo = lr + lf_get16(lr + NR_UNDO_OFFSET);
	return 0;
}

/* ---- Public walk ------------------------------------------------------ */

int ntfs_logfile_walk(ntfs_logfile_t *log, uint64_t from_lsn, ntfs_log_record_cb cb, void *ctx)
{
	struct lfs_record rec;
	struct ntfs_log_record info;
	uint64_t lsn;
	int err;

	if (log->state == NTFS_LOG_EMPTY)
		return 0;
	if (log->state == NTFS_LOG_CORRUPT || log->state == NTFS_LOG_UNSUPPORTED)
		return -EINVAL;
	err = lfs_tail_scan(log);
	if (err)
		return err;
	if (!log->crst && !log->checkpoint_lsn) {
		err = lfs_load_checkpoint(log);
		if (err && err != -ENOENT)
			return err;
	}
	lsn = from_lsn ? from_lsn : (log->checkpoint_lsn ? log->checkpoint_lsn : log->client_restart_lsn);
	if (!lsn)
		return 0;
	while (lsn) {
		uint64_t next;

		err = lfs_read_record(log, lsn, &rec);
		if (err)
			return err;
		lfs_decode_record(&rec, &info);
		if (cb(&info, ctx)) {
			lfs_free_record(&rec);
			return 0;
		}
		err = lfs_next_lsn(log, &rec, &next);
		lfs_free_record(&rec);
		if (err)
			return err;
		lsn = next;
	}
	return 0;
}
