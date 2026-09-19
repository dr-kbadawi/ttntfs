// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
/*
 * ntfslog - standalone $LogFile analyzer.
 *
 *   ntfslog [options] <ntfs-image | raw-$LogFile-dump>
 *
 * Prints the restart pages, the clean/dirty decision, the checkpoint and
 * its tables, the record chain from the checkpoint with decoded operations,
 * and the dry-run replay plan. With --apply (writable image) the plan is
 * applied and the log is marked clean.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include "ntfs_image.h"
#include "ntfs_logfile.h"
#include "logfile_layout.h"

static int verbose;
static int hexdump;

static void msg(int level, const char *m, void *ctx)
{
	static const char *const lv[] = { "error", "warn", "info", "debug" };
	if (level <= 1 || verbose)
		fprintf(stderr, "  [%s] %s\n", lv[level & 3], m);
}

static const char *state_name(int s)
{
	switch (s) {
	case NTFS_LOG_EMPTY: return "EMPTY (never written: clean)";
	case NTFS_LOG_CLEAN: return "CLEAN";
	case NTFS_LOG_DIRTY: return "DIRTY (replay required)";
	case NTFS_LOG_CHKDSK: return "CHKDSK (restart page written by an interrupted chkdsk)";
	case NTFS_LOG_CORRUPT: return "CORRUPT (no usable restart page)";
	case NTFS_LOG_UNSUPPORTED: return "UNSUPPORTED";
	default: return "?";
	}
}

static void dump_hex(const uint8_t *p, uint32_t n, const char *indent)
{
	uint32_t i;
	for (i = 0; i < n; i += 16) {
		uint32_t j;
		printf("%s%04x:", indent, i);
		for (j = i; j < i + 16 && j < n; j++)
			printf(" %02x", p[j]);
		printf("\n");
	}
}

/* Raw restart page summary, independent of the module's validation. */
static void print_restart_page(struct ntfs_log_io *io, int idx, uint64_t off, bool current)
{
	uint8_t rp[512];
	uint32_t magic;
	uint16_t ra_ofs;
	const uint8_t *ra, *ca;
	uint16_t inuse, freel, nclients;

	if (io->read(io->ctx, off, rp, sizeof(rp))) {
		printf("Restart page %d @0x%" PRIx64 ": unreadable\n", idx, off);
		return;
	}
	magic = lf_get32(rp);
	if (magic == LFS_MAGIC_EMPTY) {
		printf("Restart page %d @0x%" PRIx64 ": empty (0xff)\n", idx, off);
		return;
	}
	if (magic != LFS_MAGIC_RSTR && magic != LFS_MAGIC_CHKD) {
		printf("Restart page %d @0x%" PRIx64 ": magic %.4s (not a restart page)\n", idx, off, (const char *)rp);
		return;
	}
	printf("Restart page %d @0x%" PRIx64 ": %.4s v%u.%u system_page %u log_page %u usa %u/%u%s\n",
	       idx, off, (const char *)rp, lf_get16(rp + RP_MAJOR_VER), lf_get16(rp + RP_MINOR_VER),
	       lf_get32(rp + RP_SYSTEM_PAGE_SIZE), lf_get32(rp + RP_LOG_PAGE_SIZE),
	       lf_get16(rp + RP_USA_OFS), lf_get16(rp + RP_USA_COUNT), current ? "  [current]" : "");
	if (magic == LFS_MAGIC_CHKD)
		printf("  chkdsk_lsn 0x%" PRIx64 "\n", lf_get64(rp + RP_CHKDSK_LSN));
	ra_ofs = lf_get16(rp + RP_RESTART_AREA_OFFSET);
	if (ra_ofs + RA_SIZE_V1 > sizeof(rp)) {
		printf("  restart area offset 0x%x beyond first sector\n", ra_ofs);
		return;
	}
	ra = rp + ra_ofs;
	inuse = lf_get16(ra + RA_CLIENT_IN_USE_LIST);
	freel = lf_get16(ra + RA_CLIENT_FREE_LIST);
	nclients = lf_get16(ra + RA_LOG_CLIENTS);
	printf("  restart area @0x%x: current_lsn 0x%" PRIx64 " clients %u in_use %s free %s flags 0x%04x%s%s\n",
	       ra_ofs, lf_get64(ra + RA_CURRENT_LSN), nclients,
	       inuse == LFS_NO_CLIENT ? "none" : "0", freel == LFS_NO_CLIENT ? "none" : "0",
	       lf_get16(ra + RA_FLAGS),
	       lf_get16(ra + RA_FLAGS) & RESTART_VOLUME_IS_CLEAN ? " VOLUME_IS_CLEAN" : "",
	       lf_get16(ra + RA_FLAGS) & RESTART_SINGLE_PAGE_IO ? " SINGLE_PAGE_IO" : "");
	printf("  seq_number_bits %u file_size 0x%" PRIx64 " last_lsn_data_len %u rec_hdr_len %u data_off 0x%x open_count %u ra_len 0x%x client_array 0x%x\n",
	       lf_get32(ra + RA_SEQ_NUMBER_BITS), lf_get64(ra + RA_FILE_SIZE),
	       lf_get32(ra + RA_LAST_LSN_DATA_LENGTH), lf_get16(ra + RA_LOG_RECORD_HEADER_LENGTH),
	       lf_get16(ra + RA_LOG_PAGE_DATA_OFFSET), lf_get32(ra + RA_RESTART_LOG_OPEN_COUNT),
	       lf_get16(ra + RA_RESTART_AREA_LENGTH), lf_get16(ra + RA_CLIENT_ARRAY_OFFSET));
	ca = ra + lf_get16(ra + RA_CLIENT_ARRAY_OFFSET);
	if (nclients && (size_t)(ca - rp) + CR_SIZE <= sizeof(rp) - 2) {
		char name[65];
		uint32_t nl = lf_get32(ca + CR_CLIENT_NAME_LENGTH) / 2, i;
		if (nl > 64)
			nl = 64;
		for (i = 0; i < nl; i++) {
			uint16_t c = lf_get16(ca + CR_CLIENT_NAME + 2 * i);
			name[i] = c < 0x80 ? (char)c : '?';
		}
		name[nl] = 0;
		printf("  client 0 \"%s\": oldest_lsn 0x%" PRIx64 " client_restart_lsn 0x%" PRIx64 " seq %u prev %04x next %04x\n",
		       name, lf_get64(ca + CR_OLDEST_LSN), lf_get64(ca + CR_CLIENT_RESTART_LSN),
		       lf_get16(ca + CR_SEQ_NUMBER), lf_get16(ca + CR_PREV_CLIENT), lf_get16(ca + CR_NEXT_CLIENT));
	}
}

/* ---- tables ------------------------------------------------------------ */

static const char *tr_state(uint8_t s)
{
	switch (s) {
	case TRANSACTION_ACTIVE: return "active";
	case TRANSACTION_PREPARED: return "prepared";
	case TRANSACTION_COMMITTED: return "committed";
	default: return "uninit";
	}
}

static int cb_tr(const struct ntfs_log_transaction *t, void *ctx)
{
	printf("    tid 0x%x: %s first 0x%" PRIx64 " prev 0x%" PRIx64 " undo_next 0x%" PRIx64 " undo %u recs/%u bytes\n",
	       t->id, tr_state(t->state), t->first_lsn, t->prev_lsn, t->undo_next_lsn, t->undo_records, t->undo_bytes);
	return 0;
}

static int cb_dp(const struct ntfs_log_dirty_page *d, void *ctx)
{
	uint32_t i;
	printf("    dp 0x%x: attr 0x%x vcn %" PRIu64 " lcns %u transfer %u oldest_lsn 0x%" PRIx64 " lcn[",
	       d->id, d->target_attr, d->vcn, d->lcns_follow, d->transfer_len, d->oldest_lsn);
	for (i = 0; i < d->lcns_follow && i < 16; i++)
		printf("%s%" PRIu64, i ? "," : "", lf_get64(&d->page_lcns[i]));
	printf("%s]\n", d->lcns_follow > 16 ? ",..." : "");
	return 0;
}

static const char *attr_type_name(uint32_t t)
{
	switch (t) {
	case 0x10: return "$STANDARD_INFORMATION";
	case 0x20: return "$ATTRIBUTE_LIST";
	case 0x30: return "$FILE_NAME";
	case 0x40: return "$OBJECT_ID";
	case 0x50: return "$SECURITY_DESCRIPTOR";
	case 0x60: return "$VOLUME_NAME";
	case 0x70: return "$VOLUME_INFORMATION";
	case 0x80: return "$DATA";
	case 0x90: return "$INDEX_ROOT";
	case 0xa0: return "$INDEX_ALLOCATION";
	case 0xb0: return "$BITMAP";
	case 0xc0: return "$REPARSE_POINT";
	case 0xd0: return "$EA_INFORMATION";
	case 0xe0: return "$EA";
	case 0x100: return "$LOGGED_UTILITY_STREAM";
	default: return "?";
	}
}

static int cb_oa(const struct ntfs_log_open_attr *a, void *ctx)
{
	char name[64];
	uint32_t i, n = a->name_len < 63 ? a->name_len : 63;
	for (i = 0; i < n; i++) {
		uint16_t c = lf_get16(&a->name[i]);
		name[i] = c < 0x80 ? (char)c : '?';
	}
	name[n] = 0;
	printf("    oa 0x%x: mft %" PRIu64 " (seq %u) type 0x%x %s%s%s bytes_per_index %u open_lsn 0x%" PRIx64 " dirty %u\n",
	       a->id, a->mft_no, a->mft_seq, a->type, attr_type_name(a->type),
	       n ? " name " : "", name, a->bytes_per_index, a->open_record_lsn, a->dirty_pages);
	return 0;
}

static void print_tables(ntfs_logfile_t *log, const char *title)
{
	struct ntfs_log_table_cbs cbs = { cb_tr, cb_dp, cb_oa };
	struct ntfs_logfile_info info;

	ntfs_logfile_get_info(log, &info);
	printf("%s: %u transaction(s), %u dirty page(s), %u open attribute(s)\n", title,
	       info.table_transactions, info.table_dirty_pages, info.table_open_attrs);
	ntfs_logfile_tables(log, &cbs, NULL);
}

/* ---- records ----------------------------------------------------------- */

struct walk_ctx {
	unsigned count;
	unsigned max;
};

static int cb_rec(const struct ntfs_log_record *r, void *ctx)
{
	struct walk_ctx *w = ctx;

	w->count++;
	if (w->max && w->count > w->max)
		return 1;
	if (r->record_type == LFS_RECORD_TYPE_CLIENT_RESTART) {
		printf("  lsn 0x%" PRIx64 ": CLIENT_RESTART (%u bytes) prev 0x%" PRIx64 "\n", r->lsn, r->data_len,
		       r->client_prev_lsn);
		if (r->data_len >= CRST_SIZE) {
			const uint8_t *c = r->raw;
			printf("      restart v%u.%u start_of_checkpoint 0x%" PRIx64 " oatbl lsn 0x%" PRIx64 "/%u names 0x%" PRIx64 "/%u dptbl 0x%" PRIx64 "/%u trtbl 0x%" PRIx64 "/%u\n",
			       lf_get32(c + CRST_MAJOR_VER), lf_get32(c + CRST_MINOR_VER),
			       lf_get64(c + CRST_START_OF_CHECKPOINT),
			       lf_get64(c + CRST_OPEN_ATTR_TABLE_LSN), lf_get32(c + CRST_OPEN_ATTR_TABLE_LEN),
			       lf_get64(c + CRST_ATTR_NAMES_LSN), lf_get32(c + CRST_ATTR_NAMES_LEN),
			       lf_get64(c + CRST_DIRTY_PAGE_TABLE_LSN), lf_get32(c + CRST_DIRTY_PAGE_TABLE_LEN),
			       lf_get64(c + CRST_TRANSACTION_TABLE_LSN), lf_get32(c + CRST_TRANSACTION_TABLE_LEN));
		}
		return 0;
	}
	if (r->record_type != LFS_RECORD_TYPE_CLIENT) {
		printf("  lsn 0x%" PRIx64 ": record type %u (%u bytes)\n", r->lsn, r->record_type, r->data_len);
		return 0;
	}
	printf("  lsn 0x%" PRIx64 " tid 0x%x%s: redo %s(0x%x)/%u undo %s(0x%x)/%u attr 0x%x vcn %" PRIu64 " lcns %u rec_off 0x%x attr_off 0x%x clu_off %u prev 0x%" PRIx64 " undo_next 0x%" PRIx64 "\n",
	       r->lsn, r->transaction_id, r->flags & LFS_RECORD_MULTI_PAGE ? " [multi-page]" : "",
	       ntfs_log_op_name(r->redo_op), r->redo_op, r->redo_len,
	       ntfs_log_op_name(r->undo_op), r->undo_op, r->undo_len,
	       r->target_attr, r->target_vcn, r->lcns_follow, r->record_off, r->attr_off, r->cluster_off,
	       r->client_prev_lsn, r->client_undo_next_lsn);
	if (r->lcns_follow) {
		uint32_t i;
		printf("      page lcns:");
		for (i = 0; i < r->lcns_follow && i < 16; i++)
			printf(" %" PRIu64, lf_get64(&r->page_lcns[i]));
		printf("\n");
	}
	if (hexdump) {
		if (r->redo && r->redo_len) {
			printf("      redo data:\n");
			dump_hex(r->redo, r->redo_len, "        ");
		}
		if (r->undo && r->undo_len) {
			printf("      undo data:\n");
			dump_hex(r->undo, r->undo_len, "        ");
		}
	}
	return 0;
}

static void print_plan(const struct ntfs_log_replay_result *res)
{
	uint32_t i;

	printf("Replay result: analyzed %u, redone %u, undone %u, transactions committed %u / active(rolled back) %u, dirty pages %u, redo_lsn 0x%" PRIx64 "%s\n",
	       res->records_analyzed, res->records_redone, res->records_undone,
	       res->transactions_committed, res->transactions_active, res->dirty_pages, res->redo_lsn,
	       res->needs_chkdsk ? "  NEEDS CHKDSK" : "");
	printf("Plan (%u entries):\n", res->plan_len);
	for (i = 0; i < res->plan_len; i++) {
		const struct ntfs_log_plan_entry *e = &res->plan[i];
		switch (e->kind) {
		case NTFS_PLAN_MFT_RECORD:
			printf("  %s lsn 0x%" PRIx64 " %s: write MFT record %" PRIu64 " (attr 0x%x vcn %" PRIu64 ")\n",
			       e->undo ? "UNDO" : "REDO", e->lsn, ntfs_log_op_name(e->op), e->mft_no, e->target_attr, e->target_vcn);
			break;
		case NTFS_PLAN_CLUSTERS:
			printf("  %s lsn 0x%" PRIx64 " %s: write %u cluster(s) at lcn %" PRIu64 " (attr 0x%x vcn %" PRIu64 ")\n",
			       e->undo ? "UNDO" : "REDO", e->lsn, ntfs_log_op_name(e->op), e->count, e->lcn, e->target_attr, e->target_vcn);
			break;
		default:
			printf("  %s lsn 0x%" PRIx64 " %s: skipped (%s)\n", e->undo ? "UNDO" : "REDO", e->lsn,
			       ntfs_log_op_name(e->op), e->why);
		}
	}
}

static void usage(void)
{
	fprintf(stderr,
		"usage: ntfslog [-v] [-x] [-n MAX] [--no-plan] [--apply] [-c CLUSTER] [-r RECSIZE] <image|logfile-dump>\n"
		"  -v         module debug messages\n"
		"  -x         hex dump redo/undo data\n"
		"  -n MAX     print at most MAX records\n"
		"  --no-plan  skip the dry-run replay\n"
		"  --apply    apply the plan and mark the log clean (writes to the image!)\n"
		"  -c/-r      cluster / MFT record size for raw $LogFile dumps (default 4096/1024)\n");
}

int main(int argc, char **argv)
{
	struct ntfs_image img;
	struct ntfs_log_io io;
	struct ntfs_log_geometry geom;
	struct ntfs_log_apply ap;
	ntfs_logfile_t *log = NULL;
	struct ntfs_logfile_info info;
	struct ntfs_log_replay_result res;
	struct walk_ctx wc = { 0, 0 };
	const char *path = NULL;
	bool do_plan = true, do_apply = false;
	uint32_t cs = 0, rs = 0;
	int i, err, rc = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) verbose = 1;
		else if (!strcmp(argv[i], "-x")) hexdump = 1;
		else if (!strcmp(argv[i], "-n") && i + 1 < argc) wc.max = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--no-plan")) do_plan = false;
		else if (!strcmp(argv[i], "--apply")) do_apply = true;
		else if (!strcmp(argv[i], "-c") && i + 1 < argc) cs = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-r") && i + 1 < argc) rs = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (argv[i][0] == '-') { usage(); return 2; }
		else path = argv[i];
	}
	if (!path) {
		usage();
		return 2;
	}
	err = ntfs_image_open(&img, path, do_apply, cs, rs);
	if (err) {
		fprintf(stderr, "%s: %s (%s)\n", path, img.err, strerror(-err));
		return 1;
	}
	ntfs_image_log_io(&img, &io);
	ntfs_image_geometry(&img, &geom);
	ntfs_image_apply(&img, &ap);
	if (img.raw)
		printf("%s: raw $LogFile dump, %" PRIu64 " bytes (assuming cluster %u, MFT record %u)\n",
		       path, img.log_size, img.cluster_size, img.mft_record_size);
	else
		printf("%s: NTFS volume, sector %u cluster %u MFT record %u, $LogFile %" PRIu64 " bytes in %u run(s)\n",
		       path, img.sector_size, img.cluster_size, img.mft_record_size, img.log_size, img.n_log_runs);

	err = ntfs_logfile_open(&io, &geom, &log);
	if (err) {
		fprintf(stderr, "ntfs_logfile_open: %s\n", strerror(-err));
		rc = 1;
		goto out;
	}
	ntfs_logfile_set_logger(log, msg, NULL);
	ntfs_logfile_get_info(log, &info);

	print_restart_page(&io, 0, 0, info.restart_page_used[0]);
	print_restart_page(&io, 1, info.system_page_size ? info.system_page_size : 4096, info.restart_page_used[1]);
	printf("State: %s%s\n", state_name(info.state),
	       info.state == NTFS_LOG_CORRUPT || info.state == NTFS_LOG_UNSUPPORTED ? "" : "");
	if (info.state == NTFS_LOG_CORRUPT || info.state == NTFS_LOG_UNSUPPORTED)
		printf("  %s\n", ntfs_logfile_last_error(log));
	printf("Clean for read-write mount: %s\n", ntfs_logfile_is_clean(log) ? "yes" : "no");
	if (info.state == NTFS_LOG_EMPTY || info.state == NTFS_LOG_CORRUPT || info.state == NTFS_LOG_UNSUPPORTED)
		goto out;

	printf("Geometry: v%u.%u page %u usable size 0x%" PRIx64 " seq_number_bits %u first record page 0x%x open_count %u\n",
	       info.major_ver, info.minor_ver, info.log_page_size, info.file_size, info.seq_number_bits,
	       (info.major_ver >= 2 ? 0x22u : 4u) * info.log_page_size, info.open_log_count);
	printf("Restart area: current_lsn 0x%" PRIx64 "; client NTFS oldest_lsn 0x%" PRIx64 " restart_lsn 0x%" PRIx64 "\n",
	       info.current_lsn, info.oldest_lsn, info.client_restart_lsn);

	err = ntfs_logfile_load_checkpoint(log);
	ntfs_logfile_get_info(log, &info);
	printf("Tail scan: last_lsn 0x%" PRIx64 " (restart area said 0x%" PRIx64 "), next page 0x%x%s, %u tail-copy override(s)\n",
	       info.last_lsn, info.current_lsn, info.next_page, info.reuse_tail ? " (reused)" : "", info.tail_overrides);
	if (err == -ENOENT) {
		printf("Checkpoint: none (client restart lsn 0); nothing to replay\n");
		goto out;
	}
	if (err) {
		printf("Checkpoint: unusable: %s\n", ntfs_logfile_last_error(log));
		rc = 1;
		goto out;
	}
	printf("Checkpoint: start lsn 0x%" PRIx64 ", client restart record v%u, attribute names %u bytes\n",
	       info.checkpoint_lsn, info.client_restart_version, info.attr_names_bytes);
	print_tables(log, "Checkpoint tables");

	printf("Records from the checkpoint:\n");
	err = ntfs_logfile_walk(log, 0, cb_rec, &wc);
	if (err) {
		printf("  walk stopped: %s (%s)\n", ntfs_logfile_last_error(log), strerror(-err));
		rc = 1;
	}
	printf("  %u record(s)%s\n", wc.count, wc.max && wc.count > wc.max ? " (truncated)" : "");

	if (!do_plan)
		goto out;
	if (info.state != NTFS_LOG_DIRTY) {
		printf("Log is not dirty: no replay plan\n");
		goto out;
	}
	printf("Dry-run replay:\n");
	err = ntfs_logfile_replay(log, &ap, true, &res);
	if (err)
		printf("  replay refused: %s (%s)\n", ntfs_logfile_last_error(log), strerror(-err));
	print_plan(&res);
	print_tables(log, "Tables after analysis");
	if (err) {
		rc = 1;
		goto out;
	}
	if (do_apply) {
		printf("Applying...\n");
		err = ntfs_logfile_replay(log, &ap, false, &res);
		if (err) {
			printf("  apply failed: %s (%s)\n", ntfs_logfile_last_error(log), strerror(-err));
			rc = 1;
			goto out;
		}
		printf("  wrote %u MFT record(s), %u cluster(s)\n", res.mft_records_written, res.clusters_written);
		err = ntfs_logfile_mark_clean(log);
		if (err) {
			printf("  mark clean failed: %s (%s)\n", ntfs_logfile_last_error(log), strerror(-err));
			rc = 1;
		} else {
			printf("  restart pages rewritten: log is clean\n");
		}
	}
out:
	ntfs_logfile_close(log);
	ntfs_image_close(&img);
	return rc;
}
