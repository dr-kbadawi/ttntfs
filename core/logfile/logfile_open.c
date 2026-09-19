// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
/*
 * $LogFile restart page discovery, validation and geometry.
 *
 * Mirrors the consistency checks of Linux fs/ntfs/logfile.c (Altaparmakov)
 * which are the same checks Paragon's fslog.c performs, then derives the
 * LSN <-> file offset mapping used by the rest of the module.
 */
#include "logfile_internal.h"

void lfs_msg(ntfs_logfile_t *log, int level, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	if (!log->msg)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log->msg(level, buf, log->msg_ctx);
}

int lfs_seterr(ntfs_logfile_t *log, int err, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(log->errbuf, sizeof(log->errbuf), fmt, ap);
	va_end(ap);
	lfs_msg(log, 0, "%s", log->errbuf);
	return err;
}

const char *ntfs_logfile_last_error(const ntfs_logfile_t *log)
{
	return log->errbuf;
}

void ntfs_logfile_set_logger(ntfs_logfile_t *log, ntfs_log_msg_fn fn, void *ctx)
{
	log->msg = fn;
	log->msg_ctx = ctx;
}

static bool is_pow2(uint32_t v) { return v && !(v & (v - 1)); }

static uint32_t ilog2_u32(uint32_t v)
{
	uint32_t b = 0;
	while (v >>= 1)
		b++;
	return b;
}

/*
 * Validate a restart page header. Only the first 512 bytes are needed.
 * Returns 0, or -EINVAL with a message.
 */
static int check_rp_header(ntfs_logfile_t *log, const uint8_t *rp, uint32_t pos,
			   bool *have_usa)
{
	uint32_t sys_ps = lf_get32(rp + RP_SYSTEM_PAGE_SIZE);
	uint32_t log_ps = lf_get32(rp + RP_LOG_PAGE_SIZE);
	uint16_t major = lf_get16(rp + RP_MAJOR_VER);
	uint16_t minor = lf_get16(rp + RP_MINOR_VER);
	uint32_t magic = lf_get32(rp + RP_MAGIC);
	uint16_t usa_ofs, usa_count, ra_ofs;
	uint32_t usa_end = 0;

	*have_usa = true;
	if (sys_ps < LFS_SECTOR_SIZE || log_ps < LFS_SECTOR_SIZE ||
	    !is_pow2(sys_ps) || !is_pow2(log_ps))
		return lfs_seterr(log, -EINVAL, "restart page @%u: unsupported page sizes %u/%u",
				  pos, sys_ps, log_ps);
	if (pos && pos != sys_ps)
		return lfs_seterr(log, -EINVAL, "restart page at unexpected offset %u", pos);
	/* 1.0, 1.1 and 2.0 are the versions seen in the wild. */
	if (!((major == 1 && (minor == 0 || minor == 1)) || (major == 2 && minor == 0)))
		return lfs_seterr(log, -ENOTSUP, "LogFile version %u.%u not supported", major, minor);
	if (magic == LFS_MAGIC_CHKD && !lf_get16(rp + RP_USA_COUNT)) {
		*have_usa = false;
	} else {
		usa_count = 1 + (sys_ps >> LFS_SECTOR_SHIFT);
		if (usa_count != lf_get16(rp + RP_USA_COUNT))
			return lfs_seterr(log, -EINVAL, "restart page @%u: bad usa count", pos);
		usa_ofs = lf_get16(rp + RP_USA_OFS);
		usa_end = usa_ofs + usa_count * 2u;
		if (usa_ofs < RP_HEADER_SIZE || usa_end > LFS_SECTOR_SIZE - 2)
			return lfs_seterr(log, -EINVAL, "restart page @%u: bad usa offset", pos);
	}
	ra_ofs = lf_get16(rp + RP_RESTART_AREA_OFFSET);
	if ((ra_ofs & 7) || (*have_usa ? ra_ofs < usa_end : ra_ofs < RP_HEADER_SIZE) ||
	    ra_ofs > sys_ps)
		return lfs_seterr(log, -EINVAL, "restart page @%u: bad restart area offset", pos);
	if (magic != LFS_MAGIC_CHKD && lf_get64(rp + RP_CHKDSK_LSN))
		return lfs_seterr(log, -EINVAL, "restart page @%u: chkdsk lsn set on RSTR page", pos);
	return 0;
}

static int check_ra(ntfs_logfile_t *log, const uint8_t *rp, uint32_t pos)
{
	uint16_t ra_ofs = lf_get16(rp + RP_RESTART_AREA_OFFSET);
	const uint8_t *ra = rp + ra_ofs;
	uint32_t sys_ps = lf_get32(rp + RP_SYSTEM_PAGE_SIZE);
	uint16_t ca_ofs, ra_len_calc, ra_len, clients, fl, ul;
	uint64_t fs;
	uint32_t fs_bits = 0;

	if (ra_ofs + RA_FILE_SIZE > LFS_SECTOR_SIZE - 2)
		return lfs_seterr(log, -EINVAL, "restart area @%u: crosses first sector", pos);
	ca_ofs = lf_get16(ra + RA_CLIENT_ARRAY_OFFSET);
	if ((ca_ofs & 7) || ra_ofs + ca_ofs > LFS_SECTOR_SIZE - 2)
		return lfs_seterr(log, -EINVAL, "restart area @%u: bad client array offset", pos);
	clients = lf_get16(ra + RA_LOG_CLIENTS);
	ra_len = lf_get16(ra + RA_RESTART_AREA_LENGTH);
	ra_len_calc = ca_ofs + clients * CR_SIZE;
	if (ra_ofs + ra_len_calc > sys_ps || ra_ofs + ra_len > sys_ps || ra_len_calc > ra_len)
		return lfs_seterr(log, -EINVAL, "restart area @%u: length inconsistent", pos);
	fl = lf_get16(ra + RA_CLIENT_FREE_LIST);
	ul = lf_get16(ra + RA_CLIENT_IN_USE_LIST);
	if ((fl != LFS_NO_CLIENT && fl >= clients) || (ul != LFS_NO_CLIENT && ul >= clients))
		return lfs_seterr(log, -EINVAL, "restart area @%u: client lists overflow", pos);
	fs = lf_get64(ra + RA_FILE_SIZE);
	while (fs) {
		fs >>= 1;
		fs_bits++;
	}
	if (lf_get32(ra + RA_SEQ_NUMBER_BITS) != 67 - fs_bits)
		return lfs_seterr(log, -EINVAL, "restart area @%u: seq number bits %u != %u",
				  pos, lf_get32(ra + RA_SEQ_NUMBER_BITS), 67 - fs_bits);
	if (lf_get16(ra + RA_LOG_RECORD_HEADER_LENGTH) & 7)
		return lfs_seterr(log, -EINVAL, "restart area @%u: record header length unaligned", pos);
	if (lf_get16(ra + RA_LOG_PAGE_DATA_OFFSET) & 7)
		return lfs_seterr(log, -EINVAL, "restart area @%u: page data offset unaligned", pos);
	return 0;
}

/* Needs the whole deprotected page. */
static int check_client_array(ntfs_logfile_t *log, const uint8_t *rp, uint32_t pos)
{
	const uint8_t *ra = rp + lf_get16(rp + RP_RESTART_AREA_OFFSET);
	const uint8_t *ca = ra + lf_get16(ra + RA_CLIENT_ARRAY_OFFSET);
	uint16_t nr = lf_get16(ra + RA_LOG_CLIENTS);
	int list;

	for (list = 0; list < 2; list++) {
		uint16_t idx = lf_get16(ra + (list ? RA_CLIENT_IN_USE_LIST : RA_CLIENT_FREE_LIST));
		uint16_t budget = nr;
		bool first = true;

		while (idx != LFS_NO_CLIENT) {
			const uint8_t *cr;

			if (!budget || idx >= nr)
				return lfs_seterr(log, -EINVAL, "restart area @%u: client list corrupt", pos);
			budget--;
			cr = ca + (size_t)idx * CR_SIZE;
			if (first && lf_get16(cr + CR_PREV_CLIENT) != LFS_NO_CLIENT)
				return lfs_seterr(log, -EINVAL, "restart area @%u: first client has prev", pos);
			first = false;
			idx = lf_get16(cr + CR_NEXT_CLIENT);
		}
	}
	return 0;
}

/*
 * Load and validate the restart page at @pos. @head is its first 512 bytes.
 * On success fills log->rst[slot].
 */
static int load_restart_page(ntfs_logfile_t *log, uint32_t pos, const uint8_t *head, int slot)
{
	bool have_usa, torn = false;
	uint32_t sys_ps, magic;
	uint8_t *page;
	int err;

	err = check_rp_header(log, head, pos, &have_usa);
	if (err)
		return err;
	err = check_ra(log, head, pos);
	if (err)
		return err;
	sys_ps = lf_get32(head + RP_SYSTEM_PAGE_SIZE);
	magic = lf_get32(head + RP_MAGIC);
	if ((uint64_t)pos + sys_ps > log->orig_size)
		return lfs_seterr(log, -EINVAL, "restart page @%u exceeds file", pos);
	page = malloc(sys_ps);
	if (!page)
		return -ENOMEM;
	err = log->io.read(log->io.ctx, pos, page, sys_ps);
	if (err) {
		free(page);
		return lfs_seterr(log, err < 0 ? err : -EIO, "read restart page @%u failed", pos);
	}
	if (have_usa) {
		err = lfs_fixup_post_read(page, sys_ps, LFS_SECTOR_SIZE, &torn);
		if (err || torn) {
			/* Tolerable only if the whole restart area sits in sector 0. */
			const uint8_t *ra = page + lf_get16(page + RP_RESTART_AREA_OFFSET);
			uint32_t end = lf_get16(page + RP_RESTART_AREA_OFFSET) +
				       lf_get16(ra + RA_RESTART_AREA_LENGTH);
			if (err || end > LFS_SECTOR_SIZE - 2) {
				free(page);
				return lfs_seterr(log, -EINVAL, "restart page @%u: multi sector transfer error", pos);
			}
			lfs_msg(log, 1, "restart page @%u: usa mismatch outside restart area, tolerated", pos);
		}
	}
	if (magic == LFS_MAGIC_RSTR) {
		const uint8_t *ra = page + lf_get16(page + RP_RESTART_AREA_OFFSET);
		if (lf_get16(ra + RA_CLIENT_IN_USE_LIST) != LFS_NO_CLIENT) {
			err = check_client_array(log, page, pos);
			if (err) {
				free(page);
				return err;
			}
		}
	}
	log->rst[slot].present = true;
	log->rst[slot].valid = true;
	log->rst[slot].chkdsk = magic == LFS_MAGIC_CHKD;
	log->rst[slot].vbo = pos;
	log->rst[slot].page = page;
	if (magic == LFS_MAGIC_RSTR) {
		const uint8_t *ra = page + lf_get16(page + RP_RESTART_AREA_OFFSET);
		log->rst[slot].lsn = lf_get64(ra + RA_CURRENT_LSN);
	} else {
		log->rst[slot].lsn = lf_get64(page + RP_CHKDSK_LSN);
	}
	return 0;
}

/*
 * Search for the two restart pages the way the Linux driver does: probe
 * offsets 0, 256, 512, 1024, ... (the restart page must start a page for
 * some page size). Stop at the first RCRD page.
 */
static int find_restart_pages(ntfs_logfile_t *log)
{
	uint8_t head[LFS_SECTOR_SIZE];
	uint32_t pos;
	bool empty = true;
	int found = 0, err;

	for (pos = 0; pos < log->orig_size; pos = pos ? pos << 1 : LFS_SECTOR_SIZE) {
		uint32_t magic;

		if (pos + LFS_SECTOR_SIZE > log->orig_size)
			break;
		err = log->io.read(log->io.ctx, pos, head, sizeof(head));
		if (err)
			return lfs_seterr(log, err < 0 ? err : -EIO, "read @%u failed", pos);
		magic = lf_get32(head);
		if (magic != LFS_MAGIC_EMPTY)
			empty = false;
		else if (!empty)
			break;
		if (magic == LFS_MAGIC_RCRD)
			break;
		if (magic != LFS_MAGIC_RSTR && magic != LFS_MAGIC_CHKD)
			continue;
		err = load_restart_page(log, pos, head, found);
		if (!err) {
			found++;
			if (found == 2)
				break;
			continue;
		}
		if (err != -EINVAL && err != -ENOTSUP)
			return err;
		lfs_msg(log, 1, "restart page candidate @%u rejected: %s", pos, log->errbuf);
		if (err == -ENOTSUP)
			log->state = NTFS_LOG_UNSUPPORTED;
	}
	log->initialized = !empty;
	return found;
}

static int setup_geometry(ntfs_logfile_t *log)
{
	const uint8_t *rp = log->rst[log->cur_rst].page;
	const uint8_t *ra = rp + lf_get16(rp + RP_RESTART_AREA_OFFSET);
	const uint8_t *ca;
	uint16_t nr, idx, ca_ofs;
	uint64_t fsize;

	log->major_ver = lf_get16(rp + RP_MAJOR_VER);
	log->minor_ver = lf_get16(rp + RP_MINOR_VER);
	log->sys_page_size = lf_get32(rp + RP_SYSTEM_PAGE_SIZE);
	log->page_size = lf_get32(rp + RP_LOG_PAGE_SIZE);
	/*
	 * Every implementation we know of (Windows, fslog.c) has the log page
	 * size equal to the system page size and the page math below assumes
	 * it. Refuse anything else rather than guess.
	 */
	if (log->page_size != log->sys_page_size)
		return lfs_seterr(log, -ENOTSUP, "log page size %u != system page size %u unsupported",
				  log->page_size, log->sys_page_size);
	if (log->page_size > 64u * 1024)
		return lfs_seterr(log, -ENOTSUP, "log page size %u unsupported", log->page_size);
	log->page_bits = ilog2_u32(log->page_size);
	log->page_mask = log->page_size - 1;
	fsize = lf_get64(ra + RA_FILE_SIZE);
	if (fsize > log->orig_size)
		return lfs_seterr(log, -EINVAL, "restart area file size %llu > $LogFile size %u",
				  (unsigned long long)fsize, log->orig_size);
	log->l_size = (uint32_t)fsize & ~log->page_mask;
	if (log->l_size < (LFS_MIN_RECORD_PAGES + 2) * log->page_size)
		return lfs_seterr(log, -EINVAL, "LogFile too small (%u)", log->l_size);
	log->seq_num_bits = lf_get32(ra + RA_SEQ_NUMBER_BITS);
	log->file_data_bits = 64 - log->seq_num_bits;
	log->record_header_len = lf_get16(ra + RA_LOG_RECORD_HEADER_LENGTH);
	log->data_off = lf_get16(ra + RA_LOG_PAGE_DATA_OFFSET);
	log->open_log_count = lf_get32(ra + RA_RESTART_LOG_OPEN_COUNT);
	log->first_page = (log->major_ver >= 2 ? 0x22u : 4u) * log->page_size;
	log->clst_per_page = log->page_size / log->geom.cluster_size;
	if (!log->clst_per_page)
		log->clst_per_page = 1;
	if (log->record_header_len < LR_HEADER_SIZE || log->record_header_len >= log->page_size ||
	    log->data_off < PG_USA || log->data_off >= log->page_size)
		return lfs_seterr(log, -EINVAL, "restart area: bad record header/data offsets");
	log->current_lsn = lf_get64(ra + RA_CURRENT_LSN);
	log->last_lsn = log->current_lsn;
	log->seq_num = lfs_lsn_seq(log, log->current_lsn);
	log->single_page_io = lf_get16(ra + RA_FLAGS) & RESTART_SINGLE_PAGE_IO;

	/* Copy the restart area + client array. */
	log->ra_ofs = lf_get16(rp + RP_RESTART_AREA_OFFSET);
	log->ra_len = lf_get16(ra + RA_RESTART_AREA_LENGTH);
	log->ra = malloc(log->ra_len);
	if (!log->ra)
		return -ENOMEM;
	memcpy(log->ra, ra, log->ra_len);

	/* Find the NTFS client (the only one that exists in practice). */
	ca_ofs = lf_get16(ra + RA_CLIENT_ARRAY_OFFSET);
	ca = ra + ca_ofs;
	nr = lf_get16(ra + RA_LOG_CLIENTS);
	log->client_idx = LFS_NO_CLIENT;
	for (idx = lf_get16(ra + RA_CLIENT_IN_USE_LIST); idx != LFS_NO_CLIENT && idx < nr;
	     idx = lf_get16(ca + (size_t)idx * CR_SIZE + CR_NEXT_CLIENT)) {
		const uint8_t *cr = ca + (size_t)idx * CR_SIZE;
		if (lf_get32(cr + CR_CLIENT_NAME_LENGTH) == 8 &&
		    lf_get16(cr + CR_CLIENT_NAME + 0) == 'N' && lf_get16(cr + CR_CLIENT_NAME + 2) == 'T' &&
		    lf_get16(cr + CR_CLIENT_NAME + 4) == 'F' && lf_get16(cr + CR_CLIENT_NAME + 6) == 'S') {
			log->client_idx = idx;
			log->client_seq = lf_get16(cr + CR_SEQ_NUMBER);
			log->client_oldest_lsn = lf_get64(cr + CR_OLDEST_LSN);
			log->client_restart_lsn = lf_get64(cr + CR_CLIENT_RESTART_LSN);
			break;
		}
	}

	/* Oldest lsn: minimum over active clients, starting from last lsn. */
	log->oldest_lsn = log->current_lsn;
	for (idx = lf_get16(ra + RA_CLIENT_IN_USE_LIST); idx != LFS_NO_CLIENT && idx < nr;
	     idx = lf_get16(ca + (size_t)idx * CR_SIZE + CR_NEXT_CLIENT)) {
		uint64_t l = lf_get64(ca + (size_t)idx * CR_SIZE + CR_OLDEST_LSN);
		if (l && l < log->oldest_lsn)
			log->oldest_lsn = l;
	}
	if (lfs_lsn_to_vbo(log, log->oldest_lsn) < log->first_page)
		log->oldest_lsn = 0;	/* pseudo lsn: no oldest */

	/* Position of the next page after the recorded last lsn. */
	{
		uint32_t vbo = lfs_lsn_to_vbo(log, log->current_lsn);
		if (vbo < log->first_page) {
			log->no_last_lsn = true;
			log->next_page = log->first_page;
		} else {
			uint32_t end = lfs_final_log_off(log, log->current_lsn,
							 lf_get32(ra + RA_LAST_LSN_DATA_LENGTH));
			uint32_t tail;
			if (end <= vbo) {
				log->seq_num++;
				log->wrapped = true;
			}
			vbo &= ~log->page_mask;
			tail = log->page_size - (end & log->page_mask) - 1;
			if (tail >= log->record_header_len) {
				log->reuse_tail = true;
				log->next_page = vbo;
			} else {
				log->next_page = lfs_next_page_off(log, vbo);
			}
		}
	}
	return 0;
}

int ntfs_logfile_open(const struct ntfs_log_io *io, const struct ntfs_log_geometry *geom,
		      ntfs_logfile_t **out)
{
	ntfs_logfile_t *log;
	int found, err;

	*out = NULL;
	if (!io || !io->read || !geom || !geom->cluster_size || !geom->mft_record_size)
		return -EINVAL;
	log = calloc(1, sizeof(*log));
	if (!log)
		return -ENOMEM;
	log->io = *io;
	log->geom = *geom;
	log->cur_rst = -1;
	log->orig_size = (uint32_t)(io->size > LFS_MAX_FILE_SIZE ? LFS_MAX_FILE_SIZE : io->size);
	log->state = NTFS_LOG_CORRUPT;

	found = find_restart_pages(log);
	if (found < 0) {
		err = found;
		goto fail;
	}
	if (!log->initialized) {
		log->state = NTFS_LOG_EMPTY;
		*out = log;
		return 0;
	}
	if (!found) {
		if (log->state != NTFS_LOG_UNSUPPORTED)
			lfs_seterr(log, -EINVAL, "no valid restart page found in a non-empty LogFile");
		*out = log;
		return 0;
	}
	/* Prefer the restart page with the higher lsn. */
	log->cur_rst = 0;
	if (found == 2 && log->rst[1].lsn > log->rst[0].lsn)
		log->cur_rst = 1;
	/* If chkdsk rewrote page 0, page 1 is stale unless also CHKD. */
	if (found == 2 && log->rst[0].chkdsk && !log->rst[1].chkdsk)
		log->cur_rst = 0;

	err = setup_geometry(log);
	if (err) {
		log->state = err == -ENOTSUP ? NTFS_LOG_UNSUPPORTED : NTFS_LOG_CORRUPT;
		*out = log;
		return 0;
	}

	/*
	 * Same decision as the Linux driver (ntfs_is_logfile_clean): the log
	 * is clean when it is closed (no client in use) or open with the
	 * RESTART_VOLUME_IS_CLEAN flag, which XP+ sets at dismount and clears
	 * at mount. A page rewritten by chkdsk ("CHKD") is judged by the same
	 * rule; it is normally clean. An open CHKD page without the flag means
	 * chkdsk was interrupted: report it as NTFS_LOG_CHKDSK so nobody
	 * replays on top of a half-repaired volume.
	 */
	{
		const uint8_t *ra = log->ra;
		bool open = lf_get16(ra + RA_CLIENT_IN_USE_LIST) != LFS_NO_CLIENT;
		bool flag = lf_get16(ra + RA_FLAGS) & RESTART_VOLUME_IS_CLEAN;
		if (!open || flag)
			log->state = NTFS_LOG_CLEAN;
		else if (log->rst[log->cur_rst].chkdsk)
			log->state = NTFS_LOG_CHKDSK;
		else
			log->state = NTFS_LOG_DIRTY;
	}
	*out = log;
	return 0;
fail:
	ntfs_logfile_close(log);
	return err;
}

void ntfs_logfile_close(ntfs_logfile_t *log)
{
	int i;

	if (!log)
		return;
	for (i = 0; i < 2; i++)
		free(log->rst[i].page);
	for (i = 0; i < log->n_ovr; i++)
		free(log->ovr[i].page);
	free(log->ra);
	free(log->crst);
	lfs_table_free(&log->trtbl);
	lfs_table_free(&log->dptbl);
	lfs_table_free(&log->oatbl);
	free(log->attr_names);
	free(log->plan);
	free(log);
}

bool ntfs_logfile_is_clean(const ntfs_logfile_t *log)
{
	return log->state == NTFS_LOG_EMPTY || log->state == NTFS_LOG_CLEAN;
}

void ntfs_logfile_get_info(const ntfs_logfile_t *log, struct ntfs_logfile_info *info)
{
	memset(info, 0, sizeof(*info));
	info->state = log->state;
	info->major_ver = log->major_ver;
	info->minor_ver = log->minor_ver;
	info->system_page_size = log->sys_page_size;
	info->log_page_size = log->page_size;
	info->file_size = log->l_size;
	info->current_lsn = log->current_lsn;
	info->last_lsn = log->last_lsn;
	info->oldest_lsn = log->oldest_lsn;
	info->client_restart_lsn = log->client_restart_lsn;
	info->checkpoint_lsn = log->checkpoint_lsn;
	info->seq_number_bits = log->seq_num_bits;
	info->open_log_count = log->open_log_count;
	if (log->ra) {
		info->restart_area_flags = lf_get16(log->ra + RA_FLAGS);
		info->client_in_use = lf_get16(log->ra + RA_CLIENT_IN_USE_LIST);
		info->client_free = lf_get16(log->ra + RA_CLIENT_FREE_LIST);
		info->volume_is_clean_flag = info->restart_area_flags & RESTART_VOLUME_IS_CLEAN;
	}
	info->restart_page_used[0] = log->cur_rst == 0;
	info->restart_page_used[1] = log->cur_rst == 1;
	info->table_transactions = lfs_table_total(&log->trtbl);
	info->table_dirty_pages = lfs_table_total(&log->dptbl);
	info->table_open_attrs = lfs_table_total(&log->oatbl);
	info->tail_scanned = log->tail_scanned;
	info->tail_overrides = (uint32_t)log->n_ovr;
	info->next_page = log->next_page;
	info->reuse_tail = log->reuse_tail;
	info->client_restart_version = log->crst_major;
	info->attr_names_bytes = log->attr_names_len;
}

static const char *const op_names[LOP__MAX] = {
	[LOP_Noop] = "Noop",
	[LOP_CompensationLogRecord] = "CompensationLogRecord",
	[LOP_InitializeFileRecordSegment] = "InitializeFileRecordSegment",
	[LOP_DeallocateFileRecordSegment] = "DeallocateFileRecordSegment",
	[LOP_WriteEndOfFileRecordSegment] = "WriteEndOfFileRecordSegment",
	[LOP_CreateAttribute] = "CreateAttribute",
	[LOP_DeleteAttribute] = "DeleteAttribute",
	[LOP_UpdateResidentValue] = "UpdateResidentValue",
	[LOP_UpdateNonresidentValue] = "UpdateNonresidentValue",
	[LOP_UpdateMappingPairs] = "UpdateMappingPairs",
	[LOP_DeleteDirtyClusters] = "DeleteDirtyClusters",
	[LOP_SetNewAttributeSizes] = "SetNewAttributeSizes",
	[LOP_AddIndexEntryRoot] = "AddIndexEntryRoot",
	[LOP_DeleteIndexEntryRoot] = "DeleteIndexEntryRoot",
	[LOP_AddIndexEntryAllocation] = "AddIndexEntryAllocation",
	[LOP_DeleteIndexEntryAllocation] = "DeleteIndexEntryAllocation",
	[LOP_WriteEndOfIndexBuffer] = "WriteEndOfIndexBuffer",
	[LOP_SetIndexEntryVcnRoot] = "SetIndexEntryVcnRoot",
	[LOP_SetIndexEntryVcnAllocation] = "SetIndexEntryVcnAllocation",
	[LOP_UpdateFileNameRoot] = "UpdateFileNameRoot",
	[LOP_UpdateFileNameAllocation] = "UpdateFileNameAllocation",
	[LOP_SetBitsInNonresidentBitMap] = "SetBitsInNonresidentBitMap",
	[LOP_ClearBitsInNonresidentBitMap] = "ClearBitsInNonresidentBitMap",
	[LOP_HotFix] = "HotFix",
	[LOP_EndTopLevelAction] = "EndTopLevelAction",
	[LOP_PrepareTransaction] = "PrepareTransaction",
	[LOP_CommitTransaction] = "CommitTransaction",
	[LOP_ForgetTransaction] = "ForgetTransaction",
	[LOP_OpenNonresidentAttribute] = "OpenNonresidentAttribute",
	[LOP_OpenAttributeTableDump] = "OpenAttributeTableDump",
	[LOP_AttributeNamesDump] = "AttributeNamesDump",
	[LOP_DirtyPageTableDump] = "DirtyPageTableDump",
	[LOP_TransactionTableDump] = "TransactionTableDump",
	[LOP_UpdateRecordDataRoot] = "UpdateRecordDataRoot",
	[LOP_UpdateRecordDataAllocation] = "UpdateRecordDataAllocation",
	[LOP_UpdateRelativeDataInIndex] = "UpdateRelativeDataInIndex",
	[LOP_UpdateRelativeDataInIndex2] = "UpdateRelativeDataInIndex2",
	[LOP_ZeroEndOfFileRecord] = "ZeroEndOfFileRecord",
};

const char *ntfs_log_op_name(unsigned op)
{
	if (op < LOP__MAX && op_names[op])
		return op_names[op];
	return "Unknown";
}

/*
 * Write both restart pages closed and clean. Layout follows what Windows
 * XP+ writes (client array at 0x40) and what ntfs3 writes after replay.
 */
int ntfs_logfile_mark_clean(ntfs_logfile_t *log)
{
	uint8_t *page;
	uint32_t ps, usa_count, ra_ofs, ra_len, ca_ofs = RA_SIZE_XP;
	uint8_t *ra, *cr;
	int err, i;

	if (!log->io.write)
		return -ENOTSUP;
	/*
	 * CORRUPT here means "no usable restart page", and regenerating the two
	 * restart pages is exactly the right response to that -- it is what
	 * Windows does. Measured on a real Windows Recovery volume whose restart
	 * pages had been 0xff since 2022: attaching it to Windows wrote two fresh
	 * v1.1 restart pages, left all 1113 record pages in place, and mounted it
	 * read-write. It did not erase the log and did not repair the volume (a
	 * pre-existing $FILE_NAME/$DATA size mismatch on inode 41 survived
	 * untouched, which is a separate problem needing chkdsk, not a journal).
	 *
	 * UNSUPPORTED still refuses: that is a version we do not understand, and
	 * writing a v1.1 header over it would be a guess.
	 */
	if (log->state == NTFS_LOG_UNSUPPORTED)
		return -EINVAL;
	ps = log->page_size ? log->page_size : LFS_DEFAULT_PAGE_SIZE;
	page = calloc(1, ps);
	if (!page)
		return -ENOMEM;
	usa_count = (ps >> LFS_SECTOR_SHIFT) + 1;
	ra_ofs = LF_ALIGN8(RP_HEADER_SIZE + usa_count * 2);
	ra_len = ca_ofs + CR_SIZE;

	lf_put32(page + RP_MAGIC, LFS_MAGIC_RSTR);
	lf_put16(page + RP_USA_OFS, RP_HEADER_SIZE);
	lf_put16(page + RP_USA_COUNT, (uint16_t)usa_count);
	lf_put32(page + RP_SYSTEM_PAGE_SIZE, ps);
	lf_put32(page + RP_LOG_PAGE_SIZE, ps);
	lf_put16(page + RP_RESTART_AREA_OFFSET, (uint16_t)ra_ofs);
	lf_put16(page + RP_MINOR_VER, 1);
	lf_put16(page + RP_MAJOR_VER, 1);

	ra = page + ra_ofs;
	if (log->ra) {
		/* Keep the existing area's values where they matter. */
		lf_put64(ra + RA_CURRENT_LSN, log->last_lsn);
		lf_put32(ra + RA_SEQ_NUMBER_BITS, log->seq_num_bits);
		lf_put64(ra + RA_FILE_SIZE, log->l_size);
		lf_put16(ra + RA_LOG_RECORD_HEADER_LENGTH, (uint16_t)log->record_header_len);
		lf_put16(ra + RA_LOG_PAGE_DATA_OFFSET, (uint16_t)log->data_off);
		lf_put32(ra + RA_RESTART_LOG_OPEN_COUNT, log->open_log_count + 1);
	} else {
		/* Prefer the log's real size; fall back to a minimal one only when
		 * it is unknown. A file_size smaller than the attribute would make
		 * every later reader wrap early. */
		uint32_t bits = 0, v = log->orig_size >= ps * (LFS_MIN_RECORD_PAGES + 2)
					? log->orig_size : ps * (LFS_MIN_RECORD_PAGES + 2);
		for (uint32_t t = v; t; t >>= 1)
			bits++;
		lf_put32(ra + RA_SEQ_NUMBER_BITS, 67 - bits);
		lf_put64(ra + RA_FILE_SIZE, v);
		lf_put16(ra + RA_LOG_RECORD_HEADER_LENGTH, LR_HEADER_SIZE);
		lf_put16(ra + RA_LOG_PAGE_DATA_OFFSET, (uint16_t)LF_ALIGN8(PG_USA + usa_count * 2));
	}
	lf_put16(ra + RA_LOG_CLIENTS, 1);
	lf_put16(ra + RA_CLIENT_FREE_LIST, 0);
	lf_put16(ra + RA_CLIENT_IN_USE_LIST, LFS_NO_CLIENT);
	lf_put16(ra + RA_FLAGS, RESTART_VOLUME_IS_CLEAN);
	lf_put16(ra + RA_RESTART_AREA_LENGTH, (uint16_t)ra_len);
	lf_put16(ra + RA_CLIENT_ARRAY_OFFSET, (uint16_t)ca_ofs);
	lf_put32(ra + RA_LAST_LSN_DATA_LENGTH, 0);

	cr = ra + ca_ofs;
	lf_put64(cr + CR_OLDEST_LSN, 0);
	lf_put64(cr + CR_CLIENT_RESTART_LSN, 0);
	lf_put16(cr + CR_PREV_CLIENT, LFS_NO_CLIENT);
	lf_put16(cr + CR_NEXT_CLIENT, LFS_NO_CLIENT);
	lf_put16(cr + CR_SEQ_NUMBER, 1);
	lf_put32(cr + CR_CLIENT_NAME_LENGTH, 8);
	lf_put16(cr + CR_CLIENT_NAME + 0, 'N');
	lf_put16(cr + CR_CLIENT_NAME + 2, 'T');
	lf_put16(cr + CR_CLIENT_NAME + 4, 'F');
	lf_put16(cr + CR_CLIENT_NAME + 6, 'S');

	/* Tail copies that superseded on-disk pages go back first. */
	for (i = 0; i < log->n_ovr; i++) {
		uint8_t *copy = malloc(log->page_size);
		if (!copy) {
			err = -ENOMEM;
			goto out;
		}
		memcpy(copy, log->ovr[i].page, log->page_size);
		lf_put16(copy + PG_PAGE_COUNT, 1);
		lf_put16(copy + PG_PAGE_POSITION, 1);
		if (log->major_ver < 2)
			lf_put64(copy + PG_LAST_LSN, lf_get64(copy + PG_LAST_END_LSN));
		else
			lf_put32(copy + PG_FILE_OFFSET, 0);
		lfs_fixup_pre_write(copy, log->page_size, LFS_SECTOR_SIZE);
		err = log->io.write(log->io.ctx, log->ovr[i].vbo, copy, log->page_size);
		free(copy);
		if (err)
			goto out;
	}

	for (i = 0; i < 2; i++) {
		uint8_t *copy = malloc(ps);
		if (!copy) {
			err = -ENOMEM;
			goto out;
		}
		memcpy(copy, page, ps);
		/* Fresh usa value distinct per page. */
		lf_put16(copy + RP_HEADER_SIZE, (uint16_t)(0x1000 + i + (log->open_log_count & 0xff)));
		lfs_fixup_pre_write(copy, ps, LFS_SECTOR_SIZE);
		err = log->io.write(log->io.ctx, (uint64_t)i * ps, copy, ps);
		free(copy);
		if (err)
			goto out;
	}
	log->state = NTFS_LOG_CLEAN;
	err = 0;
out:
	free(page);
	return err;
}
