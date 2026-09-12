// SPDX-License-Identifier: GPL-2.0
/*
 * $LogFile replay: ARIES-style analysis, redo and undo passes.
 *
 * Structure follows Paragon's fs/ntfs3/fslog.c log_replay()/do_action(),
 * with two deliberate differences:
 *
 *  1. Nothing is written while passes run. Every modification lands in an
 *     in-memory overlay (MFT records by number, clusters by LCN). Only when
 *     all three passes complete without an incoherence is the overlay
 *     flushed, in order, through the apply vtable. A dry run stops before
 *     the flush and hands back the plan.
 *  2. Any record that cannot be applied safely aborts the replay
 *     (needs_chkdsk). fslog.c skips such records and marks the volume dirty;
 *     we prefer to leave the volume untouched for chkdsk.
 */
#include "logfile_internal.h"

#define SPARSE_LCN	((uint64_t)-1)
#define MFT_REC_MIRR	1
#define MFT_REC_VOL	3

/* ---- Overlay ----------------------------------------------------------- */

struct ovl_mft {
	uint64_t mft_no;
	uint8_t *rec;		/* deprotected record */
	bool dirty, bad;
};

struct ovl_clu {
	uint64_t lcn;
	uint8_t *data;
	bool dirty, used;
};

struct run {
	uint64_t vcn, lcn, len;	/* lcn == SPARSE_LCN for holes */
};

struct open_attr {
	bool loaded, failed;
	bool in_extent;		/* some runs came from an $ATTRIBUTE_LIST extent */
	uint64_t mft_no;
	uint32_t type;
	uint16_t name_len;	/* utf16 units */
	uint8_t name[2 * 255];
	uint32_t bytes_per_index;
	bool resident;
	uint64_t alloc_size, data_size;
	struct run *runs;
	uint32_t nruns, cap_runs;
	struct run *ovr;	/* per-vcn overrides from the dirty page table */
	uint32_t novr, cap_ovr;
};

struct replay {
	ntfs_logfile_t *log;
	const struct ntfs_log_apply *ap;
	bool dry_run;
	struct ntfs_log_replay_result *res;
	uint32_t cs, rs;	/* cluster size, mft record size */

	struct ovl_mft *mft;
	uint32_t nmft, cap_mft;
	struct ovl_clu *clu;	/* open-addressing hash */
	uint32_t clu_cap, clu_used;

	struct open_attr *oa;	/* indexed by (offset - RT_HEADER_SIZE) / esize */
	uint32_t noa;

	struct open_attr mftdata;	/* $MFT:$DATA runs from record 0, for aliasing checks */
	bool mft_loaded;
};

static int ovl_clu_grow(struct replay *r);

static struct ovl_clu *ovl_clu_find(struct replay *r, uint64_t lcn, bool create)
{
	uint32_t i, mask;

	if (!r->clu_cap) {
		if (!create || ovl_clu_grow(r))
			return NULL;
	}
	if (create && r->clu_used * 2 >= r->clu_cap && ovl_clu_grow(r))
		return NULL;
	mask = r->clu_cap - 1;
	i = (uint32_t)(lcn * 0x9E3779B97F4A7C15ull >> 32) & mask;
	for (;;) {
		struct ovl_clu *c = &r->clu[i];
		if (!c->used) {
			if (!create)
				return NULL;
			c->used = true;
			c->lcn = lcn;
			c->data = NULL;
			c->dirty = false;
			r->clu_used++;
			return c;
		}
		if (c->lcn == lcn)
			return c;
		i = (i + 1) & mask;
	}
}

static int ovl_clu_grow(struct replay *r)
{
	uint32_t ncap = r->clu_cap ? r->clu_cap * 2 : 1024;
	struct ovl_clu *old = r->clu;
	uint32_t ocap = r->clu_cap, i;

	r->clu = calloc(ncap, sizeof(*r->clu));
	if (!r->clu) {
		r->clu = old;
		return -ENOMEM;
	}
	r->clu_cap = ncap;
	r->clu_used = 0;
	for (i = 0; i < ocap; i++) {
		if (old[i].used) {
			struct ovl_clu *c = ovl_clu_find(r, old[i].lcn, true);
			c->data = old[i].data;
			c->dirty = old[i].dirty;
		}
	}
	free(old);
	return 0;
}

/* Cluster with data loaded (from overlay or device). */
static int ovl_get_cluster(struct replay *r, uint64_t lcn, struct ovl_clu **out)
{
	struct ovl_clu *c = ovl_clu_find(r, lcn, true);
	int err;

	if (!c)
		return -ENOMEM;
	if (!c->data) {
		c->data = malloc(r->cs);
		if (!c->data)
			return -ENOMEM;
		err = r->ap->read_clusters(r->ap->ctx, lcn, 1, c->data);
		if (err) {
			free(c->data);
			c->data = NULL;
			return lfs_seterr(r->log, err < 0 ? err : -EIO, "read cluster %llu failed",
					  (unsigned long long)lcn);
		}
	}
	*out = c;
	return 0;
}

static struct ovl_mft *ovl_mft_lookup(struct replay *r, uint64_t mft_no)
{
	uint32_t i;

	for (i = 0; i < r->nmft; i++)
		if (r->mft[i].mft_no == mft_no)
			return &r->mft[i];
	return NULL;
}

/*
 * MFT record from overlay or device, deprotected. If @init and the record
 * cannot be read or is not a FILE record, a zeroed record is returned
 * (InitializeFileRecordSegment overwrites it entirely).
 */
static int ovl_get_mft(struct replay *r, uint64_t mft_no, bool init, struct ovl_mft **out)
{
	struct ovl_mft *m = ovl_mft_lookup(r, mft_no);
	int err;

	if (m) {
		*out = m;
		return 0;
	}
	if (r->nmft == r->cap_mft) {
		uint32_t ncap = r->cap_mft ? r->cap_mft * 2 : 64;
		struct ovl_mft *n = realloc(r->mft, ncap * sizeof(*n));
		if (!n)
			return -ENOMEM;
		r->mft = n;
		r->cap_mft = ncap;
	}
	m = &r->mft[r->nmft];
	memset(m, 0, sizeof(*m));
	m->mft_no = mft_no;
	m->rec = malloc(r->rs);
	if (!m->rec)
		return -ENOMEM;
	err = r->ap->read_mft_record(r->ap->ctx, mft_no, m->rec);
	if (!err && lf_get32(m->rec) == LFS_MAGIC_FILE) {
		bool torn = false;
		err = lfs_fixup_post_read(m->rec, r->rs, LFS_SECTOR_SIZE, &torn);
		if (err || torn) {
			/* Unusable like a BAAD record; every op but Initialize refuses it. */
			lfs_msg(r->log, 1, "mft record %llu: %s update sequence array",
				(unsigned long long)mft_no, err ? "malformed" : "torn");
			m->bad = true;
			err = 0;
		}
	} else if (!err && lf_get32(m->rec) == LFS_MAGIC_BAAD) {
		m->bad = true;
	} else if (err || init) {
		if (!init) {
			free(m->rec);
			return lfs_seterr(r->log, -EIO, "read mft record %llu failed",
					  (unsigned long long)mft_no);
		}
		memset(m->rec, 0, r->rs);
		err = 0;
	}
	if (err) {
		free(m->rec);
		return err;
	}
	r->nmft++;
	*out = m;
	return 0;
}

int lfs_plan_add(ntfs_logfile_t *log, const struct ntfs_log_plan_entry *e)
{
	if (log->plan_len == log->plan_cap) {
		uint32_t ncap = log->plan_cap ? log->plan_cap * 2 : 256;
		struct ntfs_log_plan_entry *n = realloc(log->plan, ncap * sizeof(*n));
		if (!n)
			return -ENOMEM;
		log->plan = n;
		log->plan_cap = ncap;
	}
	log->plan[log->plan_len++] = *e;
	return 0;
}

static int plan_skip(struct replay *r, uint64_t lsn, uint16_t op, bool undo, const char *why)
{
	struct ntfs_log_plan_entry e;

	memset(&e, 0, sizeof(e));
	e.kind = NTFS_PLAN_SKIPPED;
	e.lsn = lsn;
	e.op = op;
	e.undo = undo;
	snprintf(e.why, sizeof(e.why), "%s", why);
	return lfs_plan_add(r->log, &e);
}

/* ---- NTFS structure helpers ------------------------------------------- */

static bool mft_ref_no(const uint8_t *ref, uint64_t *no, uint16_t *seq)
{
	*no = lf_get32(ref) | ((uint64_t)lf_get16(ref + 4) << 32);
	if (seq)
		*seq = lf_get16(ref + 6);
	return true;
}

/* Validate an MFT record enough to walk its attributes safely. */
static bool check_file_record(struct replay *r, const uint8_t *rec)
{
	uint16_t fo = lf_get16(rec + MR_USA_OFS), fn = lf_get16(rec + MR_USA_COUNT);
	uint16_t ao = lf_get16(rec + MR_ATTRS_OFFSET);
	uint32_t used = lf_get32(rec + MR_BYTES_IN_USE);
	uint32_t rs = r->rs, off;

	if (lf_get32(rec) != LFS_MAGIC_FILE)
		return false;
	if (fo > LFS_SECTOR_SIZE - ((rs >> LFS_SECTOR_SHIFT) + 1) * 2 ||
	    (uint32_t)(fn - 1) * LFS_SECTOR_SIZE != rs)
		return false;
	if (ao < MR_FIXUP_OFFSET_1 || ao > rs - AT_RESIDENT_SIZE || (ao & 7))
		return false;
	if (!(lf_get16(rec + MR_FLAGS) & MFT_RECORD_IN_USE))
		return false;
	if (lf_get32(rec + MR_BYTES_ALLOCATED) != rs || used > rs || used < ao)
		return false;
	for (off = ao;;) {
		uint32_t type, len;
		if (off + 4 > rs)
			return false;
		type = lf_get32(rec + off + AT_TYPE);
		if (type == ATTR_TYPE_END)
			break;
		if (off + AT_RESIDENT_SIZE > rs)
			return false;
		len = lf_get32(rec + off + AT_LENGTH);
		if (len < AT_RESIDENT_SIZE || (len & 7) || off + len > rs)
			return false;
		if (rec[off + AT_NAME_LENGTH] &&
		    lf_get16(rec + off + AT_NAME_OFFSET) + rec[off + AT_NAME_LENGTH] * 2u > len)
			return false;
		if (rec[off + AT_NON_RESIDENT] == 0) {
			uint32_t vl = lf_get32(rec + off + AT_VALUE_LENGTH);
			uint16_t vo = lf_get16(rec + off + AT_VALUE_OFFSET);
			if (vl >= len || vo + vl > len)
				return false;
		} else if (rec[off + AT_NON_RESIDENT] == 1) {
			if (len < AT_NONRESIDENT_SIZE)
				return false;
			if (lf_get16(rec + off + AT_MAPPING_PAIRS_OFFSET) > len)
				return false;
			if (lf_get64(rec + off + AT_LOWEST_VCN) > lf_get64(rec + off + AT_HIGHEST_VCN) + 1)
				return false;
		} else {
			return false;
		}
		off += len;
	}
	return used >= off + 4;
}

/* Does record_off point exactly at an attribute boundary in @rec? */
static bool check_if_attr(const uint8_t *rec, uint32_t rs, uint16_t ro)
{
	uint32_t o = lf_get16(rec + MR_ATTRS_OFFSET);

	while (o < ro) {
		uint32_t asize;
		if (o + 8 > rs)
			return false;
		if (lf_get32(rec + o + AT_TYPE) == ATTR_TYPE_END)
			break;
		asize = lf_get32(rec + o + AT_LENGTH);
		if (!asize)
			break;
		o += asize;
	}
	return o == ro;
}

static bool check_if_index_root(const uint8_t *rec, uint32_t rs, uint16_t ro)
{
	return check_if_attr(rec, rs, ro) && ro + 8 <= rs &&
	       lf_get32(rec + ro + AT_TYPE) == ATTR_TYPE_INDEX_ROOT &&
	       rec[ro + AT_NON_RESIDENT] == 0;
}

/* Index header checks (ntfs3 check_index_header). */
static bool check_index_header(const uint8_t *hdr, uint32_t bytes)
{
	uint32_t de_off = lf_get32(hdr + IH_ENTRIES_OFFSET);
	uint32_t used = lf_get32(hdr + IH_INDEX_LENGTH);
	uint32_t total = lf_get32(hdr + IH_ALLOCATED_SIZE);
	bool sub = lf_get32(hdr + IH_FLAGS) & INDEX_HDR_HAS_SUBNODES;
	uint32_t min_de = IE_MIN_SIZE + (sub ? 8 : 0);
	uint32_t o;

	if (bytes < IH_SIZE || de_off > bytes - min_de || used > bytes || total > bytes ||
	    de_off + min_de > used || used > total)
		return false;
	for (o = de_off;;) {
		uint16_t esize;
		if (o + IE_MIN_SIZE > used)
			return false;
		esize = lf_get16(hdr + o + IE_LENGTH);
		if (esize < min_de || o + esize > used)
			return false;
		if (((lf_get16(hdr + o + IE_FLAGS) & INDEX_ENTRY_NODE) != 0) != sub)
			return false;
		if (lf_get16(hdr + o + IE_FLAGS) & INDEX_ENTRY_END)
			break;
		o += esize;
	}
	return true;
}

static bool check_index_buffer(const uint8_t *ib, uint32_t bytes)
{
	uint16_t fo, fn;

	if (lf_get32(ib) != LFS_MAGIC_INDX)
		return false;
	fo = lf_get16(ib + IB_USA_OFS);
	fn = lf_get16(ib + IB_USA_COUNT);
	if (fo > LFS_SECTOR_SIZE - ((bytes >> LFS_SECTOR_SHIFT) + 1) * 2)
		return false;
	if ((uint32_t)(fn - 1) * LFS_SECTOR_SIZE != bytes)
		return false;
	return check_index_header(ib + IB_INDEX_HEADER, bytes - IB_INDEX_HEADER);
}

/* Is @ao an entry boundary inside the index root at @attr? */
static bool check_if_root_index(const uint8_t *attr, uint16_t ao)
{
	uint32_t asize = lf_get32(attr + AT_LENGTH);
	uint16_t vo = lf_get16(attr + AT_VALUE_OFFSET);
	const uint8_t *hdr = attr + vo + IR_INDEX_HEADER;
	uint32_t de_off = lf_get32(hdr + IH_ENTRIES_OFFSET);
	uint32_t o = vo + IR_INDEX_HEADER + de_off;

	while (o < ao) {
		uint16_t esize;
		if (o + IE_MIN_SIZE >= asize)
			break;
		esize = lf_get16(attr + o + IE_LENGTH);
		if (!esize)
			break;
		o += esize;
	}
	return o == ao;
}

static bool check_if_alloc_index(const uint8_t *ib, uint32_t attr_off)
{
	const uint8_t *hdr = ib + IB_INDEX_HEADER;
	uint32_t de_off = lf_get32(hdr + IH_ENTRIES_OFFSET);
	uint32_t used = lf_get32(hdr + IH_INDEX_LENGTH);
	uint32_t o = IB_INDEX_HEADER + de_off;

	while (o < attr_off) {
		uint16_t esize;
		if (de_off + IE_MIN_SIZE > used)
			break;
		esize = lf_get16(hdr + de_off + IE_LENGTH);
		if (!esize)
			break;
		o += esize;
		de_off += esize;
	}
	return o == attr_off;
}

/* Grow/shrink an attribute record inside an MFT record. */
static void change_attr_size(uint8_t *rec, uint8_t *attr, uint32_t nsize)
{
	uint32_t asize = lf_get32(attr + AT_LENGTH);
	uint32_t used = lf_get32(rec + MR_BYTES_IN_USE);
	uint32_t next_off = (uint32_t)(attr - rec) + asize;

	memmove(attr + nsize, attr + asize, used - next_off);
	lf_put32(rec + MR_BYTES_IN_USE, used + nsize - asize);
	lf_put32(attr + AT_LENGTH, nsize);
}

/* Bitmap set/clear on little-endian bit order. */
static void bitmap_op(uint8_t *bm, uint32_t bit, uint32_t nbits, bool set)
{
	while (nbits--) {
		if (set)
			bm[bit >> 3] |= (uint8_t)(1u << (bit & 7));
		else
			bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
		bit++;
	}
}

/* ---- Run lists --------------------------------------------------------- */

static int runs_add(struct run **runs, uint32_t *n, uint32_t *cap, uint64_t vcn, uint64_t lcn, uint64_t len)
{
	if (*n == *cap) {
		uint32_t ncap = *cap ? *cap * 2 : 16;
		struct run *nr = realloc(*runs, ncap * sizeof(*nr));
		if (!nr)
			return -ENOMEM;
		*runs = nr;
		*cap = ncap;
	}
	(*runs)[*n].vcn = vcn;
	(*runs)[*n].lcn = lcn;
	(*runs)[*n].len = len;
	(*n)++;
	return 0;
}

/* Decode NTFS mapping pairs starting at svcn. */
static int decode_mapping_pairs(struct open_attr *oa, const uint8_t *mp, uint32_t mp_len, uint64_t svcn)
{
	uint64_t vcn = svcn;
	int64_t lcn = 0;
	uint32_t i = 0;

	while (i < mp_len && mp[i]) {
		uint8_t hdr = mp[i++];
		uint8_t ls = hdr & 0xf, os = hdr >> 4;
		uint64_t len = 0;
		int64_t delta = 0;
		uint32_t k;
		int err;

		if (!ls || ls > 8 || os > 8 || i + ls + os > mp_len)
			return -EINVAL;
		for (k = 0; k < ls; k++)
			len |= (uint64_t)mp[i + k] << (8 * k);
		i += ls;
		if (!len)
			return -EINVAL;
		if (os) {
			for (k = 0; k < os; k++)
				delta |= (int64_t)mp[i + k] << (8 * k);
			if (mp[i + os - 1] & 0x80)
				delta |= -((int64_t)1 << (8 * os));
			i += os;
			lcn += delta;
			if (lcn < 0)
				return -EINVAL;
			err = runs_add(&oa->runs, &oa->nruns, &oa->cap_runs, vcn, (uint64_t)lcn, len);
		} else {
			err = runs_add(&oa->runs, &oa->nruns, &oa->cap_runs, vcn, SPARSE_LCN, len);
		}
		if (err)
			return err;
		vcn += len;
	}
	return 0;
}

static bool run_lookup(const struct open_attr *oa, uint64_t vcn, uint64_t *lcn)
{
	uint32_t i;

	for (i = 0; i < oa->novr; i++) {
		if (oa->ovr[i].vcn == vcn) {
			*lcn = oa->ovr[i].lcn;
			return true;
		}
	}
	for (i = 0; i < oa->nruns; i++) {
		const struct run *rl = &oa->runs[i];
		if (vcn >= rl->vcn && vcn < rl->vcn + rl->len) {
			*lcn = rl->lcn == SPARSE_LCN ? SPARSE_LCN : rl->lcn + (vcn - rl->vcn);
			return true;
		}
	}
	return false;
}

static int run_override(struct open_attr *oa, uint64_t vcn, uint64_t lcn)
{
	uint32_t i;

	for (i = 0; i < oa->novr; i++) {
		if (oa->ovr[i].vcn == vcn) {
			oa->ovr[i].lcn = lcn;
			return 0;
		}
	}
	return runs_add(&oa->ovr, &oa->novr, &oa->cap_ovr, vcn, lcn, 1);
}

/* Read/write @bytes of an attribute at byte offset @vbo through its runs. */
static int attr_io(struct replay *r, struct open_attr *oa, uint64_t vbo, uint8_t *buf, uint32_t bytes,
		   bool write, uint64_t *first_lcn)
{
	uint32_t cs = r->cs;

	if (first_lcn)
		*first_lcn = SPARSE_LCN;
	while (bytes) {
		uint64_t vcn = vbo / cs, lcn;
		uint32_t off = (uint32_t)(vbo % cs);
		uint32_t n = cs - off < bytes ? cs - off : bytes;
		struct ovl_clu *c;
		int err;

		if (!run_lookup(oa, vcn, &lcn) || lcn == SPARSE_LCN)
			return lfs_seterr(r->log, -EINVAL, "attribute vcn %llu unmapped", (unsigned long long)vcn);
		if (first_lcn && *first_lcn == SPARSE_LCN)
			*first_lcn = lcn;
		err = ovl_get_cluster(r, lcn, &c);
		if (err)
			return err;
		if (write) {
			memcpy(c->data + off, buf, n);
			c->dirty = true;
		} else {
			memcpy(buf, c->data + off, n);
		}
		buf += n;
		bytes -= n;
		vbo += n;
	}
	return 0;
}

/* ---- Open attributes --------------------------------------------------- */

static uint8_t *oa_entry(struct replay *r, uint32_t off)
{
	if (!lfs_table_entry_ok(&r->log->oatbl, off))
		return NULL;
	return r->log->oatbl.buf + off;
}

static struct open_attr *oa_slot(struct replay *r, uint32_t off)
{
	uint32_t idx;

	if (!r->log->oatbl.buf || off < RT_HEADER_SIZE)
		return NULL;
	idx = (off - RT_HEADER_SIZE) / lfs_table_esize(&r->log->oatbl);
	if (idx >= r->noa)
		return NULL;
	return &r->oa[idx];
}

static bool attr_matches(const uint8_t *attr, uint32_t type, const uint8_t *name, uint16_t name_len)
{
	if (lf_get32(attr + AT_TYPE) != type || attr[AT_NAME_LENGTH] != name_len)
		return false;
	return !name_len || !memcmp(attr + lf_get16(attr + AT_NAME_OFFSET), name, name_len * 2u);
}

static int oa_load_extent(struct replay *r, struct open_attr *oa, const uint8_t *attr)
{
	uint32_t alen = lf_get32(attr + AT_LENGTH);
	uint16_t mpo;

	if (attr[AT_NON_RESIDENT] == 0) {
		oa->resident = true;
		oa->data_size = lf_get32(attr + AT_VALUE_LENGTH);
		return 0;
	}
	mpo = lf_get16(attr + AT_MAPPING_PAIRS_OFFSET);
	if (mpo >= alen)
		return -EINVAL;
	if (!lf_get64(attr + AT_LOWEST_VCN)) {
		oa->alloc_size = lf_get64(attr + AT_ALLOCATED_SIZE);
		oa->data_size = lf_get64(attr + AT_DATA_SIZE);
	}
	return decode_mapping_pairs(oa, attr + mpo, alen - mpo, lf_get64(attr + AT_LOWEST_VCN));
}

/* Read the whole value of a resident-or-nonresident attribute in @rec. */
static int read_attr_value(struct replay *r, const uint8_t *attr, uint8_t **out, uint32_t *len)
{
	struct open_attr tmp;
	int err;

	if (attr[AT_NON_RESIDENT] == 0) {
		*len = lf_get32(attr + AT_VALUE_LENGTH);
		*out = malloc(*len ? *len : 1);
		if (!*out)
			return -ENOMEM;
		memcpy(*out, attr + lf_get16(attr + AT_VALUE_OFFSET), *len);
		return 0;
	}
	memset(&tmp, 0, sizeof(tmp));
	err = oa_load_extent(r, &tmp, attr);
	if (err) {
		free(tmp.runs);
		return err;
	}
	*len = (uint32_t)tmp.data_size;
	*out = malloc(*len ? *len : 1);
	if (!*out) {
		free(tmp.runs);
		return -ENOMEM;
	}
	err = attr_io(r, &tmp, 0, *out, *len, false, NULL);
	free(tmp.runs);
	if (err)
		free(*out);
	return err;
}

/*
 * Resolve an open attribute: find the attribute (type + name) in the base
 * record and, through $ATTRIBUTE_LIST, in extent records; collect its runs.
 */
static int oa_load(struct replay *r, uint32_t off, struct open_attr **out)
{
	struct open_attr *oa = oa_slot(r, off);
	const uint8_t *oe = oa_entry(r, off);
	struct ovl_mft *m;
	const uint8_t *rec;
	uint32_t ao, o, rs = r->rs;
	const uint8_t *attr_list = NULL;
	int err;

	*out = NULL;
	if (!oa || !oe)
		return lfs_seterr(r->log, -EINVAL, "open attribute entry 0x%x not allocated", off);
	if (oa->loaded)
		goto done;
	oa->loaded = true;
	if (r->log->crst_major) {
		oa->type = lf_get32(oe + OA1_TYPE);
		oa->bytes_per_index = lf_get32(oe + OA1_BYTES_PER_INDEX);
		mft_ref_no(oe + OA1_FILE_REF, &oa->mft_no, NULL);
	} else {
		oa->type = lf_get32(oe + OA0_TYPE);
		oa->bytes_per_index = lf_get32(oe + OA0_BYTES_PER_INDEX);
		mft_ref_no(oe + OA0_FILE_REF, &oa->mft_no, NULL);
	}
	/* Name from the attribute names dump. */
	if (r->log->attr_names) {
		uint32_t p = 0;
		while (p + AN_NAME <= r->log->attr_names_len) {
			uint16_t eo = lf_get16(r->log->attr_names + p + AN_OFFSET);
			uint16_t nb = lf_get16(r->log->attr_names + p + AN_NAME_BYTES);
			if (!eo)
				break;
			if (p + AN_NAME + nb > r->log->attr_names_len)
				break;
			if (eo == off && nb <= sizeof(oa->name)) {
				oa->name_len = nb / 2;
				memcpy(oa->name, r->log->attr_names + p + AN_NAME, nb);
			}
			p += AN_NAME + nb;
		}
	}

	err = ovl_get_mft(r, oa->mft_no, false, &m);
	if (err) {
		oa->failed = true;
		return err;
	}
	rec = m->rec;
	if (m->bad || !check_file_record(r, rec)) {
		oa->failed = true;
		return lfs_seterr(r->log, -EINVAL, "open attribute 0x%x: mft record %llu unusable", off,
				  (unsigned long long)oa->mft_no);
	}
	ao = lf_get16(rec + MR_ATTRS_OFFSET);
	for (o = ao; lf_get32(rec + o + AT_TYPE) != ATTR_TYPE_END; o += lf_get32(rec + o + AT_LENGTH)) {
		const uint8_t *attr = rec + o;
		if (lf_get32(attr + AT_TYPE) == ATTR_TYPE_ATTRIBUTE_LIST)
			attr_list = attr;
		if (attr_matches(attr, oa->type, oa->name, oa->name_len)) {
			err = oa_load_extent(r, oa, attr);
			if (err) {
				oa->failed = true;
				return err;
			}
		}
	}
	if (attr_list) {
		uint8_t *al;
		uint32_t al_len, p;

		err = read_attr_value(r, attr_list, &al, &al_len);
		if (err) {
			oa->failed = true;
			return err;
		}
		for (p = 0; p + AL_NAME <= al_len;) {
			uint16_t elen = lf_get16(al + p + AL_LENGTH);
			uint64_t ref_no;
			if (elen < AL_NAME || p + elen > al_len)
				break;
			mft_ref_no(al + p + AL_MFT_REFERENCE, &ref_no, NULL);
			if (lf_get32(al + p + AL_TYPE) == oa->type && al[p + AL_NAME_LENGTH] == oa->name_len &&
			    (!oa->name_len || !memcmp(al + p + al[p + AL_NAME_OFFSET], oa->name, oa->name_len * 2u)) &&
			    ref_no != oa->mft_no) {
				struct ovl_mft *em;
				uint64_t lvcn = lf_get64(al + p + AL_LOWEST_VCN);
				uint32_t eo;
				oa->in_extent = true;
				err = ovl_get_mft(r, ref_no, false, &em);
				if (err || em->bad || !check_file_record(r, em->rec)) {
					free(al);
					oa->failed = true;
					return lfs_seterr(r->log, -EINVAL, "open attribute 0x%x: extent record %llu unusable",
							  off, (unsigned long long)ref_no);
				}
				for (eo = lf_get16(em->rec + MR_ATTRS_OFFSET);
				     lf_get32(em->rec + eo + AT_TYPE) != ATTR_TYPE_END;
				     eo += lf_get32(em->rec + eo + AT_LENGTH)) {
					const uint8_t *attr = em->rec + eo;
					if (attr_matches(attr, oa->type, oa->name, oa->name_len) &&
					    attr[AT_NON_RESIDENT] && lf_get64(attr + AT_LOWEST_VCN) == lvcn) {
						err = oa_load_extent(r, oa, attr);
						if (err) {
							free(al);
							oa->failed = true;
							return err;
						}
					}
				}
			}
			p += elen;
		}
		free(al);
	}
	(void)rs;
done:
	if (oa->failed)
		return -EINVAL;
	*out = oa;
	return 0;
}

/*
 * An MFT operation changed a record: any open attribute resolved through
 * that record (base or extent) must be re-read from the overlay before it
 * is used again, since its run list or size may have changed. Dirty-page
 * overrides survive (they come from the log, not the record).
 */
static void oa_invalidate_mft(struct replay *r, uint64_t mft_no)
{
	uint32_t i;

	for (i = 0; i < r->noa; i++) {
		struct open_attr *oa = &r->oa[i];
		if (!oa->loaded)
			continue;
		if (oa->mft_no == mft_no || oa->in_extent) {
			free(oa->runs);
			oa->runs = NULL;
			oa->nruns = oa->cap_runs = 0;
			oa->loaded = false;
			oa->failed = false;
			oa->resident = false;
			oa->alloc_size = oa->data_size = 0;
		}
	}
	if (r->mft_loaded && mft_no == 0) {
		free(r->mftdata.runs);
		memset(&r->mftdata, 0, sizeof(r->mftdata));
		r->mft_loaded = false;
	}
}

/* $MFT:$DATA run list straight from record 0 (through the overlay). */
static int mft_runs_load(struct replay *r)
{
	struct ovl_mft *m;
	uint32_t o;
	int err;

	if (r->mft_loaded)
		return 0;
	err = ovl_get_mft(r, 0, false, &m);
	if (err)
		return err;
	if (m->bad || !check_file_record(r, m->rec))
		return lfs_seterr(r->log, -EINVAL, "mft record 0 unusable");
	memset(&r->mftdata, 0, sizeof(r->mftdata));
	for (o = lf_get16(m->rec + MR_ATTRS_OFFSET); lf_get32(m->rec + o + AT_TYPE) != ATTR_TYPE_END;
	     o += lf_get32(m->rec + o + AT_LENGTH)) {
		const uint8_t *attr = m->rec + o;
		if (lf_get32(attr + AT_TYPE) == ATTR_TYPE_DATA && !attr[AT_NAME_LENGTH] && attr[AT_NON_RESIDENT]) {
			err = oa_load_extent(r, &r->mftdata, attr);
			if (err)
				return lfs_seterr(r->log, -EINVAL, "mft record 0: undecodable $DATA runs");
			break;
		}
	}
	if (!r->mftdata.nruns)
		return lfs_seterr(r->log, -EINVAL, "mft record 0: no non-resident $DATA");
	r->mft_loaded = true;
	return 0;
}

/* ---- do_action --------------------------------------------------------- */

struct action_ctx {
	uint64_t lsn;			/* record lsn (redo) or 0 (undo) */
	const uint8_t *lr;		/* client record header */
	uint32_t rec_len;
	uint16_t op;
	const uint8_t *data;
	uint32_t dlen;
	bool undo;
};

static bool op_is_mft(uint16_t op)
{
	switch (op) {
	case LOP_InitializeFileRecordSegment:
	case LOP_DeallocateFileRecordSegment:
	case LOP_WriteEndOfFileRecordSegment:
	case LOP_CreateAttribute:
	case LOP_DeleteAttribute:
	case LOP_UpdateResidentValue:
	case LOP_UpdateMappingPairs:
	case LOP_SetNewAttributeSizes:
	case LOP_AddIndexEntryRoot:
	case LOP_DeleteIndexEntryRoot:
	case LOP_SetIndexEntryVcnRoot:
	case LOP_UpdateFileNameRoot:
	case LOP_UpdateRecordDataRoot:
	case LOP_ZeroEndOfFileRecord:
		return true;
	default:
		return false;
	}
}

static bool op_is_attr(uint16_t op)
{
	switch (op) {
	case LOP_UpdateNonresidentValue:
	case LOP_AddIndexEntryAllocation:
	case LOP_DeleteIndexEntryAllocation:
	case LOP_WriteEndOfIndexBuffer:
	case LOP_SetIndexEntryVcnAllocation:
	case LOP_UpdateFileNameAllocation:
	case LOP_SetBitsInNonresidentBitMap:
	case LOP_ClearBitsInNonresidentBitMap:
	case LOP_UpdateRecordDataAllocation:
		return true;
	default:
		return false;
	}
}

static bool can_skip_action(uint16_t op)
{
	switch (op) {
	case LOP_Noop:
	case LOP_DeleteDirtyClusters:
	case LOP_HotFix:
	case LOP_EndTopLevelAction:
	case LOP_PrepareTransaction:
	case LOP_CommitTransaction:
	case LOP_ForgetTransaction:
	case LOP_CompensationLogRecord:
	case LOP_OpenNonresidentAttribute:
	case LOP_OpenAttributeTableDump:
	case LOP_AttributeNamesDump:
	case LOP_DirtyPageTableDump:
	case LOP_TransactionTableDump:
		return true;
	default:
		return false;
	}
}

/* Skip when the target already carries this lsn or newer (redo only). */
static bool lsn_already_applied(const uint8_t *hdr, uint64_t rlsn)
{
	if (!rlsn)
		return false;
	if (lf_get32(hdr) == LFS_MAGIC_HOLE)
		return true;
	return lf_get64(hdr + 8) >= rlsn;
}

#define INCOHERENT(r, ...) (lfs_seterr((r)->log, -EINVAL, __VA_ARGS__))

static int do_action(struct replay *r, uint32_t target_attr, const struct action_ctx *a)
{
	const uint8_t *lr = a->lr;
	uint16_t roff = lf_get16(lr + NR_RECORD_OFFSET);
	uint16_t aoff = lf_get16(lr + NR_ATTRIBUTE_OFFSET);
	uint64_t cbo = (uint64_t)lf_get16(lr + NR_CLUSTER_BLOCK_OFFSET) << LFS_SECTOR_SHIFT;
	uint64_t tvo = lf_get64(lr + NR_TARGET_VCN) * r->cs;
	uint64_t vbo = cbo + tvo;
	uint32_t rs = r->rs, cs = r->cs;
	const uint8_t *data = a->data;
	uint32_t dlen = a->dlen;
	uint16_t op = a->op;
	struct ntfs_log_plan_entry pe;
	int err = 0;

	memset(&pe, 0, sizeof(pe));
	pe.lsn = a->lsn;
	pe.op = op;
	pe.undo = a->undo;
	pe.target_attr = target_attr;
	pe.target_vcn = lf_get64(lr + NR_TARGET_VCN);

	if (op_is_mft(op)) {
		uint64_t mft_no = vbo / rs;
		struct ovl_mft *m;
		uint8_t *rec, *attr;
		uint32_t asize, used, nsize, esize;
		struct open_attr *oa;
		uint16_t lcns = lf_get16(lr + NR_LCNS_TO_FOLLOW);

		/*
		 * Heuristic H1: an MFT operation must be logged against the
		 * $MFT:$DATA open attribute, and when the record carries the
		 * page's LCN it must agree with where $MFT's run list (plus
		 * dirty-page overrides) puts target_vcn. Otherwise the record
		 * number we derive from target_vcn could name the wrong record.
		 */
		err = oa_load(r, target_attr, &oa);
		if (err)
			return INCOHERENT(r, "lsn 0x%llx: mft op targets unloadable attribute 0x%x",
					  (unsigned long long)a->lsn, target_attr);
		if (oa->mft_no != 0 || oa->type != ATTR_TYPE_DATA || oa->name_len)
			return INCOHERENT(r, "lsn 0x%llx: mft op targets attribute 0x%x which is not $MFT:$DATA",
					  (unsigned long long)a->lsn, target_attr);
		if (lcns) {
			uint64_t tvcn = lf_get64(lr + NR_TARGET_VCN), lcn, logged;
			logged = lf_get64(lr + NR_PAGE_LCNS);
			if (logged && (!run_lookup(oa, tvcn, &lcn) || lcn != logged))
				return INCOHERENT(r, "lsn 0x%llx: logged lcn %llu for $MFT vcn %llu does not match run list",
						  (unsigned long long)a->lsn, (unsigned long long)logged,
						  (unsigned long long)tvcn);
		}
		if (mft_no >= (uint64_t)1 << 48)
			return INCOHERENT(r, "lsn 0x%llx: absurd mft record number", (unsigned long long)a->lsn);

		err = ovl_get_mft(r, mft_no, op == LOP_InitializeFileRecordSegment, &m);
		if (err)
			return err;
		rec = m->rec;
		if (op != LOP_DeallocateFileRecordSegment && op != LOP_InitializeFileRecordSegment) {
			if (m->bad)
				return INCOHERENT(r, "lsn 0x%llx: mft record %llu is BAAD",
						  (unsigned long long)a->lsn, (unsigned long long)mft_no);
			if (lsn_already_applied(rec, a->lsn))
				return plan_skip(r, a->lsn, op, a->undo, "mft record already at or past lsn");
			if (!check_file_record(r, rec))
				return INCOHERENT(r, "lsn 0x%llx: mft record %llu fails validation",
						  (unsigned long long)a->lsn, (unsigned long long)mft_no);
		} else if (op == LOP_DeallocateFileRecordSegment) {
			if (!m->bad && lsn_already_applied(rec, a->lsn))
				return plan_skip(r, a->lsn, op, a->undo, "mft record already at or past lsn");
			if (m->bad)
				return INCOHERENT(r, "lsn 0x%llx: cannot deallocate BAAD mft record %llu",
						  (unsigned long long)a->lsn, (unsigned long long)mft_no);
		} else {
			/* Initialize: skip only if a valid record already carries a later lsn (H2). */
			if (!m->bad && lf_get32(rec) == LFS_MAGIC_FILE && lsn_already_applied(rec, a->lsn))
				return plan_skip(r, a->lsn, op, a->undo, "mft record already at or past lsn");
		}
		if (roff >= rs)
			return INCOHERENT(r, "lsn 0x%llx: record offset %u out of range", (unsigned long long)a->lsn, roff);
		attr = rec + roff;

		switch (op) {
		case LOP_InitializeFileRecordSegment:
			if (roff + dlen > rs)
				return INCOHERENT(r, "InitializeFileRecordSegment overflows record");
			memcpy(rec + roff, data, dlen);
			break;
		case LOP_DeallocateFileRecordSegment:
			lf_put16(rec + MR_FLAGS, lf_get16(rec + MR_FLAGS) & ~MFT_RECORD_IN_USE);
			lf_put16(rec + MR_SEQUENCE, lf_get16(rec + MR_SEQUENCE) + 1);
			break;
		case LOP_WriteEndOfFileRecordSegment:
			if (!check_if_attr(rec, rs, roff) || roff + dlen > rs)
				return INCOHERENT(r, "WriteEndOfFileRecordSegment: bad offsets");
			memmove(attr, data, dlen);
			lf_put32(rec + MR_BYTES_IN_USE, LF_ALIGN8(roff + dlen));
			break;
		case LOP_CreateAttribute:
			if (dlen < AT_RESIDENT_SIZE)
				return INCOHERENT(r, "CreateAttribute: short data");
			asize = lf_get32(data + AT_LENGTH);
			used = lf_get32(rec + MR_BYTES_IN_USE);
			if (!check_if_attr(rec, rs, roff) || (asize & 7) || asize > dlen || asize < AT_RESIDENT_SIZE ||
			    asize > rs - used || roff > used)
				return INCOHERENT(r, "CreateAttribute: inconsistent sizes");
			memmove(attr + asize, attr, used - roff);
			memcpy(attr, data, asize);
			lf_put32(rec + MR_BYTES_IN_USE, used + asize);
			if (lf_get16(rec + MR_NEXT_ATTR_ID) <= lf_get16(data + AT_INSTANCE))
				lf_put16(rec + MR_NEXT_ATTR_ID, lf_get16(data + AT_INSTANCE) + 1);
			if (!attr[AT_NON_RESIDENT] && (attr[AT_RES_FLAGS] & ATTR_RES_FLAG_INDEXED))
				lf_put16(rec + MR_LINK_COUNT, lf_get16(rec + MR_LINK_COUNT) + 1);
			break;
		case LOP_DeleteAttribute:
			if (!check_if_attr(rec, rs, roff))
				return INCOHERENT(r, "DeleteAttribute: bad offset");
			asize = lf_get32(attr + AT_LENGTH);
			used = lf_get32(rec + MR_BYTES_IN_USE);
			if (!asize || roff + asize > used)
				return INCOHERENT(r, "DeleteAttribute: bad size");
			if (!attr[AT_NON_RESIDENT] && (attr[AT_RES_FLAGS] & ATTR_RES_FLAG_INDEXED))
				lf_put16(rec + MR_LINK_COUNT, lf_get16(rec + MR_LINK_COUNT) - 1);
			memmove(attr, attr + asize, used - asize - roff);
			lf_put32(rec + MR_BYTES_IN_USE, used - asize);
			break;
		case LOP_UpdateResidentValue: {
			uint16_t data_off;
			nsize = aoff + dlen;
			if (!check_if_attr(rec, rs, roff))
				return INCOHERENT(r, "UpdateResidentValue: bad offset");
			asize = lf_get32(attr + AT_LENGTH);
			used = lf_get32(rec + MR_BYTES_IN_USE);
			if (lf_get16(lr + NR_REDO_LENGTH) == lf_get16(lr + NR_UNDO_LENGTH)) {
				if (nsize > asize)
					return INCOHERENT(r, "UpdateResidentValue: overflow");
				memmove(attr + aoff, data, dlen);
				break;
			}
			if (nsize > asize && nsize - asize > rs - used)
				return INCOHERENT(r, "UpdateResidentValue: no room");
			nsize = LF_ALIGN8(nsize);
			data_off = lf_get16(attr + AT_VALUE_OFFSET);
			if (nsize < asize) {
				memmove(attr + aoff, data, dlen);
				data = NULL;
			}
			memmove(attr + nsize, attr + asize, used - roff - asize);
			lf_put32(rec + MR_BYTES_IN_USE, used + nsize - asize);
			lf_put32(attr + AT_LENGTH, nsize);
			lf_put32(attr + AT_VALUE_LENGTH, aoff + dlen - data_off);
			if (data)
				memmove(attr + aoff, data, dlen);
			break;
		}
		case LOP_UpdateMappingPairs: {
			nsize = aoff + dlen;
			asize = lf_get32(attr + AT_LENGTH);
			used = lf_get32(rec + MR_BYTES_IN_USE);
			if (!check_if_attr(rec, rs, roff) || !attr[AT_NON_RESIDENT] ||
			    aoff < lf_get16(attr + AT_MAPPING_PAIRS_OFFSET) || aoff > asize ||
			    (nsize > asize && nsize - asize > rs - used))
				return INCOHERENT(r, "UpdateMappingPairs: inconsistent");
			nsize = LF_ALIGN8(nsize);
			memmove(attr + nsize, attr + asize, used - roff - asize);
			lf_put32(rec + MR_BYTES_IN_USE, used + nsize - asize);
			lf_put32(attr + AT_LENGTH, nsize);
			memmove(attr + aoff, data, dlen);
			/* Recompute highest_vcn from the new mapping pairs. */
			{
				struct open_attr tmp;
				uint16_t mpo = lf_get16(attr + AT_MAPPING_PAIRS_OFFSET);
				memset(&tmp, 0, sizeof(tmp));
				if (decode_mapping_pairs(&tmp, attr + mpo, nsize - mpo, lf_get64(attr + AT_LOWEST_VCN))) {
					free(tmp.runs);
					return INCOHERENT(r, "UpdateMappingPairs: undecodable runs");
				}
				if (tmp.nruns)
					lf_put64(attr + AT_HIGHEST_VCN,
						 tmp.runs[tmp.nruns - 1].vcn + tmp.runs[tmp.nruns - 1].len - 1);
				free(tmp.runs);
			}
			break;
		}
		case LOP_SetNewAttributeSizes:
			if (!check_if_attr(rec, rs, roff) || !attr[AT_NON_RESIDENT] || dlen < 0x18)
				return INCOHERENT(r, "SetNewAttributeSizes: inconsistent");
			lf_put64(attr + AT_ALLOCATED_SIZE, lf_get64(data + NAS_ALLOC));
			lf_put64(attr + AT_DATA_SIZE, lf_get64(data + NAS_DATA));
			lf_put64(attr + AT_INITIALIZED_SIZE, lf_get64(data + NAS_VALID));
			if (dlen >= NAS_SIZE && lf_get32(attr + AT_LENGTH) >= AT_NONRESIDENT_EX_SIZE)
				lf_put64(attr + AT_COMPRESSED_SIZE, lf_get64(data + NAS_TOTAL));
			break;
		case LOP_AddIndexEntryRoot: {
			uint8_t *hdr, *e1;
			if (dlen < IE_MIN_SIZE)
				return INCOHERENT(r, "AddIndexEntryRoot: short data");
			esize = lf_get16(data + IE_LENGTH);
			if (!check_if_index_root(rec, rs, roff) || !check_if_root_index(attr, aoff) ||
			    esize > dlen || esize < IE_MIN_SIZE ||
			    esize > lf_get32(rec + MR_BYTES_ALLOCATED) - lf_get32(rec + MR_BYTES_IN_USE))
				return INCOHERENT(r, "AddIndexEntryRoot: inconsistent");
			hdr = attr + lf_get16(attr + AT_VALUE_OFFSET) + IR_INDEX_HEADER;
			used = lf_get32(hdr + IH_INDEX_LENGTH);
			e1 = attr + aoff;
			if (e1 > hdr + used)
				return INCOHERENT(r, "AddIndexEntryRoot: entry past used");
			change_attr_size(rec, attr, lf_get32(attr + AT_LENGTH) + esize);
			memmove(e1 + esize, e1, (size_t)(hdr + used - e1));
			memmove(e1, data, esize);
			lf_put32(attr + AT_VALUE_LENGTH, lf_get32(attr + AT_VALUE_LENGTH) + esize);
			lf_put32(hdr + IH_INDEX_LENGTH, used + esize);
			lf_put32(hdr + IH_ALLOCATED_SIZE, lf_get32(hdr + IH_ALLOCATED_SIZE) + esize);
			break;
		}
		case LOP_DeleteIndexEntryRoot: {
			uint8_t *hdr, *e1, *e2;
			if (!check_if_index_root(rec, rs, roff) || !check_if_root_index(attr, aoff))
				return INCOHERENT(r, "DeleteIndexEntryRoot: inconsistent");
			hdr = attr + lf_get16(attr + AT_VALUE_OFFSET) + IR_INDEX_HEADER;
			used = lf_get32(hdr + IH_INDEX_LENGTH);
			e1 = attr + aoff;
			esize = lf_get16(e1 + IE_LENGTH);
			if (e1 + esize > hdr + used || !esize)
				return INCOHERENT(r, "DeleteIndexEntryRoot: entry past used");
			e2 = e1 + esize;
			memmove(e1, e2, (size_t)(hdr + used - e2));
			lf_put32(attr + AT_VALUE_LENGTH, lf_get32(attr + AT_VALUE_LENGTH) - esize);
			lf_put32(hdr + IH_INDEX_LENGTH, used - esize);
			lf_put32(hdr + IH_ALLOCATED_SIZE, lf_get32(hdr + IH_ALLOCATED_SIZE) - esize);
			change_attr_size(rec, attr, lf_get32(attr + AT_LENGTH) - esize);
			break;
		}
		case LOP_SetIndexEntryVcnRoot: {
			uint8_t *e;
			if (!check_if_index_root(rec, rs, roff) || !check_if_root_index(attr, aoff) || dlen < 8)
				return INCOHERENT(r, "SetIndexEntryVcnRoot: inconsistent");
			e = attr + aoff;
			esize = lf_get16(e + IE_LENGTH);
			if (esize < IE_MIN_SIZE + 8 || aoff + esize > lf_get32(attr + AT_LENGTH))
				return INCOHERENT(r, "SetIndexEntryVcnRoot: bad entry");
			lf_put64(e + esize - 8, lf_get64(data));
			break;
		}
		case LOP_UpdateFileNameRoot: {
			uint8_t *e;
			if (!check_if_index_root(rec, rs, roff) || !check_if_root_index(attr, aoff) || dlen < FN_DUP_SIZE)
				return INCOHERENT(r, "UpdateFileNameRoot: inconsistent");
			e = attr + aoff;
			if (aoff + IE_KEY + FN_DUP_OFFSET + FN_DUP_SIZE > lf_get32(attr + AT_LENGTH))
				return INCOHERENT(r, "UpdateFileNameRoot: entry overflow");
			memmove(e + IE_KEY + FN_DUP_OFFSET, data, FN_DUP_SIZE);
			break;
		}
		case LOP_UpdateRecordDataRoot: {
			uint8_t *e;
			uint16_t doff;
			if (!check_if_index_root(rec, rs, roff) || !check_if_root_index(attr, aoff))
				return INCOHERENT(r, "UpdateRecordDataRoot: inconsistent");
			e = attr + aoff;
			doff = lf_get16(e + IE_DATA_OFFSET);
			if (aoff + doff + dlen > lf_get32(attr + AT_LENGTH))
				return INCOHERENT(r, "UpdateRecordDataRoot: overflow");
			memmove(e + doff, data, dlen);
			break;
		}
		case LOP_ZeroEndOfFileRecord:
			if (roff + dlen > rs)
				return INCOHERENT(r, "ZeroEndOfFileRecord: overflow");
			memset(attr, 0, dlen);
			break;
		default:
			return INCOHERENT(r, "unexpected mft op %u", op);
		}
		if (a->lsn)
			lf_put64(rec + MR_LSN, a->lsn);
		m->dirty = true;
		m->bad = false;
		switch (op) {
		case LOP_InitializeFileRecordSegment:
		case LOP_DeallocateFileRecordSegment:
		case LOP_WriteEndOfFileRecordSegment:
		case LOP_CreateAttribute:
		case LOP_DeleteAttribute:
		case LOP_UpdateResidentValue:
		case LOP_UpdateMappingPairs:
		case LOP_SetNewAttributeSizes:
		case LOP_ZeroEndOfFileRecord:
			oa_invalidate_mft(r, mft_no);
			break;
		default:
			break;
		}
		pe.kind = NTFS_PLAN_MFT_RECORD;
		pe.mft_no = mft_no;
		return lfs_plan_add(r->log, &pe);
	}

	if (op_is_attr(op)) {
		struct open_attr *oa;
		uint64_t lco = (uint64_t)lf_get16(lr + NR_LCNS_TO_FOLLOW) * cs;
		uint32_t bytes;
		uint8_t *buf, *ib, *hdr, *e;
		uint64_t first_lcn;
		bool deprotected = false;

		err = oa_load(r, target_attr, &oa);
		if (err)
			return err;
		bytes = op == LOP_UpdateNonresidentValue ? dlen : 0;
		if (oa->type == ATTR_TYPE_INDEX_ALLOCATION && bytes < oa->bytes_per_index)
			bytes = oa->bytes_per_index;
		if (!bytes) {
			if (lco <= cbo)
				return INCOHERENT(r, "lsn 0x%llx: cluster offset beyond logged clusters",
						  (unsigned long long)a->lsn);
			bytes = (uint32_t)(lco - cbo);
		}
		bytes += roff;
		if (oa->type == ATTR_TYPE_INDEX_ALLOCATION)
			bytes = (bytes + 511) & ~511u;
		if (bytes > 64u * 1024 * 1024)
			return INCOHERENT(r, "lsn 0x%llx: absurd transfer size %u", (unsigned long long)a->lsn, bytes);
		buf = malloc(bytes);
		if (!buf)
			return -ENOMEM;
		err = attr_io(r, oa, vbo, buf, bytes, false, &first_lcn);
		if (err) {
			free(buf);
			return err;
		}
		ib = buf + roff;
		if (oa->type == ATTR_TYPE_INDEX_ALLOCATION && lf_get32(ib) == LFS_MAGIC_INDX) {
			bool torn = false;
			if (lfs_fixup_post_read(ib, bytes - roff, LFS_SECTOR_SIZE, &torn) || torn) {
				free(buf);
				return INCOHERENT(r, "lsn 0x%llx: index block torn", (unsigned long long)a->lsn);
			}
			deprotected = true;
		}

		switch (op) {
		case LOP_UpdateNonresidentValue:
			if (lco < cbo + roff + dlen) {
				free(buf);
				return INCOHERENT(r, "UpdateNonresidentValue: overflow");
			}
			memcpy(buf + roff, data, dlen);
			if (deprotected && lfs_fixup_pre_write(ib, bytes - roff, LFS_SECTOR_SIZE)) {
				free(buf);
				return INCOHERENT(r, "UpdateNonresidentValue: cannot re-protect index block");
			}
			break;
		case LOP_SetBitsInNonresidentBitMap:
		case LOP_ClearBitsInNonresidentBitMap: {
			uint32_t boff, bits;
			if (dlen < BR_SIZE) {
				free(buf);
				return INCOHERENT(r, "bitmap op: short data");
			}
			boff = lf_get32(data + BR_BITMAP_OFF);
			bits = lf_get32(data + BR_BITS);
			if (cbo + ((uint64_t)boff + 7) / 8 > lco || cbo + ((uint64_t)boff + bits + 7) / 8 > lco ||
			    roff + ((uint64_t)boff + bits + 7) / 8 > bytes) {
				free(buf);
				return INCOHERENT(r, "bitmap op: range outside logged clusters");
			}
			bitmap_op(buf + roff, boff, bits, op == LOP_SetBitsInNonresidentBitMap);
			break;
		}
		default:
			/* Index allocation ops. */
			if (lf_get32(ib) == LFS_MAGIC_BAAD) {
				free(buf);
				return INCOHERENT(r, "index block is BAAD");
			}
			if (lsn_already_applied(ib, a->lsn)) {
				free(buf);
				return plan_skip(r, a->lsn, op, a->undo, "index block already at or past lsn");
			}
			if (!check_index_buffer(ib, bytes - roff) || !check_if_alloc_index(ib, aoff)) {
				free(buf);
				return INCOHERENT(r, "lsn 0x%llx: index block fails validation", (unsigned long long)a->lsn);
			}
			hdr = ib + IB_INDEX_HEADER;
			e = ib + aoff;
			{
				uint32_t used = lf_get32(hdr + IH_INDEX_LENGTH);
				uint32_t total = lf_get32(hdr + IH_ALLOCATED_SIZE);
				uint32_t esize;

				switch (op) {
				case LOP_AddIndexEntryAllocation:
					if (dlen < IE_MIN_SIZE)
						goto bad_ib;
					esize = lf_get16(data + IE_LENGTH);
					if (esize > dlen || used + esize > total || e > hdr + used)
						goto bad_ib;
					memmove(e + esize, e, (size_t)(hdr + used - e));
					memcpy(e, data, esize);
					lf_put32(hdr + IH_INDEX_LENGTH, used + esize);
					break;
				case LOP_DeleteIndexEntryAllocation:
					esize = lf_get16(e + IE_LENGTH);
					if (!esize || e + esize > hdr + used)
						goto bad_ib;
					memmove(e, e + esize, (size_t)(hdr + used - (e + esize)));
					lf_put32(hdr + IH_INDEX_LENGTH, used - esize);
					break;
				case LOP_WriteEndOfIndexBuffer:
					if (aoff + dlen > IB_INDEX_HEADER + total)
						goto bad_ib;
					lf_put32(hdr + IH_INDEX_LENGTH, dlen + (uint32_t)(e - hdr));
					memmove(e, data, dlen);
					break;
				case LOP_SetIndexEntryVcnAllocation:
					esize = lf_get16(e + IE_LENGTH);
					if (dlen < 8 || esize < IE_MIN_SIZE + 8 || e + esize > hdr + used)
						goto bad_ib;
					lf_put64(e + esize - 8, lf_get64(data));
					break;
				case LOP_UpdateFileNameAllocation:
					if (dlen < FN_DUP_SIZE || e + IE_KEY + FN_DUP_OFFSET + FN_DUP_SIZE > hdr + used)
						goto bad_ib;
					memmove(e + IE_KEY + FN_DUP_OFFSET, data, FN_DUP_SIZE);
					break;
				case LOP_UpdateRecordDataAllocation: {
					uint16_t doff = lf_get16(e + IE_DATA_OFFSET);
					if (e + doff + dlen > hdr + total)
						goto bad_ib;
					memmove(e + doff, data, dlen);
					break;
				}
				default:
					goto bad_ib;
				}
			}
			if (a->lsn)
				lf_put64(ib + IB_LSN, a->lsn);
			if (lfs_fixup_pre_write(ib, bytes - roff, LFS_SECTOR_SIZE)) {
bad_ib:
				free(buf);
				return INCOHERENT(r, "lsn 0x%llx: index operation %s inconsistent",
						  (unsigned long long)a->lsn, ntfs_log_op_name(op));
			}
			break;
		}
		err = attr_io(r, oa, vbo, buf, bytes, true, NULL);
		free(buf);
		if (err)
			return err;
		pe.kind = NTFS_PLAN_CLUSTERS;
		pe.lcn = first_lcn;
		pe.count = (bytes + (uint32_t)(vbo % cs) + cs - 1) / cs;
		return lfs_plan_add(r->log, &pe);
	}
	return INCOHERENT(r, "lsn 0x%llx: op %u (%s) has no apply handler",
			  (unsigned long long)a->lsn, op, ntfs_log_op_name(op));
}

/* ---- Dirty page table helpers ----------------------------------------- */

static uint8_t *find_dp(ntfs_logfile_t *log, uint32_t target_attr, uint64_t vcn)
{
	uint8_t *dp = NULL;

	while ((dp = lfs_table_next(&log->dptbl, dp))) {
		uint64_t dv = lf_get64(dp + DP1_VCN);
		if (lf_get32(dp + DP1_TARGET_ATTR) == target_attr && vcn >= dv &&
		    vcn < dv + lf_get32(dp + DP1_LCNS_FOLLOW))
			return dp;
	}
	return NULL;
}

/* ---- Passes ------------------------------------------------------------ */

static int analysis_pass(struct replay *r, uint64_t *rlsn_out)
{
	ntfs_logfile_t *log = r->log;
	struct lfs_record rec;
	uint64_t lsn = log->checkpoint_lsn, rlsn = 0;
	int err;

	if (!log->trtbl.buf) {
		err = lfs_table_init(&log->trtbl, TR_SIZE, 5);
		if (err)
			return err;
	}
	if (!log->oatbl.buf) {
		err = lfs_table_init(&log->oatbl, (uint16_t)log->bytes_per_attr_entry, 8);
		if (err)
			return err;
	}

	err = lfs_read_record(log, lsn, &rec);
	if (err)
		return err;
	for (;;) {
		uint64_t next;
		const uint8_t *lr;
		uint32_t tid;
		uint16_t redo_op;
		uint8_t *tr;

		err = lfs_next_lsn(log, &rec, &next);
		lfs_free_record(&rec);
		if (err)
			return err;
		if (!next)
			break;
		lsn = next;
		err = lfs_read_record(log, lsn, &rec);
		if (err)
			return err;
		r->res->records_analyzed++;
		if (!rlsn)
			rlsn = lsn;
		if (lf_get32(rec.hdr + LR_RECORD_TYPE) != LFS_RECORD_TYPE_CLIENT)
			continue;
		if (!lfs_check_client_rec(&rec, log->bytes_per_attr_entry)) {
			lfs_free_record(&rec);
			return lfs_seterr(log, -EINVAL, "analysis: malformed client record at lsn 0x%llx",
					  (unsigned long long)lsn);
		}
		lr = rec.data;
		tid = lf_get32(rec.hdr + LR_TRANSACTION_ID);
		redo_op = lf_get16(lr + NR_REDO_OP);

		/* Transaction table upkeep. */
		if (!lfs_table_entry_ok(&log->trtbl, tid)) {
			tr = lfs_table_alloc_at(&log->trtbl, tid);
			if (!tr) {
				lfs_free_record(&rec);
				return lfs_seterr(log, -EINVAL, "analysis: bad transaction id 0x%x", tid);
			}
			tr[TR_STATE] = TRANSACTION_ACTIVE;
			lf_put64(tr + TR_FIRST_LSN, lsn);
		}
		tr = log->trtbl.buf + tid;
		lf_put64(tr + TR_PREV_LSN, lsn);
		lf_put64(tr + TR_UNDO_NEXT_LSN, lsn);
		if (lf_get16(lr + NR_UNDO_OP) == LOP_CompensationLogRecord)
			lf_put64(tr + TR_UNDO_NEXT_LSN, lf_get64(rec.hdr + LR_CLIENT_UNDO_NEXT_LSN));

		if (op_is_mft(redo_op) || op_is_attr(redo_op)) {
			uint16_t ta = lf_get16(lr + NR_TARGET_ATTRIBUTE);
			uint64_t tvcn = lf_get64(lr + NR_TARGET_VCN);
			uint16_t lcns = lf_get16(lr + NR_LCNS_TO_FOLLOW);
			uint8_t *dp = find_dp(log, ta, tvcn);
			uint32_t per_page, i;

			if (!dp) {
				if (log->dptbl.buf) {
					per_page = (lfs_table_esize(&log->dptbl) - DP1_SIZE) / 8;
				} else {
					per_page = log->clst_per_page;
					err = lfs_table_init(&log->dptbl, (uint16_t)(DP1_SIZE + 8 * per_page), 32);
					if (err) {
						lfs_free_record(&rec);
						return err;
					}
				}
				dp = lfs_table_alloc(&log->dptbl);
				if (!dp) {
					lfs_free_record(&rec);
					return -ENOMEM;
				}
				lf_put32(dp + DP1_TARGET_ATTR, ta);
				lf_put32(dp + DP1_TRANSFER_LEN, per_page * r->cs);
				lf_put32(dp + DP1_LCNS_FOLLOW, per_page);
				lf_put64(dp + DP1_VCN, tvcn & ~((uint64_t)per_page - 1));
				lf_put64(dp + DP1_OLDEST_LSN, lsn);
			}
			per_page = lf_get32(dp + DP1_LCNS_FOLLOW);
			for (i = 0; i < lcns; i++) {
				uint64_t j = tvcn - lf_get64(dp + DP1_VCN) + i;
				if (j < per_page)
					lf_put64(dp + DP1_PAGE_LCNS + 8 * j, lf_get64(lr + NR_PAGE_LCNS + 8 * i));
			}
			continue;
		}
		switch (redo_op) {
		case LOP_DeleteDirtyClusters: {
			uint16_t rlen = lf_get16(lr + NR_REDO_LENGTH);
			const uint8_t *rg = lr + lf_get16(lr + NR_REDO_OFFSET);
			uint32_t n = rlen / LR_RANGE_SIZE, k;
			for (k = 0; k < n; k++) {
				uint64_t l0 = lf_get64(rg + k * LR_RANGE_SIZE);
				uint64_t le = l0 + lf_get64(rg + k * LR_RANGE_SIZE + 8) - 1;
				uint8_t *dp = NULL;
				while ((dp = lfs_table_next(&log->dptbl, dp))) {
					uint32_t cnt = lf_get32(dp + DP1_LCNS_FOLLOW), j;
					for (j = 0; j < cnt; j++) {
						uint64_t l = lf_get64(dp + DP1_PAGE_LCNS + 8 * j);
						if (l >= l0 && l <= le)
							lf_put64(dp + DP1_PAGE_LCNS + 8 * j, 0);
					}
				}
			}
			break;
		}
		case LOP_OpenNonresidentAttribute: {
			uint16_t ta = lf_get16(lr + NR_TARGET_ATTRIBUTE);
			uint16_t redo_off = lf_get16(lr + NR_REDO_OFFSET);
			uint8_t *oe = lfs_table_alloc_at(&log->oatbl, ta);
			if (!oe || redo_off + log->bytes_per_attr_entry > rec.data_len) {
				lfs_free_record(&rec);
				return lfs_seterr(log, -EINVAL, "analysis: bad OpenNonresidentAttribute");
			}
			memcpy(oe + 4, lr + redo_off + 4, log->bytes_per_attr_entry - 4);
			/* The undo data carries the attribute name; stash it in the
			 * names blob so oa_load finds it. */
			if (lf_get16(lr + NR_UNDO_LENGTH)) {
				uint16_t nb = lf_get16(lr + NR_UNDO_LENGTH);
				uint32_t nl = log->attr_names_len;
				uint8_t *nn = realloc(log->attr_names, nl + AN_NAME + nb + AN_NAME);
				if (!nn) {
					lfs_free_record(&rec);
					return -ENOMEM;
				}
				/* Drop the terminating zero entry if present. */
				if (nl >= AN_NAME && !lf_get16(nn + nl - AN_NAME))
					nl -= AN_NAME;
				lf_put16(nn + nl + AN_OFFSET, ta);
				lf_put16(nn + nl + AN_NAME_BYTES, nb);
				memcpy(nn + nl + AN_NAME, lr + lf_get16(lr + NR_UNDO_OFFSET), nb);
				lf_put32(nn + nl + AN_NAME + nb, 0);
				log->attr_names = nn;
				log->attr_names_len = nl + AN_NAME + nb + AN_NAME;
			}
			break;
		}
		case LOP_HotFix: {
			uint16_t ta = lf_get16(lr + NR_TARGET_ATTRIBUTE);
			uint64_t tvcn = lf_get64(lr + NR_TARGET_VCN);
			uint8_t *dp = find_dp(log, ta, tvcn);
			if (dp) {
				uint64_t j = tvcn - lf_get64(dp + DP1_VCN);
				if (lf_get64(dp + DP1_PAGE_LCNS + 8 * j))
					lf_put64(dp + DP1_PAGE_LCNS + 8 * j, lf_get64(lr + NR_PAGE_LCNS));
			}
			break;
		}
		case LOP_EndTopLevelAction:
			lf_put64(tr + TR_PREV_LSN, lsn);
			lf_put64(tr + TR_UNDO_NEXT_LSN, lf_get64(rec.hdr + LR_CLIENT_UNDO_NEXT_LSN));
			break;
		case LOP_PrepareTransaction:
			tr[TR_STATE] = TRANSACTION_PREPARED;
			break;
		case LOP_CommitTransaction:
			tr[TR_STATE] = TRANSACTION_COMMITTED;
			break;
		case LOP_ForgetTransaction:
			lfs_table_free_idx(&log->trtbl, tid);
			break;
		default:
			break;
		}
	}

	/* Redo lsn: lowest of dirty page oldest lsns and transaction first lsns. */
	{
		uint8_t *e = NULL;
		while ((e = lfs_table_next(&log->dptbl, e))) {
			uint64_t l = lf_get64(e + DP1_OLDEST_LSN);
			if (l && (!rlsn || l < rlsn))
				rlsn = l;
		}
		e = NULL;
		while ((e = lfs_table_next(&log->trtbl, e))) {
			uint64_t l = lf_get64(e + TR_FIRST_LSN);
			if (l && (!rlsn || l < rlsn))
				rlsn = l;
			if (e[TR_STATE] == TRANSACTION_COMMITTED)
				r->res->transactions_committed++;
			else if (e[TR_STATE] == TRANSACTION_ACTIVE)
				r->res->transactions_active++;
		}
	}
	r->res->dirty_pages = lfs_table_total(&log->dptbl);
	*rlsn_out = rlsn;
	return 0;
}

/* Seed run overrides from the dirty page table. */
static int prepare_dirty_pages(struct replay *r)
{
	ntfs_logfile_t *log = r->log;
	uint8_t *dp = NULL;
	int err;

	while ((dp = lfs_table_next(&log->dptbl, dp))) {
		uint32_t ta = lf_get32(dp + DP1_TARGET_ATTR);
		uint32_t cnt = lf_get32(dp + DP1_LCNS_FOLLOW), i;
		struct open_attr *oa;

		if (!lfs_table_entry_ok(&log->oatbl, ta))
			continue;
		err = oa_load(r, ta, &oa);
		if (err) {
			/* Attribute no longer exists: its pages cannot matter. */
			lfs_msg(log, 1, "dirty page for unavailable attribute 0x%x ignored", ta);
			continue;
		}
		for (i = 0; i < cnt; i++) {
			uint64_t vcn = lf_get64(dp + DP1_VCN) + i;
			uint64_t lcn = lf_get64(dp + DP1_PAGE_LCNS + 8 * i);
			uint64_t size = (vcn + 1) * r->cs, cur;

			if (!lcn)
				continue;
			/* Never remap the first system records of $MFT. */
			if (oa->mft_no <= MFT_REC_MIRR && oa->type == ATTR_TYPE_DATA &&
			    size < (uint64_t)(MFT_REC_VOL + 1) * r->rs)
				continue;
			if (run_lookup(oa, vcn, &cur) && cur == lcn)
				continue;
			err = run_override(oa, vcn, lcn);
			if (err)
				return err;
		}
	}
	return 0;
}

static int redo_pass(struct replay *r, uint64_t rlsn)
{
	ntfs_logfile_t *log = r->log;
	struct lfs_record rec;
	uint64_t lsn = rlsn;
	int err;

	if (!lfs_table_total(&log->dptbl) || !rlsn)
		return 0;
	err = lfs_read_record(log, lsn, &rec);
	if (err)
		return err;
	for (;;) {
		const uint8_t *lr = rec.data;
		uint16_t lcns, ta, op;
		uint64_t tvcn, lcn;
		uint8_t *dp;
		struct open_attr *oa;
		struct action_ctx a;
		uint32_t dlen, saved_len, i;
		uint64_t next;

		if (lf_get32(rec.hdr + LR_RECORD_TYPE) != LFS_RECORD_TYPE_CLIENT)
			goto next_rec;
		if (!lfs_check_client_rec(&rec, log->bytes_per_attr_entry)) {
			lfs_free_record(&rec);
			return lfs_seterr(log, -EINVAL, "redo: malformed client record at lsn 0x%llx",
					  (unsigned long long)lsn);
		}
		lcns = lf_get16(lr + NR_LCNS_TO_FOLLOW);
		if (!lcns)
			goto next_rec;
		ta = lf_get16(lr + NR_TARGET_ATTRIBUTE);
		tvcn = lf_get64(lr + NR_TARGET_VCN);
		dp = find_dp(log, ta, tvcn);
		if (!dp || lsn < lf_get64(dp + DP1_OLDEST_LSN))
			goto next_rec;
		err = oa_load(r, ta, &oa);
		if (err) {
			lfs_free_record(&rec);
			return lfs_seterr(log, -EINVAL, "redo: lsn 0x%llx targets unloadable attribute 0x%x",
					  (unsigned long long)lsn, ta);
		}
		if (!run_lookup(oa, tvcn, &lcn) || lcn == SPARSE_LCN) {
			err = plan_skip(r, lsn, lf_get16(lr + NR_REDO_OP), false, "target vcn not allocated");
			if (err)
				goto fail;
			goto next_rec;
		}
		dlen = lf_get16(lr + NR_REDO_LENGTH);
		saved_len = dlen;
		/* Shorten by clusters deleted since (DeleteDirtyClusters zeroed them). */
		for (i = lcns; i; i--) {
			uint64_t j = tvcn - lf_get64(dp + DP1_VCN);
			uint32_t voff = lf_get16(lr + NR_RECORD_OFFSET) + lf_get16(lr + NR_ATTRIBUTE_OFFSET) +
					(lf_get16(lr + NR_CLUSTER_BLOCK_OFFSET) << LFS_SECTOR_SHIFT);
			uint32_t alen;
			if (lf_get64(dp + DP1_PAGE_LCNS + 8 * (j + i - 1)))
				break;
			if (!saved_len)
				saved_len = 1;
			alen = (i - 1) * r->cs;
			if (voff >= alen)
				dlen = 0;
			else if (voff + dlen > alen)
				dlen = alen - voff;
		}
		if (!dlen && saved_len)
			goto next_rec;
		op = lf_get16(lr + NR_REDO_OP);
		if (can_skip_action(op))
			goto next_rec;
		memset(&a, 0, sizeof(a));
		a.lsn = lsn;
		a.lr = lr;
		a.rec_len = rec.data_len;
		a.op = op;
		a.data = lr + lf_get16(lr + NR_REDO_OFFSET);
		a.dlen = dlen;
		err = do_action(r, ta, &a);
		if (err)
			goto fail;
		r->res->records_redone++;
next_rec:
		err = lfs_next_lsn(log, &rec, &next);
		lfs_free_record(&rec);
		if (err)
			return err;
		if (!next)
			return 0;
		lsn = next;
		err = lfs_read_record(log, lsn, &rec);
		if (err)
			return err;
		continue;
fail:
		lfs_free_record(&rec);
		return err;
	}
}

static int undo_pass(struct replay *r)
{
	ntfs_logfile_t *log = r->log;
	uint8_t *tr = NULL;
	int err;

	while ((tr = lfs_table_next(&log->trtbl, tr))) {
		uint64_t lsn = lf_get64(tr + TR_UNDO_NEXT_LSN);
		struct lfs_record rec;

		if (tr[TR_STATE] != TRANSACTION_ACTIVE || !lsn)
			continue;
		while (lsn) {
			const uint8_t *lr;
			uint16_t op, ta;
			struct action_ctx a;

			if (!lfs_lsn_in_file(log, lsn))
				break;
			err = lfs_read_record(log, lsn, &rec);
			if (err)
				return err;
			if (lf_get32(rec.hdr + LR_RECORD_TYPE) != LFS_RECORD_TYPE_CLIENT ||
			    !lfs_check_client_rec(&rec, log->bytes_per_attr_entry)) {
				lfs_free_record(&rec);
				return lfs_seterr(log, -EINVAL, "undo: malformed record at lsn 0x%llx",
						  (unsigned long long)lsn);
			}
			lr = rec.data;
			op = lf_get16(lr + NR_UNDO_OP);
			ta = lf_get16(lr + NR_TARGET_ATTRIBUTE);
			if (op != LOP_Noop && !can_skip_action(op)) {
				memset(&a, 0, sizeof(a));
				a.lsn = 0;
				a.lr = lr;
				a.rec_len = rec.data_len;
				a.op = op;
				a.undo = true;
				a.data = lr + lf_get16(lr + NR_UNDO_OFFSET);
				a.dlen = lf_get16(lr + NR_UNDO_LENGTH);
				err = do_action(r, ta, &a);
				if (err) {
					lfs_free_record(&rec);
					return err;
				}
				r->res->records_undone++;
			}
			lsn = lf_get64(rec.hdr + LR_CLIENT_UNDO_NEXT_LSN);
			lfs_free_record(&rec);
		}
	}
	return 0;
}

/* ---- Flush ------------------------------------------------------------- */

static int flush_overlay(struct replay *r)
{
	uint32_t i;
	int err;
	uint8_t *mirr_rec = NULL;
	struct open_attr mirr;

	memset(&mirr, 0, sizeof(mirr));
	for (i = 0; i < r->nmft; i++) {
		struct ovl_mft *m = &r->mft[i];
		uint8_t *copy;

		if (!m->dirty)
			continue;
		copy = malloc(r->rs);
		if (!copy)
			return -ENOMEM;
		memcpy(copy, m->rec, r->rs);
		if (lf_get32(copy) == LFS_MAGIC_FILE && lfs_fixup_pre_write(copy, r->rs, LFS_SECTOR_SIZE)) {
			free(copy);
			return lfs_seterr(r->log, -EINVAL, "mft record %llu: cannot protect",
					  (unsigned long long)m->mft_no);
		}
		err = r->ap->write_mft_record(r->ap->ctx, m->mft_no, copy);
		if (err) {
			free(copy);
			return lfs_seterr(r->log, err < 0 ? err : -EIO, "write mft record %llu failed",
					  (unsigned long long)m->mft_no);
		}
		r->res->mft_records_written++;
		/* Keep $MFTMirr in sync for the first records. */
		if (m->mft_no <= MFT_REC_VOL) {
			if (!mirr_rec) {
				struct ovl_mft *mm;
				if (!ovl_get_mft(r, MFT_REC_MIRR, false, &mm) && !mm->bad && check_file_record(r, mm->rec)) {
					uint32_t o;
					for (o = lf_get16(mm->rec + MR_ATTRS_OFFSET);
					     lf_get32(mm->rec + o + AT_TYPE) != ATTR_TYPE_END;
					     o += lf_get32(mm->rec + o + AT_LENGTH)) {
						if (lf_get32(mm->rec + o + AT_TYPE) == ATTR_TYPE_DATA && !mm->rec[o + AT_NAME_LENGTH]) {
							oa_load_extent(r, &mirr, mm->rec + o);
							break;
						}
					}
				}
				mirr_rec = copy;
			}
			if (mirr.nruns && (uint64_t)(m->mft_no + 1) * r->rs <= mirr.data_size) {
				uint64_t lcn;
				uint64_t vbo = m->mft_no * (uint64_t)r->rs;
				if (run_lookup(&mirr, vbo / r->cs, &lcn) && lcn != SPARSE_LCN) {
					uint8_t *cl = malloc(r->cs);
					if (cl && !r->ap->read_clusters(r->ap->ctx, lcn, 1, cl)) {
						memcpy(cl + vbo % r->cs, copy, r->rs);
						r->ap->write_clusters(r->ap->ctx, lcn, 1, cl);
					}
					free(cl);
				}
			}
		}
		if (mirr_rec != copy)
			free(copy);
		else
			mirr_rec = NULL, free(copy);
	}
	free(mirr.runs);

	/* Clusters: coalesce runs of consecutive dirty lcns. */
	for (i = 0; i < r->clu_cap; i++) {
		struct ovl_clu *c = &r->clu[i];
		uint64_t lcn;
		uint32_t n = 0, k;
		uint8_t *buf;
		struct ovl_clu *cc;

		if (!c->used || !c->dirty)
			continue;
		lcn = c->lcn;
		/* Walk back to the start of the dirty run. */
		while (lcn > 0 && (cc = ovl_clu_find(r, lcn - 1, false)) && cc->dirty)
			lcn--;
		while ((cc = ovl_clu_find(r, lcn + n, false)) && cc->dirty && n < 256)
			n++;
		buf = malloc((size_t)n * r->cs);
		if (!buf)
			return -ENOMEM;
		for (k = 0; k < n; k++) {
			cc = ovl_clu_find(r, lcn + k, false);
			memcpy(buf + (size_t)k * r->cs, cc->data, r->cs);
			cc->dirty = false;
		}
		err = r->ap->write_clusters(r->ap->ctx, lcn, n, buf);
		free(buf);
		if (err)
			return lfs_seterr(r->log, err < 0 ? err : -EIO, "write clusters %llu+%u failed",
					  (unsigned long long)lcn, n);
		r->res->clusters_written += n;
	}
	if (r->ap->sync)
		return r->ap->sync(r->ap->ctx);
	return 0;
}

/*
 * Heuristic H3: the overlay keeps MFT records and clusters separately. A
 * dirty cluster that lies inside $MFT:$DATA would race the record writes,
 * so such a plan is refused (NTFS logs MFT changes as record operations,
 * never as cluster writes to $MFT).
 */
static int check_overlay_aliasing(struct replay *r)
{
	uint32_t i, k;
	int err;

	for (i = 0; i < r->clu_cap; i++) {
		struct ovl_clu *c = &r->clu[i];
		if (!c->used || !c->dirty)
			continue;
		err = mft_runs_load(r);
		if (err)
			return err;
		for (k = 0; k < r->mftdata.nruns; k++) {
			const struct run *rl = &r->mftdata.runs[k];
			if (rl->lcn != SPARSE_LCN && c->lcn >= rl->lcn && c->lcn < rl->lcn + rl->len)
				return lfs_seterr(r->log, -EINVAL,
						  "cluster %llu written by replay lies inside $MFT",
						  (unsigned long long)c->lcn);
		}
	}
	return 0;
}

static void replay_free(struct replay *r)
{
	uint32_t i;

	free(r->mftdata.runs);
	free(r->mftdata.ovr);

	for (i = 0; i < r->nmft; i++)
		free(r->mft[i].rec);
	free(r->mft);
	for (i = 0; i < r->clu_cap; i++)
		if (r->clu[i].used)
			free(r->clu[i].data);
	free(r->clu);
	for (i = 0; i < r->noa; i++) {
		free(r->oa[i].runs);
		free(r->oa[i].ovr);
	}
	free(r->oa);
}

int ntfs_logfile_replay(ntfs_logfile_t *log, const struct ntfs_log_apply *apply, bool dry_run,
			struct ntfs_log_replay_result *result)
{
	struct replay r;
	uint64_t rlsn = 0;
	int err;

	memset(result, 0, sizeof(*result));
	if (!apply || !apply->read_mft_record || !apply->read_clusters ||
	    (!dry_run && (!apply->write_mft_record || !apply->write_clusters)))
		return -EINVAL;
	if (log->state == NTFS_LOG_EMPTY || log->state == NTFS_LOG_CLEAN)
		return 0;
	if (log->state == NTFS_LOG_CORRUPT || log->state == NTFS_LOG_UNSUPPORTED)
		return -EINVAL;
	if (log->state == NTFS_LOG_CHKDSK)
		return lfs_seterr(log, -EINVAL, "restart page written by chkdsk: let Windows finish");

	memset(&r, 0, sizeof(r));
	r.log = log;
	r.ap = apply;
	r.dry_run = dry_run;
	r.res = result;
	r.cs = log->geom.cluster_size;
	r.rs = log->geom.mft_record_size;
	log->plan_len = 0;

	err = lfs_tail_scan(log);
	if (err)
		goto out;
	err = lfs_load_checkpoint(log);
	if (err == -ENOENT) {
		err = 0;	/* no checkpoint: nothing was ever logged */
		goto out;
	}
	if (err)
		goto out;
	if (!log->checkpoint_lsn) {
		err = 0;
		goto out;
	}
	err = analysis_pass(&r, &rlsn);
	if (err)
		goto out;
	result->redo_lsn = rlsn;
	if (!lfs_table_total(&log->dptbl) && !lfs_table_total(&log->trtbl)) {
		lfs_msg(log, 2, "analysis: no dirty pages and no open transactions");
		err = 0;
		goto out;
	}
	/* Open attribute slots. */
	r.noa = log->oatbl.buf ? (log->oatbl.bytes - RT_HEADER_SIZE) / lfs_table_esize(&log->oatbl) : 0;
	r.oa = calloc(r.noa ? r.noa : 1, sizeof(*r.oa));
	if (!r.oa) {
		err = -ENOMEM;
		goto out;
	}
	err = prepare_dirty_pages(&r);
	if (err)
		goto out;
	err = redo_pass(&r, rlsn);
	if (err)
		goto out;
	err = undo_pass(&r);
	if (err)
		goto out;
	err = check_overlay_aliasing(&r);
	if (err)
		goto out;
	if (!dry_run) {
		err = flush_overlay(&r);
		if (err)
			goto out;
	}
	err = 0;
out:
	if (err == -EINVAL)
		result->needs_chkdsk = true;
	result->plan = log->plan;
	result->plan_len = log->plan_len;
	replay_free(&r);
	return err;
}
