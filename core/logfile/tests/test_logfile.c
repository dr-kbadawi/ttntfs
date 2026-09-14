// SPDX-License-Identifier: GPL-2.0
/*
 * Unit tests for the $LogFile module, built from synthetic pages.
 *
 * The builder writes a version 1.1 log the way the LFS lays it out:
 * restart pages at 0 and 1, tail-copy pages at 2 and 3, record pages from
 * page 4, records made of a 0x30-byte LFS header followed by client data
 * that may continue on the next page (from data_off) and wrap to the first
 * record page with the sequence number incremented. See docs/LOGFILE.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <inttypes.h>
#include "ntfs_logfile.h"
#include "logfile_layout.h"
#include "ntfs_image.h"

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { checks++; unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); \
	if (_a != _b) { failures++; fprintf(stderr, "FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n", \
	__FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static void logger(int level, const char *m, void *ctx)
{
	if (getenv("LOGTEST_VERBOSE") || level == 0)
		fprintf(stderr, "    [%d] %s\n", level, m);
}

/* ---- Synthetic log builder --------------------------------------------- */

#define PS 4096u
#define NPAGES 64u			/* 256 KiB: file_data_bits = 16 */
#define LSIZE (PS * NPAGES)
#define DATA_OFF 0x40u
#define FDB 16u				/* file data bits */
#define SEQ_BITS (64 - FDB)

/* Log version the builder emits. v1.x: record pages from page 4, tail
 * copies at 2 and 3. v2.0: record pages from page 0x22, tail copies at
 * 0x02.. and 0x12.., page header carries file_off at 0x3c. */
static uint16_t log_major = 1, log_minor = 1;
static uint32_t first_page = 4 * PS;
#define FIRST_PAGE first_page

static void lb_set_version(uint16_t major, uint16_t minor)
{
	log_major = major;
	log_minor = minor;
	first_page = (major >= 2 ? 0x22u : 4u) * PS;
}

struct lb {
	uint8_t *buf;
	uint32_t page;			/* current page offset */
	uint32_t off;			/* next free byte in page */
	uint64_t seq;
	uint64_t last_lsn;		/* lsn of last record appended */
	uint32_t last_len;
	uint64_t prev_lsn;		/* for client_prev_lsn chaining */
	bool wrapped;
};

static uint64_t mk_lsn(uint32_t vbo, uint64_t seq) { return (vbo >> 3) + (seq << FDB); }

static void lb_page_init(struct lb *b, uint32_t page)
{
	uint8_t *p = b->buf + page;
	memset(p, 0, PS);
	lf_put32(p + PG_MAGIC, LFS_MAGIC_RCRD);
	lf_put16(p + PG_USA_OFS, PG_USA);
	lf_put16(p + PG_USA_COUNT, PS / 512 + 1);
	lf_put16(p + PG_PAGE_COUNT, 1);
	lf_put16(p + PG_PAGE_POSITION, 1);
	lf_put16(p + PG_NEXT_RECORD_OFFSET, DATA_OFF);
}

static void lb_init(struct lb *b, uint32_t start_page, uint64_t seq)
{
	memset(b, 0, sizeof(*b));
	b->buf = malloc(LSIZE);
	memset(b->buf, 0xff, LSIZE);
	b->page = start_page;
	b->off = DATA_OFF;
	b->seq = seq;
	lb_page_init(b, b->page);
}

static void lb_next_page(struct lb *b)
{
	b->page += PS;
	if (b->page >= LSIZE) {
		b->page = FIRST_PAGE;
		b->seq++;
		b->wrapped = true;
	}
	lb_page_init(b, b->page);
	b->off = DATA_OFF;
}

/* Append a record; returns its lsn. hdr: prev/undo_next/type/tid/client. */
static uint64_t lb_append(struct lb *b, uint32_t type, uint32_t tid, uint64_t undo_next,
			  const void *data, uint32_t len)
{
	uint8_t *p;
	uint64_t lsn;
	uint32_t left = len, first_page;
	const uint8_t *d = data;
	bool multi;

	if (b->off + LR_HEADER_SIZE > PS)
		lb_next_page(b);
	first_page = b->page;
	lsn = mk_lsn(b->page + b->off, b->seq);
	p = b->buf + b->page + b->off;
	memset(p, 0, LR_HEADER_SIZE);
	lf_put64(p + LR_THIS_LSN, lsn);
	lf_put64(p + LR_CLIENT_PREV_LSN, b->prev_lsn);
	lf_put64(p + LR_CLIENT_UNDO_NEXT_LSN, undo_next);
	lf_put32(p + LR_CLIENT_DATA_LENGTH, len);
	lf_put16(p + LR_CLIENT_SEQ_NUMBER, 0);
	lf_put16(p + LR_CLIENT_INDEX, 0);
	lf_put32(p + LR_RECORD_TYPE, type);
	lf_put32(p + LR_TRANSACTION_ID, tid);
	multi = b->off + LR_HEADER_SIZE + len > PS;
	lf_put16(p + LR_FLAGS, multi ? LFS_RECORD_MULTI_PAGE : 0);
	lf_put64(b->buf + b->page + PG_LAST_LSN, lsn);
	b->off += LR_HEADER_SIZE;
	for (;;) {
		uint32_t n = PS - b->off;
		if (n > left)
			n = left;
		memcpy(b->buf + b->page + b->off, d, n);
		d += n;
		left -= n;
		b->off += n;
		if (!left)
			break;
		/* continue on the next page; that page has no record header yet */
		lf_put16(b->buf + b->page + PG_NEXT_RECORD_OFFSET, (uint16_t)PS);
		lb_next_page(b);
		/* pages without a record of their own still carry the spanning
		 * record's lsn as last_lsn (the reader checks lsn <= last_lsn) */
		lf_put64(b->buf + b->page + PG_LAST_LSN, lsn);
	}
	/* record ends on b->page */
	lf_put32(b->buf + b->page + PG_FLAGS, LFS_PAGE_LOG_RECORD_END);
	lf_put64(b->buf + b->page + PG_LAST_END_LSN, lsn);
	b->off = LF_ALIGN8(b->off);
	lf_put16(b->buf + b->page + PG_NEXT_RECORD_OFFSET, (uint16_t)(b->off > PS ? PS : b->off));
	(void)first_page;
	b->prev_lsn = lsn;
	b->last_lsn = lsn;
	b->last_len = len;
	return lsn;
}

/* NTFS client record. Redo/undo data appended after the header + lcns. */
struct crec {
	uint16_t redo_op, undo_op, target_attr, lcns;
	uint16_t record_off, attr_off, cluster_off;
	uint64_t target_vcn;
	uint64_t lcn[4];
	const void *redo; uint16_t redo_len;
	const void *undo; uint16_t undo_len;
};

static uint32_t build_crec(const struct crec *c, uint8_t *out)
{
	uint32_t hdr = NR_HEADER_SIZE + 8u * (c->lcns ? c->lcns : 1), o = hdr, i;

	memset(out, 0, hdr);
	lf_put16(out + NR_REDO_OP, c->redo_op);
	lf_put16(out + NR_UNDO_OP, c->undo_op);
	lf_put16(out + NR_TARGET_ATTRIBUTE, c->target_attr);
	lf_put16(out + NR_LCNS_TO_FOLLOW, c->lcns);
	lf_put16(out + NR_RECORD_OFFSET, c->record_off);
	lf_put16(out + NR_ATTRIBUTE_OFFSET, c->attr_off);
	lf_put16(out + NR_CLUSTER_BLOCK_OFFSET, c->cluster_off);
	lf_put64(out + NR_TARGET_VCN, c->target_vcn);
	for (i = 0; i < c->lcns; i++)
		lf_put64(out + NR_PAGE_LCNS + 8 * i, c->lcn[i]);
	if (c->redo_len) {
		lf_put16(out + NR_REDO_OFFSET, (uint16_t)o);
		lf_put16(out + NR_REDO_LENGTH, c->redo_len);
		memcpy(out + o, c->redo, c->redo_len);
		o = LF_ALIGN8(o + c->redo_len);
	} else {
		lf_put16(out + NR_REDO_OFFSET, (uint16_t)o);
	}
	if (c->undo_len) {
		lf_put16(out + NR_UNDO_OFFSET, (uint16_t)o);
		lf_put16(out + NR_UNDO_LENGTH, c->undo_len);
		memcpy(out + o, c->undo, c->undo_len);
		o = LF_ALIGN8(o + c->undo_len);
	} else {
		lf_put16(out + NR_UNDO_OFFSET, (uint16_t)o);
	}
	return o;
}

static uint64_t lb_client(struct lb *b, uint32_t tid, uint64_t undo_next, const struct crec *c)
{
	uint8_t tmp[8192];
	uint32_t n = build_crec(c, tmp);
	return lb_append(b, LFS_RECORD_TYPE_CLIENT, tid, undo_next, tmp, n);
}

static uint64_t lb_simple(struct lb *b, uint32_t tid, uint16_t redo_op, uint16_t undo_op)
{
	struct crec c;
	memset(&c, 0, sizeof(c));
	c.redo_op = redo_op;
	c.undo_op = undo_op;
	return lb_client(b, tid, 0, &c);
}

/* Restart record (checkpoint). Returns its lsn. */
static uint64_t lb_checkpoint(struct lb *b, uint64_t start, uint64_t oatbl_lsn, uint32_t oatbl_len,
			      uint64_t dptbl_lsn, uint32_t dptbl_len, uint64_t trtbl_lsn, uint32_t trtbl_len)
{
	uint8_t c[CRST_SIZE];
	memset(c, 0, sizeof(c));
	lf_put32(c + CRST_MAJOR_VER, 1);
	lf_put64(c + CRST_START_OF_CHECKPOINT, start);
	lf_put64(c + CRST_OPEN_ATTR_TABLE_LSN, oatbl_lsn);
	lf_put32(c + CRST_OPEN_ATTR_TABLE_LEN, oatbl_len);
	lf_put64(c + CRST_DIRTY_PAGE_TABLE_LSN, dptbl_lsn);
	lf_put32(c + CRST_DIRTY_PAGE_TABLE_LEN, dptbl_len);
	lf_put64(c + CRST_TRANSACTION_TABLE_LSN, trtbl_lsn);
	lf_put32(c + CRST_TRANSACTION_TABLE_LEN, trtbl_len);
	return lb_append(b, LFS_RECORD_TYPE_CLIENT_RESTART, 0, 0, c, sizeof(c));
}

/* Open attribute table with two v1 entries: 0x18 $MFT:$DATA, 0x40 target. */
static uint32_t build_oatbl(uint8_t *t, uint64_t mft2, uint32_t type2)
{
	uint32_t n = RT_HEADER_SIZE + 2 * OA1_SIZE;
	uint8_t *e;
	memset(t, 0, n);
	lf_put16(t + RT_ENTRY_SIZE, OA1_SIZE);
	lf_put16(t + RT_NUMBER_ENTRIES, 2);
	lf_put16(t + RT_NUMBER_ALLOCATED, 2);
	lf_put32(t + RT_FREE_GOAL, 0xffffffffu);
	e = t + RT_HEADER_SIZE;
	lf_put32(e + OA1_NEXT, RT_ENTRY_ALLOCATED);
	lf_put32(e + OA1_TYPE, ATTR_TYPE_DATA);
	lf_put32(e + OA1_FILE_REF, 0);
	lf_put16(e + OA1_FILE_REF + 6, 1);
	e += OA1_SIZE;
	lf_put32(e + OA1_NEXT, RT_ENTRY_ALLOCATED);
	lf_put32(e + OA1_TYPE, type2);
	lf_put32(e + OA1_FILE_REF, (uint32_t)mft2);
	lf_put16(e + OA1_FILE_REF + 6, 1);
	return n;
}

static uint64_t lb_table_dump(struct lb *b, uint16_t op, const uint8_t *t, uint32_t n)
{
	struct crec c;
	memset(&c, 0, sizeof(c));
	c.redo_op = op;
	c.undo_op = LOP_Noop;
	c.redo = t;
	c.redo_len = (uint16_t)n;
	return lb_client(b, 0x18, 0, &c);
}

/* Write the two restart pages. */
static void lb_restart(struct lb *b, uint64_t current_lsn, uint32_t last_len, uint64_t oldest,
		       uint64_t client_restart, uint16_t flags, bool closed)
{
	int i;
	for (i = 0; i < 2; i++) {
		uint8_t *p = b->buf + i * PS, *ra, *cr;
		memset(p, 0, PS);
		lf_put32(p + RP_MAGIC, LFS_MAGIC_RSTR);
		lf_put16(p + RP_USA_OFS, RP_HEADER_SIZE);
		lf_put16(p + RP_USA_COUNT, PS / 512 + 1);
		lf_put32(p + RP_SYSTEM_PAGE_SIZE, PS);
		lf_put32(p + RP_LOG_PAGE_SIZE, PS);
		lf_put16(p + RP_RESTART_AREA_OFFSET, 0x30);
		lf_put16(p + RP_MINOR_VER, log_minor);
		lf_put16(p + RP_MAJOR_VER, log_major);
		ra = p + 0x30;
		lf_put64(ra + RA_CURRENT_LSN, current_lsn);
		lf_put16(ra + RA_LOG_CLIENTS, 1);
		lf_put16(ra + RA_CLIENT_FREE_LIST, closed ? 0 : LFS_NO_CLIENT);
		lf_put16(ra + RA_CLIENT_IN_USE_LIST, closed ? LFS_NO_CLIENT : 0);
		lf_put16(ra + RA_FLAGS, flags);
		lf_put32(ra + RA_SEQ_NUMBER_BITS, SEQ_BITS);	/* 67 - 19 */
		lf_put16(ra + RA_RESTART_AREA_LENGTH, RA_SIZE_XP + CR_SIZE);
		lf_put16(ra + RA_CLIENT_ARRAY_OFFSET, RA_SIZE_XP);
		lf_put64(ra + RA_FILE_SIZE, LSIZE);
		lf_put32(ra + RA_LAST_LSN_DATA_LENGTH, last_len);
		lf_put16(ra + RA_LOG_RECORD_HEADER_LENGTH, LR_HEADER_SIZE);
		lf_put16(ra + RA_LOG_PAGE_DATA_OFFSET, DATA_OFF);
		lf_put32(ra + RA_RESTART_LOG_OPEN_COUNT, 7);
		cr = ra + RA_SIZE_XP;
		lf_put64(cr + CR_OLDEST_LSN, oldest);
		lf_put64(cr + CR_CLIENT_RESTART_LSN, client_restart);
		lf_put16(cr + CR_PREV_CLIENT, LFS_NO_CLIENT);
		lf_put16(cr + CR_NEXT_CLIENT, LFS_NO_CLIENT);
		lf_put32(cr + CR_CLIENT_NAME_LENGTH, 8);
		lf_put16(cr + CR_CLIENT_NAME + 0, 'N');
		lf_put16(cr + CR_CLIENT_NAME + 2, 'T');
		lf_put16(cr + CR_CLIENT_NAME + 4, 'F');
		lf_put16(cr + CR_CLIENT_NAME + 6, 'S');
		lf_put16(p + RP_HEADER_SIZE, (uint16_t)(0x100 + i));
		ntfs_log_fixup_pre_write(p, PS, 512);
	}
}

/* Protect all record pages (in place, once, after building). */
static void lb_protect(struct lb *b)
{
	uint32_t pg;
	for (pg = 2 * PS; pg < LSIZE; pg += PS) {
		uint8_t *p = b->buf + pg;
		if (lf_get32(p) == LFS_MAGIC_RCRD) {
			lf_put16(p + PG_USA, (uint16_t)(1 + pg / PS));
			CHECK(ntfs_log_fixup_pre_write(p, PS, 512) == 0);
		}
	}
}

/* Make a (deprotected) copy of record page @page, append one Noop record
 * to it and store it as a v1 tail copy at page 2. Returns the new lsn. */
static uint64_t lb_make_tail_copy(struct lb *b, uint32_t page, uint64_t seq)
{
	uint8_t *copy = b->buf + 2 * PS, *src = b->buf + page;
	uint8_t tmp[64];
	struct crec c;
	uint32_t n, off;
	uint64_t lsn;

	memcpy(copy, src, PS);
	off = lf_get16(copy + PG_NEXT_RECORD_OFFSET);
	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_Noop;
	c.undo_op = LOP_Noop;
	n = build_crec(&c, tmp);
	lsn = mk_lsn(page + off, seq);
	memset(copy + off, 0, LR_HEADER_SIZE);
	lf_put64(copy + off + LR_THIS_LSN, lsn);
	lf_put64(copy + off + LR_CLIENT_PREV_LSN, lf_get64(copy + PG_LAST_END_LSN));
	lf_put32(copy + off + LR_CLIENT_DATA_LENGTH, n);
	lf_put32(copy + off + LR_RECORD_TYPE, LFS_RECORD_TYPE_CLIENT);
	lf_put32(copy + off + LR_TRANSACTION_ID, 0x18);
	memcpy(copy + off + LR_HEADER_SIZE, tmp, n);
	lf_put64(copy + PG_LAST_END_LSN, lsn);
	lf_put16(copy + PG_NEXT_RECORD_OFFSET, (uint16_t)LF_ALIGN8(off + LR_HEADER_SIZE + n));
	if (log_major >= 2) {
		/* v2 tail copies keep last_lsn and name the page in file_off */
		lf_put64(copy + PG_LAST_LSN, lsn);
		lf_put32(copy + PG_FILE_OFFSET, page);
	} else {
		/* v1 tail copies carry the page's file offset in the last_lsn field */
		lf_put64(copy + PG_LAST_LSN, page);
	}
	return lsn;
}

/* ---- Memory-backed io ------------------------------------------------- */

struct memio {
	uint8_t *buf;
	uint64_t size;
	int writes;
};

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct memio *m = ctx;
	if (off + len > m->size)
		return -EIO;
	memcpy(buf, m->buf + off, len);
	return 0;
}

static int mem_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
	struct memio *m = ctx;
	if (off + len > m->size)
		return -EIO;
	memcpy(m->buf + off, buf, len);
	m->writes++;
	return 0;
}

static ntfs_logfile_t *open_log(struct memio *m, struct ntfs_log_io *io)
{
	struct ntfs_log_geometry g = { 4096, 1024, 512 };
	ntfs_logfile_t *log = NULL;
	memset(io, 0, sizeof(*io));
	io->ctx = m;
	io->size = m->size;
	io->read = mem_read;
	io->write = mem_write;
	CHECK_EQ(ntfs_logfile_open(io, &g, &log), 0);
	if (log) {
		struct ntfs_logfile_info info;
		ntfs_logfile_set_logger(log, logger, NULL);
		ntfs_logfile_get_info(log, &info);
		if (info.state == NTFS_LOG_CORRUPT || info.state == NTFS_LOG_UNSUPPORTED)
			fprintf(stderr, "    open: state %d: %s\n", info.state, ntfs_logfile_last_error(log));
	}
	return log;
}

/* ---- Fake volume -------------------------------------------------------- */

#define VCS 4096u
#define VRS 1024u
#define MFT_LCN 16u			/* 16 clusters = 64 records */
#define MFT_CLUSTERS 16u
#define BITMAP_LCN 40u
#define VOL_CLUSTERS 64u

struct vol {
	uint8_t *disk;
	int mft_writes, clu_writes, syncs;
	int sync_errno;		/* non-zero: the device refuses the barrier */
	int write_errno;	/* non-zero: the device refuses the writes themselves */
	uint64_t last_mft_no;
};

static void put_nonres_attr(uint8_t *rec, uint32_t *off, uint32_t type, uint64_t lcn, uint64_t clusters, uint16_t id)
{
	uint8_t *a = rec + *off;
	uint8_t mp[16];
	uint32_t mplen = 0, len;

	mp[mplen++] = 0x41;	/* 1 byte length, 4 byte lcn */
	mp[mplen++] = (uint8_t)clusters;
	mp[mplen++] = (uint8_t)lcn;
	mp[mplen++] = (uint8_t)(lcn >> 8);
	mp[mplen++] = (uint8_t)(lcn >> 16);
	mp[mplen++] = (uint8_t)(lcn >> 24);
	mp[mplen++] = 0;
	len = LF_ALIGN8(AT_NONRESIDENT_SIZE + mplen);
	memset(a, 0, len);
	lf_put32(a + AT_TYPE, type);
	lf_put32(a + AT_LENGTH, len);
	a[AT_NON_RESIDENT] = 1;
	lf_put16(a + AT_NAME_OFFSET, AT_NONRESIDENT_SIZE);
	lf_put16(a + AT_INSTANCE, id);
	lf_put64(a + AT_HIGHEST_VCN, clusters - 1);
	lf_put16(a + AT_MAPPING_PAIRS_OFFSET, AT_NONRESIDENT_SIZE);
	lf_put64(a + AT_ALLOCATED_SIZE, clusters * VCS);
	lf_put64(a + AT_DATA_SIZE, clusters * VCS);
	lf_put64(a + AT_INITIALIZED_SIZE, clusters * VCS);
	memcpy(a + AT_NONRESIDENT_SIZE, mp, mplen);
	*off += len;
}

static void put_res_attr(uint8_t *rec, uint32_t *off, uint32_t type, const void *val, uint32_t vlen, uint16_t id)
{
	uint8_t *a = rec + *off;
	uint32_t len = LF_ALIGN8(AT_RESIDENT_SIZE + vlen);
	memset(a, 0, len);
	lf_put32(a + AT_TYPE, type);
	lf_put32(a + AT_LENGTH, len);
	lf_put16(a + AT_NAME_OFFSET, AT_RESIDENT_SIZE);
	lf_put16(a + AT_INSTANCE, id);
	lf_put32(a + AT_VALUE_LENGTH, vlen);
	lf_put16(a + AT_VALUE_OFFSET, AT_RESIDENT_SIZE);
	memcpy(a + AT_RESIDENT_SIZE, val, vlen);
	*off += len;
}

/* Build a deprotected FILE record with the given attributes appended by @fill. */
static void build_record(uint8_t *rec, uint64_t no, uint16_t flags, void (*fill)(uint8_t *, uint32_t *))
{
	uint32_t off = MR_FIXUP_OFFSET_3 + 8;	/* usa 0x30..0x36, attrs at 0x38 */
	memset(rec, 0, VRS);
	lf_put32(rec + MR_MAGIC, LFS_MAGIC_FILE);
	lf_put16(rec + MR_USA_OFS, MR_FIXUP_OFFSET_3);
	lf_put16(rec + MR_USA_COUNT, VRS / 512 + 1);
	lf_put16(rec + MR_SEQUENCE, 1);
	lf_put16(rec + MR_LINK_COUNT, 1);
	lf_put16(rec + MR_ATTRS_OFFSET, (uint16_t)off);
	lf_put16(rec + MR_FLAGS, flags);
	lf_put32(rec + MR_BYTES_ALLOCATED, VRS);
	lf_put32(rec + MR_RECORD_NUMBER, (uint32_t)no);
	lf_put16(rec + MR_FIXUP_OFFSET_3, 0x0001);
	fill(rec, &off);
	lf_put32(rec + off, ATTR_TYPE_END);
	off += 8;
	lf_put32(rec + MR_BYTES_IN_USE, off);
	lf_put16(rec + MR_NEXT_ATTR_ID, 4);
}

static void fill_mft(uint8_t *rec, uint32_t *off) { put_nonres_attr(rec, off, ATTR_TYPE_DATA, MFT_LCN, MFT_CLUSTERS, 1); }
static void fill_bitmap(uint8_t *rec, uint32_t *off) { put_nonres_attr(rec, off, ATTR_TYPE_DATA, BITMAP_LCN, 1, 1); }
static void fill_plain(uint8_t *rec, uint32_t *off)
{
	static const uint8_t si[0x30] = { 1, 2, 3 };
	put_res_attr(rec, off, ATTR_TYPE_STANDARD_INFORMATION, si, sizeof(si), 1);
}

static void vol_put_record(struct vol *v, uint64_t no, const uint8_t *deprot)
{
	uint8_t *dst = v->disk + MFT_LCN * VCS + no * VRS;
	memcpy(dst, deprot, VRS);
	ntfs_log_fixup_pre_write(dst, VRS, 512);
}

static void vol_init(struct vol *v)
{
	uint8_t rec[VRS];
	memset(v, 0, sizeof(*v));
	v->disk = calloc(VOL_CLUSTERS, VCS);
	build_record(rec, 0, MFT_RECORD_IN_USE, fill_mft);
	vol_put_record(v, 0, rec);
	build_record(rec, 6, MFT_RECORD_IN_USE, fill_bitmap);
	vol_put_record(v, 6, rec);
	build_record(rec, 30, MFT_RECORD_IN_USE, fill_plain);
	vol_put_record(v, 30, rec);
}

static int v_read_mft(void *ctx, uint64_t no, void *buf)
{
	struct vol *v = ctx;
	if (no >= MFT_CLUSTERS * VCS / VRS)
		return -ENOENT;
	memcpy(buf, v->disk + MFT_LCN * VCS + no * VRS, VRS);
	return 0;
}
static int v_write_mft(void *ctx, uint64_t no, const void *buf)
{
	struct vol *v = ctx;
	if (no >= MFT_CLUSTERS * VCS / VRS)
		return -ENOENT;
	memcpy(v->disk + MFT_LCN * VCS + no * VRS, buf, VRS);
	v->mft_writes++;
	v->last_mft_no = no;
	return 0;
}
static int v_read_clu(void *ctx, uint64_t lcn, uint32_t n, void *buf)
{
	struct vol *v = ctx;
	if (v->write_errno)
		return v->write_errno;
	if (lcn + n > VOL_CLUSTERS)
		return -EIO;
	memcpy(buf, v->disk + lcn * VCS, (size_t)n * VCS);
	return 0;
}
static int v_write_clu(void *ctx, uint64_t lcn, uint32_t n, const void *buf)
{
	struct vol *v = ctx;
	if (lcn + n > VOL_CLUSTERS)
		return -EIO;
	memcpy(v->disk + lcn * VCS, buf, (size_t)n * VCS);
	v->clu_writes += (int)n;
	return 0;
}
static int v_sync(void *ctx)
{
	struct vol *v = ctx;
	v->syncs++;
	return v->sync_errno;
}

static void vol_apply(struct vol *v, struct ntfs_log_apply *ap)
{
	memset(ap, 0, sizeof(*ap));
	ap->ctx = v;
	ap->read_mft_record = v_read_mft;
	ap->write_mft_record = v_write_mft;
	ap->read_clusters = v_read_clu;
	ap->write_clusters = v_write_clu;
	ap->sync = v_sync;
}

/* ---- Tests -------------------------------------------------------------- */

struct rec_list { uint64_t lsn[256]; uint16_t redo[256]; uint32_t len[256]; unsigned n; uint32_t last_data_crc; };

static int collect(const struct ntfs_log_record *r, void *ctx)
{
	struct rec_list *l = ctx;
	if (l->n < 256) {
		l->lsn[l->n] = r->lsn;
		l->redo[l->n] = r->redo_op;
		l->len[l->n] = r->data_len;
		if (r->record_type == LFS_RECORD_TYPE_CLIENT && r->redo && r->redo_len) {
			uint32_t i, c = 0;
			for (i = 0; i < r->redo_len; i++)
				c = c * 31 + ((const uint8_t *)r->redo)[i];
			l->last_data_crc = c;
		}
	}
	l->n++;
	return 0;
}

static void test_states(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct ntfs_logfile_info info;

	printf("test_states\n");
	/* Empty file. */
	lb_init(&b, FIRST_PAGE, 2);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	memset(b.buf, 0xff, LSIZE);
	log = open_log(&m, &io);
	CHECK(log && ntfs_logfile_is_clean(log));
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_EMPTY);
	ntfs_logfile_close(log);

	/* Open, flag clear: dirty. */
	lb_init(&b, FIRST_PAGE, 2);
	{
		uint64_t l = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
		lb_restart(&b, l, b.last_len, l, 0, 0, false);
		lb_protect(&b);
	}
	m.buf = b.buf;
	log = open_log(&m, &io);
	CHECK(log && !ntfs_logfile_is_clean(log));
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_DIRTY);
	CHECK_EQ(info.major_ver, 1);
	CHECK_EQ(info.minor_ver, 1);
	CHECK_EQ(info.log_page_size, PS);
	CHECK_EQ(info.seq_number_bits, SEQ_BITS);
	/* Open with VOLUME_IS_CLEAN: clean. */
	lb_restart(&b, b.last_lsn, b.last_len, b.last_lsn, 0, RESTART_VOLUME_IS_CLEAN, false);
	ntfs_logfile_close(log);
	log = open_log(&m, &io);
	CHECK(log && ntfs_logfile_is_clean(log));
	/* Closed: clean. */
	lb_restart(&b, b.last_lsn, b.last_len, b.last_lsn, 0, 0, true);
	ntfs_logfile_close(log);
	log = open_log(&m, &io);
	CHECK(log && ntfs_logfile_is_clean(log));
	ntfs_logfile_close(log);
	/* Unsupported version. */
	lb_restart(&b, b.last_lsn, b.last_len, b.last_lsn, 0, 0, false);
	lf_put16(b.buf + RP_MAJOR_VER, 3);
	lf_put16(b.buf + PS + RP_MAJOR_VER, 3);
	log = open_log(&m, &io);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_UNSUPPORTED);
	CHECK(!ntfs_logfile_is_clean(log));
	ntfs_logfile_close(log);
	/* Both restart pages garbage: corrupt. */
	memset(b.buf, 0x5a, 2 * PS);
	log = open_log(&m, &io);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_CORRUPT);
	CHECK(!ntfs_logfile_is_clean(log));
	ntfs_logfile_close(log);
	/* Second page newer than first: it is used. */
	lb_restart(&b, b.last_lsn, b.last_len, b.last_lsn, 0, 0, false);
	{
		uint8_t *p = b.buf;
		ntfs_log_fixup_post_read(p, PS, 512, NULL);
		lf_put64(p + 0x30 + RA_CURRENT_LSN, b.last_lsn - 8);
		ntfs_log_fixup_pre_write(p, PS, 512);
	}
	log = open_log(&m, &io);
	ntfs_logfile_get_info(log, &info);
	CHECK(info.restart_page_used[1]);
	CHECK_EQ(info.current_lsn, b.last_lsn);
	ntfs_logfile_close(log);
	free(b.buf);
}

static void test_walk_wrap_multipage(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct ntfs_logfile_info info;
	struct rec_list l;
	uint64_t ckpt, restart_lsn, lsn_at_ra = 0, big_lsn = 0, last;
	uint32_t len_at_ra = 0;
	uint8_t big[6000];
	unsigned i, expect = 0;
	uint32_t crc = 0;

	printf("test_walk_wrap_multipage\n");
	for (i = 0; i < sizeof(big); i++) {
		big[i] = (uint8_t)(i * 7 + 3);
		crc = crc * 31 + big[i];
	}
	/* Start near the end so the records wrap to the first record page. */
	lb_init(&b, LSIZE - 6 * PS, 5);
	ckpt = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);		/* start of checkpoint */
	restart_lsn = lb_checkpoint(&b, ckpt, 0, 0, 0, 0, 0, 0);
	/* Filler records of 1500 bytes each; 25 of them cross the wrap. */
	for (i = 0; i < 25; i++) {
		uint8_t pad[1500];
		struct crec c;
		memset(pad, (int)i, sizeof(pad));
		memset(&c, 0, sizeof(c));
		c.redo_op = LOP_Noop;
		c.undo_op = LOP_Noop;
		c.redo = pad;
		c.redo_len = sizeof(pad);
		last = lb_client(&b, 0x18, 0, &c);
		expect++;
		if (i == 10) {
			lsn_at_ra = last;
			len_at_ra = b.last_len;
		}
	}
	CHECK(b.wrapped);
	/* A multi-page record. */
	{
		struct crec c;
		memset(&c, 0, sizeof(c));
		c.redo_op = LOP_Noop;
		c.undo_op = LOP_Noop;
		c.redo = big;
		c.redo_len = sizeof(big);
		big_lsn = lb_client(&b, 0x18, 0, &c);
		expect++;
	}
	last = lb_simple(&b, 0x18, LOP_CommitTransaction, LOP_Noop);
	expect++;
	/* Restart area lags behind: it knows only record 10. */
	lb_restart(&b, lsn_at_ra, len_at_ra, ckpt, restart_lsn, 0, false);
	lb_protect(&b);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	CHECK(log);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_DIRTY);
	CHECK_EQ(info.current_lsn, lsn_at_ra);
	memset(&l, 0, sizeof(l));
	CHECK_EQ(ntfs_logfile_walk(log, 0, collect, &l), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK(info.tail_scanned);
	CHECK_EQ(info.last_lsn, last);
	CHECK_EQ(info.checkpoint_lsn, ckpt);
	/* walk yields the checkpoint record itself first, then everything after */
	CHECK_EQ(l.n, expect + 2);
	if (l.n < 2 || l.n > 256) {
		ntfs_logfile_close(log);
		free(b.buf);
		return;
	}
	CHECK_EQ(l.lsn[0], ckpt);
	CHECK_EQ(l.lsn[1], restart_lsn);
	CHECK_EQ(l.lsn[l.n - 1], last);
	CHECK_EQ(l.lsn[l.n - 2], big_lsn);
	CHECK_EQ(l.len[l.n - 2], NR_HEADER_SIZE + 8 + sizeof(big));
	CHECK_EQ(l.redo[l.n - 1], LOP_CommitTransaction);
	/* multi-page payload intact: recompute crc over the big record only */
	{
		struct rec_list l2;
		memset(&l2, 0, sizeof(l2));
		CHECK_EQ(ntfs_logfile_walk(log, big_lsn, collect, &l2), 0);
		CHECK_EQ(l2.n, 2);
		l2.n = 0;
		/* first record yielded is big_lsn; crc captured when yielded */
		memset(&l2, 0, sizeof(l2));
		ntfs_logfile_walk(log, big_lsn, collect, &l2);
	}
	/* Walk from an explicit lsn. */
	memset(&l, 0, sizeof(l));
	CHECK_EQ(ntfs_logfile_walk(log, big_lsn, collect, &l), 0);
	CHECK_EQ(l.n, 2);
	CHECK_EQ(l.lsn[0], big_lsn);
	/* crc of the last client record with redo data seen in that walk = big */
	CHECK_EQ(l.last_data_crc, crc);
	/* Replay: nothing dirty, one committed-but-forgotten? No: transaction 0x18
	 * was committed, has no dirty pages -> no plan, no error. */
	{
		struct vol v;
		struct ntfs_log_apply ap;
		struct ntfs_log_replay_result res;
		vol_init(&v);
		vol_apply(&v, &ap);
		CHECK_EQ(ntfs_logfile_replay(log, &ap, true, &res), 0);
		CHECK_EQ(res.records_analyzed, expect + 1);	/* + the restart record */
		CHECK_EQ(res.transactions_committed, 1);
		CHECK_EQ(res.plan_len, 0);
		CHECK(!res.needs_chkdsk);
		free(v.disk);
	}
	ntfs_logfile_close(log);
	free(b.buf);
}

/* Build the standard replay scenario. @commit selects committed vs active.
 * *@bits_lsn (optional) receives the lsn of the SetBits record. */
static uint64_t build_bitmap_scenario(struct lb *b, bool commit, bool bogus_attr, uint64_t *bits_lsn)
{
	uint8_t oat[256];
	uint32_t oatn = build_oatbl(oat, 6, ATTR_TYPE_DATA);
	uint64_t start, oa_lsn, restart_lsn, l;
	struct crec c;
	uint8_t br[BR_SIZE];

	lb_init(b, FIRST_PAGE, 2);
	start = lb_simple(b, 0x18, LOP_Noop, LOP_Noop);
	oa_lsn = lb_table_dump(b, LOP_OpenAttributeTableDump, oat, oatn);
	restart_lsn = lb_checkpoint(b, 0, oa_lsn, oatn, 0, 0, 0, 0);

	lf_put32(br + BR_BITMAP_OFF, 100);
	lf_put32(br + BR_BITS, 3);
	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_SetBitsInNonresidentBitMap;
	c.undo_op = LOP_ClearBitsInNonresidentBitMap;
	c.target_attr = bogus_attr ? 0x68 : 0x40;
	c.lcns = 1;
	c.lcn[0] = BITMAP_LCN;
	c.redo = br; c.redo_len = BR_SIZE;
	c.undo = br; c.undo_len = BR_SIZE;
	l = lb_client(b, 0x40, 0, &c);
	if (bits_lsn)
		*bits_lsn = l;
	if (commit) {
		lb_simple(b, 0x40, LOP_CommitTransaction, LOP_Noop);
		l = lb_simple(b, 0x40, LOP_ForgetTransaction, LOP_Noop);
	}
	lb_restart(b, b->last_lsn, b->last_len, start, restart_lsn, 0, false);
	lb_protect(b);
	return l;
}

static void test_redo_bitmap(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;
	struct ntfs_logfile_info info;

	printf("test_redo_bitmap\n");
	build_bitmap_scenario(&b, true, false, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	/* dry run */
	CHECK_EQ(ntfs_logfile_replay(log, &ap, true, &res), 0);
	CHECK(!res.needs_chkdsk);
	CHECK_EQ(res.records_analyzed, 3);	/* SetBits, Commit, Forget */
	CHECK_EQ(res.records_redone, 1);
	CHECK_EQ(res.records_undone, 0);
	CHECK_EQ(res.dirty_pages, 1);
	CHECK_EQ(res.transactions_active, 0);
	CHECK_EQ(res.plan_len, 1);
	if (res.plan_len == 1) {
		CHECK_EQ(res.plan[0].kind, NTFS_PLAN_CLUSTERS);
		CHECK_EQ(res.plan[0].lcn, BITMAP_LCN);
		CHECK_EQ(res.plan[0].count, 1);
		CHECK_EQ(res.plan[0].op, LOP_SetBitsInNonresidentBitMap);
		CHECK(!res.plan[0].undo);
	}
	CHECK_EQ(v.clu_writes, 0);
	CHECK_EQ(v.mft_writes, 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.table_open_attrs, 2);
	CHECK_EQ(info.table_dirty_pages, 1);
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 12], 0);
	/* real run */
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK_EQ(v.clu_writes, 1);
	CHECK_EQ(v.mft_writes, 0);
	CHECK_EQ(v.syncs, 1);
	CHECK_EQ(res.clusters_written, 1);
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 12], 0x70);	/* bits 100..102 */
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 13], 0);
	/* mark clean and reopen */
	CHECK_EQ(ntfs_logfile_mark_clean(log), 0);
	CHECK_EQ(m.writes, 2);
	ntfs_logfile_close(log);
	log = open_log(&m, &io);
	CHECK(ntfs_logfile_is_clean(log));
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_CLEAN);
	CHECK_EQ(info.open_log_count, 8);
	ntfs_logfile_close(log);
	free(v.disk);
	free(b.buf);
}

static void test_undo_bitmap(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;

	printf("test_undo_bitmap\n");
	build_bitmap_scenario(&b, false, false, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	/* the page was flushed before the crash: bits already set */
	v.disk[BITMAP_LCN * VCS + 12] = 0x70;
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK(!res.needs_chkdsk);
	CHECK_EQ(res.records_redone, 1);
	CHECK_EQ(res.records_undone, 1);
	CHECK_EQ(res.transactions_active, 1);
	CHECK_EQ(res.plan_len, 2);
	if (res.plan_len == 2) {
		CHECK(!res.plan[0].undo);
		CHECK(res.plan[1].undo);
		CHECK_EQ(res.plan[1].op, LOP_ClearBitsInNonresidentBitMap);
	}
	CHECK_EQ(v.clu_writes, 1);
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 12], 0);
	ntfs_logfile_close(log);
	free(v.disk);
	free(b.buf);
}

static void test_refuse_incoherent(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;

	printf("test_refuse_incoherent\n");
	/* Target attribute not in the open attribute table. */
	build_bitmap_scenario(&b, true, true, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), -EINVAL);
	CHECK(res.needs_chkdsk);
	CHECK_EQ(v.clu_writes, 0);
	CHECK_EQ(v.mft_writes, 0);
	CHECK_EQ(v.syncs, 0);
	ntfs_logfile_close(log);
	free(b.buf);

	/*
	 * Bitmap record whose logged lcn is unreadable (beyond the volume).
	 * The page LCN in the log record is what replay reads and writes
	 * (dirty page table semantics), so that is what must be corrupted;
	 * the read fails and nothing is written.
	 */
	{
		uint64_t bits_lsn;
		uint32_t vbo;
		uint8_t *p;
		build_bitmap_scenario(&b, true, false, &bits_lsn);
		vbo = (uint32_t)(bits_lsn << 3) & (LSIZE - 1);
		p = b.buf + (vbo & ~(PS - 1));
		ntfs_log_fixup_post_read(p, PS, 512, NULL);
		lf_put64(b.buf + vbo + LR_HEADER_SIZE + NR_PAGE_LCNS, VOL_CLUSTERS + 5);
		ntfs_log_fixup_pre_write(p, PS, 512);
	}
	m.buf = b.buf;
	log = open_log(&m, &io);
	CHECK(ntfs_logfile_replay(log, &ap, false, &res) < 0);
	CHECK_EQ(v.clu_writes, 0);
	CHECK_EQ(v.mft_writes, 0);
	ntfs_logfile_close(log);
	free(b.buf);

	/*
	 * Stale run list: record 6's $DATA says the bitmap lives at lcn 41,
	 * the log says the dirty page is at lcn 40. The LCN logged by NTFS at
	 * the time of the operation wins (that is why the LFS records LCNs at
	 * all: the owning record may not have been flushed yet), so replay
	 * writes cluster 40 and leaves 41 alone. Refusing here would break the
	 * file-extension case the module exists for.
	 */
	build_bitmap_scenario(&b, true, false, NULL);
	{
		uint8_t rec[VRS];
		uint32_t off;
		memcpy(rec, v.disk + MFT_LCN * VCS + 6 * VRS, VRS);
		ntfs_log_fixup_post_read(rec, VRS, 512, NULL);
		off = lf_get16(rec + MR_ATTRS_OFFSET) + AT_NONRESIDENT_SIZE;
		rec[off + 2] = (uint8_t)(BITMAP_LCN + 1);
		vol_put_record(&v, 6, rec);
	}
	m.buf = b.buf;
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK(!res.needs_chkdsk);
	CHECK_EQ(v.clu_writes, 1);
	CHECK_EQ(res.plan_len, 1);
	if (res.plan_len == 1)
		CHECK_EQ(res.plan[0].lcn, BITMAP_LCN);
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 12], 0x70);
	CHECK_EQ(v.disk[(BITMAP_LCN + 1) * VCS + 12], 0);
	ntfs_logfile_close(log);
	free(v.disk);
	free(b.buf);

	/*
	 * MFT op whose target vcn lies outside $MFT's run list: $MFT never
	 * shrinks, so the log and record 0 disagree; refused, not skipped.
	 */
	{
		uint8_t oat[256], newrec[VRS];
		uint32_t oatn = build_oatbl(oat, 6, ATTR_TYPE_DATA);
		uint64_t start, oa_lsn, restart_lsn;
		struct crec c;
		lb_init(&b, FIRST_PAGE, 2);
		start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
		oa_lsn = lb_table_dump(&b, LOP_OpenAttributeTableDump, oat, oatn);
		restart_lsn = lb_checkpoint(&b, 0, oa_lsn, oatn, 0, 0, 0, 0);
		build_record(newrec, 4 * MFT_CLUSTERS, MFT_RECORD_IN_USE, fill_plain);
		memset(&c, 0, sizeof(c));
		c.redo_op = LOP_InitializeFileRecordSegment;
		c.undo_op = LOP_Noop;
		c.target_attr = 0x18;
		c.lcns = 1;
		c.lcn[0] = MFT_LCN + MFT_CLUSTERS;	/* one past the end of $MFT */
		c.target_vcn = MFT_CLUSTERS;
		c.redo = newrec;
		c.redo_len = (uint16_t)lf_get32(newrec + MR_BYTES_IN_USE);
		lb_client(&b, 0x40, 0, &c);
		lb_simple(&b, 0x40, LOP_CommitTransaction, LOP_Noop);
		lb_simple(&b, 0x40, LOP_ForgetTransaction, LOP_Noop);
		lb_restart(&b, b.last_lsn, b.last_len, start, restart_lsn, 0, false);
		lb_protect(&b);
	}
	m.buf = b.buf;
	log = open_log(&m, &io);
	vol_init(&v);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), -EINVAL);
	CHECK(res.needs_chkdsk);
	CHECK_EQ(v.mft_writes + v.clu_writes, 0);
	ntfs_logfile_close(log);
	free(v.disk);
	free(b.buf);
}

static void test_mft_record_ops(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;
	uint8_t oat[256], newrec[VRS], got[VRS];
	uint32_t oatn = build_oatbl(oat, 6, ATTR_TYPE_DATA);
	uint64_t start, oa_lsn, restart_lsn, init_lsn;
	struct crec c;
	static const uint8_t newsi[0x30] = { 9, 9, 9, 9 };

	printf("test_mft_record_ops\n");
	lb_init(&b, FIRST_PAGE, 2);
	start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	oa_lsn = lb_table_dump(&b, LOP_OpenAttributeTableDump, oat, oatn);
	restart_lsn = lb_checkpoint(&b, 0, oa_lsn, oatn, 0, 0, 0, 0);

	/* InitializeFileRecordSegment of record 40 (page: vcn 10 -> lcn 26). */
	build_record(newrec, 40, MFT_RECORD_IN_USE, fill_plain);
	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_InitializeFileRecordSegment;
	c.undo_op = LOP_Noop;
	c.target_attr = 0x18;
	c.lcns = 1;
	c.lcn[0] = MFT_LCN + 10;
	c.target_vcn = 10;
	c.cluster_off = 0;		/* record 40 = byte 40960 = vcn 10 + 0 */
	c.redo = newrec;
	c.redo_len = (uint16_t)lf_get32(newrec + MR_BYTES_IN_USE);
	init_lsn = lb_client(&b, 0x40, 0, &c);
	/* UpdateResidentValue on record 30: $STANDARD_INFORMATION value. */
	memset(&c, 0, sizeof(c));
	c.redo_op = LOP_UpdateResidentValue;
	c.undo_op = LOP_UpdateResidentValue;
	c.target_attr = 0x18;
	c.lcns = 1;
	c.lcn[0] = MFT_LCN + 7;			/* record 30 = byte 30720 = vcn 7 + 2048 */
	c.target_vcn = 7;
	c.cluster_off = 2048 / 512;
	c.record_off = MR_FIXUP_OFFSET_3 + 8;	/* first attribute */
	c.attr_off = AT_RESIDENT_SIZE;		/* value */
	c.redo = newsi; c.redo_len = sizeof(newsi);
	c.undo = newsi; c.undo_len = sizeof(newsi);
	lb_client(&b, 0x40, 0, &c);
	lb_simple(&b, 0x40, LOP_CommitTransaction, LOP_Noop);
	lb_simple(&b, 0x40, LOP_ForgetTransaction, LOP_Noop);
	lb_restart(&b, b.last_lsn, b.last_len, start, restart_lsn, 0, false);
	lb_protect(&b);

	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, true, &res), 0);
	CHECK(!res.needs_chkdsk);
	CHECK_EQ(res.records_redone, 2);
	CHECK_EQ(res.plan_len, 2);
	if (res.plan_len == 2) {
		CHECK_EQ(res.plan[0].kind, NTFS_PLAN_MFT_RECORD);
		CHECK_EQ(res.plan[0].mft_no, 40);
		CHECK_EQ(res.plan[1].kind, NTFS_PLAN_MFT_RECORD);
		CHECK_EQ(res.plan[1].mft_no, 30);
	}
	CHECK_EQ(v.mft_writes, 0);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK_EQ(v.mft_writes, 2);
	CHECK_EQ(v.clu_writes, 0);
	/* record 40 now exists, protected, with the record lsn stamped */
	memcpy(got, v.disk + MFT_LCN * VCS + 40 * VRS, VRS);
	CHECK_EQ(lf_get32(got), LFS_MAGIC_FILE);
	CHECK_EQ(ntfs_log_fixup_post_read(got, VRS, 512, NULL), 0);
	CHECK_EQ(lf_get64(got + MR_LSN), init_lsn);
	CHECK_EQ(lf_get32(got + MR_RECORD_NUMBER), 40);
	CHECK(!memcmp(got + MR_ATTRS_OFFSET, newrec + MR_ATTRS_OFFSET, 2));
	/* record 30's value updated */
	memcpy(got, v.disk + MFT_LCN * VCS + 30 * VRS, VRS);
	CHECK_EQ(ntfs_log_fixup_post_read(got, VRS, 512, NULL), 0);
	CHECK(!memcmp(got + MR_FIXUP_OFFSET_3 + 8 + AT_RESIDENT_SIZE, newsi, sizeof(newsi)));
	/* Replaying again is idempotent: records carry lsn >= record lsn. */
	CHECK_EQ(ntfs_logfile_replay(log, &ap, true, &res), 0);
	CHECK_EQ(res.records_redone, 0);
	CHECK_EQ(res.plan_len, 2);
	if (res.plan_len == 2)
		CHECK_EQ(res.plan[0].kind, NTFS_PLAN_SKIPPED);
	ntfs_logfile_close(log);

	/* Wrong logged lcn for the MFT page: refused, nothing written. */
	{
		uint8_t *p = b.buf + FIRST_PAGE;
		uint32_t vbo = (uint32_t)(init_lsn << 3) & (LSIZE - 1);
		ntfs_log_fixup_post_read(p, PS, 512, NULL);
		lf_put64(b.buf + vbo + LR_HEADER_SIZE + NR_PAGE_LCNS, MFT_LCN + 11);
		ntfs_log_fixup_pre_write(p, PS, 512);
		vol_init(&v);	/* leaks previous disk? free first */
	}
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), -EINVAL);
	CHECK(res.needs_chkdsk);
	CHECK_EQ(v.mft_writes, 0);
	ntfs_logfile_close(log);
	free(v.disk);
	free(b.buf);
}

static void test_tail_copy_and_torn(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct ntfs_logfile_info info;
	struct rec_list l;
	uint64_t start, restart_lsn, last, tail_lsn;
	uint32_t last_page;
	unsigned i;

	printf("test_tail_copy_and_torn\n");
	lb_init(&b, FIRST_PAGE, 2);
	start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	restart_lsn = lb_checkpoint(&b, start, 0, 0, 0, 0, 0, 0);
	for (i = 0; i < 3; i++)
		last = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	last_page = b.page;
	/* Tail copy at page 2 holds one more record than the in-place page. */
	tail_lsn = lb_make_tail_copy(&b, last_page, b.seq);
	lb_restart(&b, start, 0, start, restart_lsn, 0, false);
	lb_protect(&b);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	memset(&l, 0, sizeof(l));
	CHECK_EQ(ntfs_logfile_walk(log, 0, collect, &l), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.tail_overrides, 1);
	CHECK_EQ(info.last_lsn, tail_lsn);
	CHECK_EQ(l.n, 6);
	CHECK_EQ(l.lsn[l.n - 1], tail_lsn);
	CHECK_EQ(l.lsn[l.n - 2], last);
	/* mark_clean writes the superseding copy back, then both restart pages */
	CHECK_EQ(ntfs_logfile_mark_clean(log), 0);
	CHECK_EQ(m.writes, 3);
	ntfs_logfile_close(log);
	log = open_log(&m, &io);
	CHECK(ntfs_logfile_is_clean(log));
	ntfs_logfile_close(log);
	{
		uint8_t pg[PS];
		memcpy(pg, b.buf + last_page, PS);
		CHECK_EQ(ntfs_log_fixup_post_read(pg, PS, 512, NULL), 0);
		CHECK_EQ(lf_get64(pg + PG_LAST_END_LSN), tail_lsn);
		CHECK_EQ(lf_get64(pg + PG_LAST_LSN), tail_lsn);
	}
	free(b.buf);

	/* Torn last page without tail copy: log ends at the previous page. */
	lb_init(&b, FIRST_PAGE, 2);
	start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	restart_lsn = lb_checkpoint(&b, start, 0, 0, 0, 0, 0, 0);
	for (i = 0; i < 3; i++)
		last = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	lb_next_page(&b);
	lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	last_page = b.page;
	lb_restart(&b, start, 0, start, restart_lsn, 0, false);
	lb_protect(&b);
	b.buf[last_page + 512 - 2] ^= 0xff;	/* sector 0 tail mismatch */
	m.buf = b.buf;
	log = open_log(&m, &io);
	memset(&l, 0, sizeof(l));
	CHECK_EQ(ntfs_logfile_walk(log, 0, collect, &l), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.last_lsn, last);
	CHECK_EQ(l.n, 5);
	ntfs_logfile_close(log);
	/* ... but a valid page from the expected sequence after the torn one is corruption. */
	{
		struct lb b2;
		lb_init(&b2, FIRST_PAGE, 2);
		lb_simple(&b2, 0x18, LOP_Noop, LOP_Noop);
		lb_simple(&b2, 0x18, LOP_Noop, LOP_Noop);
		lb_next_page(&b2);
		lb_simple(&b2, 0x18, LOP_Noop, LOP_Noop);
		lb_next_page(&b2);
		lb_simple(&b2, 0x18, LOP_Noop, LOP_Noop);
		lb_protect(&b2);
		/* page after the torn one, with the same (expected) sequence */
		memcpy(b.buf + last_page + PS, b2.buf + FIRST_PAGE + 2 * PS, PS);
		free(b2.buf);
	}
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_logfile_walk(log, 0, collect, &l), -EINVAL);
	{
		struct vol v;
		struct ntfs_log_apply ap;
		struct ntfs_log_replay_result res;
		vol_init(&v);
		vol_apply(&v, &ap);
		CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), -EINVAL);
		CHECK(res.needs_chkdsk);
		CHECK_EQ(v.mft_writes + v.clu_writes, 0);
		free(v.disk);
	}
	ntfs_logfile_close(log);
	free(b.buf);
}

static int oa_cb(const struct ntfs_log_open_attr *a, void *ctx)
{
	int *n = ctx;
	(*n)++;
	if (a->id == 0x18) {
		CHECK_EQ(a->mft_no, 0);
		CHECK_EQ(a->type, ATTR_TYPE_DATA);
	} else {
		CHECK_EQ(a->id, 0x40);
		CHECK_EQ(a->mft_no, 6);
	}
	return 0;
}

static void test_tables_api(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	int n_oa = 0;
	struct ntfs_log_table_cbs cbs;

	printf("test_tables_api\n");
	build_bitmap_scenario(&b, true, false, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	CHECK_EQ(ntfs_logfile_load_checkpoint(log), 0);
	memset(&cbs, 0, sizeof(cbs));
	cbs.open_attr = oa_cb;
	CHECK_EQ(ntfs_logfile_tables(log, &cbs, &n_oa), 0);
	CHECK_EQ(n_oa, 2);
	ntfs_logfile_close(log);
	free(b.buf);
}

/*
 * Version 2.0 log (Windows 8+ layout as inferred from fslog.c): restart
 * pages at 0/1, 32 tail-copy slots at 0x02..0x21, record pages from 0x22,
 * file_off at 0x3c of the page header. Same scenario as test_redo_bitmap
 * plus a v2 tail copy that supersedes its in-place page. No real 2.0 log
 * has been seen by this code: this pins the inferred layout, nothing more.
 */
static void test_v2_log(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct ntfs_logfile_info info;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;
	struct rec_list l;
	uint64_t start, restart_lsn, last = 0, tail_lsn;
	uint32_t last_page;
	unsigned i;

	printf("test_v2_log\n");
	lb_set_version(2, 0);
	CHECK_EQ(FIRST_PAGE, 0x22 * PS);

	/* Replay of the standard scenario. */
	build_bitmap_scenario(&b, true, false, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	CHECK(log);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.state, NTFS_LOG_DIRTY);
	CHECK_EQ(info.major_ver, 2);
	CHECK_EQ(info.minor_ver, 0);
	CHECK(!ntfs_logfile_is_clean(log));
	vol_init(&v);
	vol_apply(&v, &ap);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, true, &res), 0);
	CHECK_EQ(res.records_redone, 1);
	CHECK_EQ(res.plan_len, 1);
	CHECK_EQ(v.clu_writes, 0);
	CHECK_EQ(ntfs_logfile_replay(log, &ap, false, &res), 0);
	CHECK_EQ(v.clu_writes, 1);
	CHECK_EQ(v.disk[BITMAP_LCN * VCS + 12], 0x70);
	CHECK_EQ(ntfs_logfile_mark_clean(log), 0);
	ntfs_logfile_close(log);
	log = open_log(&m, &io);
	CHECK(log && ntfs_logfile_is_clean(log));
	ntfs_logfile_close(log);
	free(v.disk);
	free(b.buf);

	/* Tail copy at slot 0x02 with file_off naming the last record page. */
	lb_init(&b, FIRST_PAGE, 2);
	start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	restart_lsn = lb_checkpoint(&b, start, 0, 0, 0, 0, 0, 0);
	for (i = 0; i < 3; i++)
		last = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	last_page = b.page;
	tail_lsn = lb_make_tail_copy(&b, last_page, b.seq);
	lb_restart(&b, start, 0, start, restart_lsn, 0, false);
	lb_protect(&b);
	m.buf = b.buf; m.writes = 0;
	log = open_log(&m, &io);
	memset(&l, 0, sizeof(l));
	CHECK_EQ(ntfs_logfile_walk(log, 0, collect, &l), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.tail_overrides, 1);
	CHECK_EQ(info.last_lsn, tail_lsn);
	CHECK_EQ(l.n, 6);
	CHECK_EQ(l.lsn[l.n - 1], tail_lsn);
	CHECK_EQ(l.lsn[l.n - 2], last);
	CHECK_EQ(ntfs_logfile_mark_clean(log), 0);
	CHECK_EQ(m.writes, 3);
	ntfs_logfile_close(log);
	{
		uint8_t pg[PS];
		memcpy(pg, b.buf + last_page, PS);
		CHECK_EQ(ntfs_log_fixup_post_read(pg, PS, 512, NULL), 0);
		CHECK_EQ(lf_get64(pg + PG_LAST_END_LSN), tail_lsn);
		CHECK_EQ(lf_get32(pg + PG_FILE_OFFSET), 0);	/* in-place page again */
	}
	log = open_log(&m, &io);
	CHECK(log && ntfs_logfile_is_clean(log));
	ntfs_logfile_close(log);
	free(b.buf);

	/* A stray tail copy sitting in the record area is not a log page. */
	lb_init(&b, FIRST_PAGE, 2);
	start = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	restart_lsn = lb_checkpoint(&b, start, 0, 0, 0, 0, 0, 0);
	last = lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	last_page = b.page;
	lb_next_page(&b);
	lb_simple(&b, 0x18, LOP_Noop, LOP_Noop);
	lf_put32(b.buf + b.page + PG_FILE_OFFSET, last_page);	/* claims to be a copy of the previous page */
	lb_restart(&b, start, 0, start, restart_lsn, 0, false);
	lb_protect(&b);
	m.buf = b.buf;
	log = open_log(&m, &io);
	memset(&l, 0, sizeof(l));
	CHECK_EQ(ntfs_logfile_walk(log, 0, collect, &l), 0);
	ntfs_logfile_get_info(log, &info);
	CHECK_EQ(info.last_lsn, last);
	CHECK_EQ(l.n, 3);
	ntfs_logfile_close(log);
	free(b.buf);

	lb_set_version(1, 1);
}

/* Fixture images from the tools stream: freshly formatted, must be clean. */
static void test_images(void)
{
	const char *dir = getenv("NTFS_IMAGES");
	DIR *d;
	struct dirent *de;
	int seen = 0;

	if (!dir || !(d = opendir(dir))) {
		printf("test_images: NTFS_IMAGES not set or missing, skipped\n");
		return;
	}
	while ((de = readdir(d))) {
		char path[1024];
		struct ntfs_image img;
		struct ntfs_log_io io;
		struct ntfs_log_geometry g;
		ntfs_logfile_t *log = NULL;
		struct ntfs_logfile_info info;
		size_t n = strlen(de->d_name);
		if (n < 4 || strcmp(de->d_name + n - 4, ".img"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (ntfs_image_open(&img, path, false, 0, 0)) {
			printf("test_images: %s: %s (skipped)\n", de->d_name, img.err);
			ntfs_image_close(&img);
			continue;
		}
		seen++;
		ntfs_image_log_io(&img, &io);
		ntfs_image_geometry(&img, &g);
		CHECK_EQ(ntfs_logfile_open(&io, &g, &log), 0);
		if (log) {
			ntfs_logfile_get_info(log, &info);
			printf("test_images: %s: state %d clean=%d\n", de->d_name, info.state,
			       ntfs_logfile_is_clean(log));
			CHECK(ntfs_logfile_is_clean(log));
			ntfs_logfile_close(log);
		}
		ntfs_image_close(&img);
	}
	closedir(d);
	printf("test_images: %d image(s)\n", seen);
}

/*
 * A device that accepts every write and then refuses the barrier.
 *
 * Not hypothetical: macOS FSKit's metadataFlush fails on every USB device on
 * the development machine (20 failures in 45 minutes on one stick), and a real
 * replay on a real Windows 10 volume reported itself as "Replay failed part-way.
 * The volume may be inconsistent" when every write had in fact landed and chkdsk
 * afterwards found no problems. The distinction matters: a failed write can
 * leave a volume half-applied, a failed barrier only means the device would not
 * confirm what it has already taken.
 */
static void test_flush_failure_is_not_a_partial_write(void)
{
	struct lb b;
	struct memio m;
	struct ntfs_log_io io;
	ntfs_logfile_t *log;
	struct vol v;
	struct ntfs_log_apply ap;
	struct ntfs_log_replay_result res;

	printf("test_flush_failure_is_not_a_partial_write\n");

	/* barrier refused, writes fine */
	build_bitmap_scenario(&b, true, false, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	v.sync_errno = -EIO;
	memset(&res, 0, sizeof(res));
	CHECK(ntfs_logfile_replay(log, &ap, false, &res) != 0);
	CHECK(res.flush_failed);
	CHECK_EQ(v.syncs, 1);
	CHECK_EQ(v.clu_writes, 1);	/* the write still happened */
	CHECK_EQ(res.clusters_written, 1);
	ntfs_logfile_close(log);

	/* a genuine write failure must NOT be reported as a flush failure */
	build_bitmap_scenario(&b, true, false, NULL);
	m.buf = b.buf; m.size = LSIZE; m.writes = 0;
	log = open_log(&m, &io);
	vol_init(&v);
	vol_apply(&v, &ap);
	v.write_errno = -EIO;
	memset(&res, 0, sizeof(res));
	CHECK(ntfs_logfile_replay(log, &ap, false, &res) != 0);
	CHECK(!res.flush_failed);
	ntfs_logfile_close(log);
}

int main(void)
{
	test_states();
	test_walk_wrap_multipage();
	test_redo_bitmap();
	test_undo_bitmap();
	test_refuse_incoherent();
	test_mft_record_ops();
	test_tail_copy_and_torn();
	test_tables_api();
	test_v2_log();
	test_flush_failure_is_not_a_partial_write();
	test_images();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
