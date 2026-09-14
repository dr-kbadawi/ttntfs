// SPDX-License-Identifier: GPL-2.0
/*
 * test_geometry.c - sector size, cluster size, and volumes big enough that the
 * arithmetic has to be 64-bit.
 *
 * The fixtures in tools/images are all 512-byte-sector volumes of a few
 * megabytes, so two shapes of disk that people actually own had never been
 * fed to this driver:
 *
 *   4Kn          Every external SSD sold in the last few years reports either
 *                512-byte logical sectors on 4096-byte physical ones (512e) or
 *                4096-byte logical sectors (4Kn). A 4Kn volume changes the size
 *                of an MFT record (mkntfs makes it 4096 bytes, so it carries
 *                nine update-sequence entries instead of three), changes
 *                sb->s_blocksize, and sends every sub-block write through the
 *                read-modify-write path in platform/src/bdev_file.c.
 *
 *   Large        A multi-terabyte volume puts metadata at byte offsets that do
 *                not fit in 32 bits. $MFTMirr on a 16 TiB volume sits at LCN
 *                2147483647, byte 8796093018112, and the driver rewrites it
 *                whenever one of the first four MFT records changes. Truncate
 *                that offset to 32 bits and the write lands 8 TiB away from
 *                where it belongs, on top of live data.
 *
 * What the tests found, in short: read-only mounts work on every geometry
 * mkntfs can produce, a 16 TiB volume survives a full read-write cycle, and
 * ANY volume whose sector size was not 512 was corrupted by the first write.
 * That last one was finding 15 in docs/UPSTREAM-BUGS.md, fixed 2026-09-14 --
 * two unit errors in the upstream metadata write path, both described above
 * test_read_write_cycle(), which now runs the real cycle on every geometry.
 *
 * Every image is built here by mkntfs rather than checked in, because the
 * large one is 16 TiB of sparse file. After every mutating test the image goes
 * through `ntfsck -n`, never `ntfsck` on its own: a repair pass would be
 * measuring the repair, not the driver.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ntfscore.h"
#include "ntfsport/bdev.h"

static int failures, checks, skipped, last_checks;

/* Print what a test group covered, so a CI log says where the checks went. */
static void done(const char *name)
{
	printf("%-40s %3d checks\n", name, checks - last_checks);
	last_checks = checks;
}

#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

#define CHECK_MSG(cond, ...) do {					\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
	}								\
} while (0)

/* ---- where the tools and the scratch images live --------------------- */

static char workdir[512];

static const char *tools_dir(void)
{
	const char *d = getenv("NTFS_TOOLS");
	return d && *d ? d : "tools";
}

static int tool_path(char *buf, size_t n, const char *rel)
{
	snprintf(buf, n, "%s/%s", tools_dir(), rel);
	return access(buf, X_OK) == 0;
}

static int run(const char *cmd)
{
	int st = system(cmd);

	if (st == -1)
		return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void img_path(char *buf, size_t n, const char *name)
{
	snprintf(buf, n, "%s/%s.img", workdir, name);
}

/* ---- image construction ---------------------------------------------- */

/*
 * A sparse file of @bytes, then mkntfs on top. -F because a regular file is
 * not a block device, -Q to skip the full-surface pass: on the 16 TiB image a
 * non-quick format would write 16 TiB of zeroes.
 */
static int make_image(const char *path, u64 bytes, unsigned sector,
		      unsigned cluster, const char *extra)
{
	char cmd[2048], mk[512];
	int fd, rc;

	if (!tool_path(mk, sizeof(mk), ".local/sbin/mkntfs"))
		return -ENOENT;
	unlink(path);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -errno;
	if (ftruncate(fd, (off_t)bytes) != 0) {
		close(fd);
		return -errno;
	}
	close(fd);
	snprintf(cmd, sizeof(cmd),
		 "'%s' -F -Q -s %u -c %u %s -L GEO '%s' >/dev/null 2>&1",
		 mk, sector, cluster, extra ? extra : "", path);
	rc = run(cmd);
	if (rc != 0)
		unlink(path);
	return rc;
}

/* `ntfsck -n`: check only. Repairing the image under test would measure the
 * repair rather than what the driver left behind. */
static int fsck(const char *path)
{
	char cmd[2048], ck[512];

	if (!tool_path(ck, sizeof(ck), ".local-plus/sbin/ntfsck"))
		return -ENOENT;
	snprintf(cmd, sizeof(cmd), "'%s' -n '%s' >/dev/null 2>&1", ck, path);
	return run(cmd);
}

/* ---- sparseness ------------------------------------------------------- */

static u64 allocated_bytes(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return 0;
	return (u64)st.st_blocks * 512;
}

/*
 * mkntfs materialises $Bitmap in full, which on a 16 TiB volume is 512 MiB of
 * almost entirely zero bytes. Give them back to the file system before the
 * rest of the test runs, so CI carries ~70 MiB rather than ~600. Only the
 * ranges the file system already calls data are scanned, so this costs a read
 * of what is really allocated and not of the 16 TiB address space.
 */
static u64 resparsify(const char *path)
{
	enum { CHUNK = 1 << 20 };
	char *buf, *zero;
	struct stat st;
	off_t pos = 0;
	int fd;

	fd = open(path, O_RDWR);
	if (fd < 0)
		return 0;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return 0;
	}
	buf = malloc(CHUNK);
	zero = calloc(1, CHUNK);
	if (!buf || !zero) {
		free(buf);
		free(zero);
		close(fd);
		return 0;
	}
	while (pos < st.st_size) {
		off_t data = lseek(fd, pos, SEEK_DATA);
		off_t hole;

		if (data < 0)
			break;
		hole = lseek(fd, data, SEEK_HOLE);
		if (hole < 0)
			hole = st.st_size;
		for (off_t p = data; p < hole; p += CHUNK) {
			size_t len = (size_t)((hole - p < CHUNK) ? hole - p : CHUNK);
			fpunchhole_t ph = { 0, 0, p, (off_t)len };

			if (pread(fd, buf, len, p) != (ssize_t)len)
				break;
			if (memcmp(buf, zero, len) == 0)
				(void)fcntl(fd, F_PUNCHHOLE, &ph);
		}
		pos = hole;
	}
	free(buf);
	free(zero);
	close(fd);
	return allocated_bytes(path);
}

/*
 * Find @pat in the image without reading the holes. Returns the byte offset,
 * which for a file the driver placed is (first LCN * cluster size).
 */
static int find_pattern(const char *path, const void *pat, size_t patlen, u64 *out)
{
	enum { CHUNK = 4096 };
	unsigned char blk[CHUNK];
	struct stat st;
	off_t pos = 0;
	int fd, found = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return 0;
	}
	while (pos < st.st_size && !found) {
		off_t data = lseek(fd, pos, SEEK_DATA);
		off_t hole;

		if (data < 0)
			break;
		hole = lseek(fd, data, SEEK_HOLE);
		if (hole < 0)
			hole = st.st_size;
		for (off_t p = data; p < hole; p += CHUNK) {
			if (pread(fd, blk, CHUNK, p) != CHUNK)
				break;
			if (memcmp(blk, pat, patlen) == 0) {
				*out = (u64)p;
				found = 1;
				break;
			}
		}
		pos = hole;
	}
	close(fd);
	return found;
}

/* ---- the boot sector, read as bytes ----------------------------------- */

struct boot_geom {
	u32 sector_size;
	u32 sectors_per_cluster;
	u32 cluster_size;
	u32 mft_record_size;
	u32 index_record_size;
	u64 total_sectors;
	u64 mft_lcn;
	u64 mftmirr_lcn;
};

static u16 rd16(const unsigned char *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const unsigned char *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd64(const unsigned char *p)
{
	u64 v = 0;

	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

/*
 * NTFS stores sectors_per_cluster, clusters_per_mft_record and
 * clusters_per_index_record as signed bytes: a negative value is the base-2
 * logarithm of a byte count, used when the record is smaller than a cluster.
 * A 4Kn volume with 64 KiB clusters hits that encoding on two of the three
 * fields at once, which is why it is worth reading them here rather than
 * trusting the driver's own parse.
 */
static u32 decode_spc(unsigned char v)
{
	if (v >= 0xf4)
		return 1u << (unsigned)(-(signed char)v);
	return v;
}

static u32 decode_record(signed char v, u32 cluster_size)
{
	if (v > 0)
		return cluster_size * (u32)v;
	return 1u << (unsigned)(-v);
}

static int read_boot(const char *path, struct boot_geom *g)
{
	unsigned char b[512];
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;
	if (pread(fd, b, sizeof(b), 0) != (ssize_t)sizeof(b)) {
		close(fd);
		return -EIO;
	}
	close(fd);
	if (memcmp(b + 3, "NTFS    ", 8) != 0)
		return -EINVAL;
	g->sector_size = rd16(b + 0x0b);
	g->sectors_per_cluster = decode_spc(b[0x0d]);
	g->cluster_size = g->sector_size * g->sectors_per_cluster;
	g->total_sectors = rd64(b + 0x28);
	g->mft_lcn = rd64(b + 0x30);
	g->mftmirr_lcn = rd64(b + 0x38);
	g->mft_record_size = decode_record((signed char)b[0x40], g->cluster_size);
	g->index_record_size = decode_record((signed char)b[0x44], g->cluster_size);
	return 0;
}

/* ---- $MFT and $MFTMirr, read straight off the image --------------------- */

/* mkntfs never makes an MFT record larger than a cluster, and the largest
 * cluster these tests build is 64 KiB. */
#define MAX_MFT_REC (64u << 10)

/*
 * Copy $MFT records 0-5 out of @path, still mst-protected. Comparing the raw
 * bytes is the point: the corruption this file was written to find left legal,
 * ntfsck-clean records behind, so only the bytes say whether they are the same
 * records as before.
 */
static void save_mft_head(const char *path, const struct boot_geom *g,
			  unsigned char rec[6][MAX_MFT_REC])
{
	int fd = open(path, O_RDONLY);

	memset(rec, 0, 6 * MAX_MFT_REC);
	if (fd < 0)
		return;
	for (unsigned i = 0; i < 6; i++)
		(void)pread(fd, rec[i], g->mft_record_size,
			    (off_t)(g->mft_lcn * g->cluster_size +
				    (u64)i * g->mft_record_size));
	close(fd);
}

/*
 * $MFTMirr holds copies of the first four $MFT records, and the copies are
 * byte-identical: ntfs_sync_mft_mirror() writes the same mst-protected buffer
 * that went into $MFT, so the update sequence numbers agree too. Returns 0 when
 * they match, or 1 + the first record number that does not.
 */
static int mirror_matches(const char *path, const struct boot_geom *g)
{
	unsigned char a[MAX_MFT_REC], b[MAX_MFT_REC];
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -1;
	for (unsigned i = 0; i < 4; i++) {
		if (pread(fd, a, g->mft_record_size,
			  (off_t)(g->mft_lcn * g->cluster_size +
				  (u64)i * g->mft_record_size)) != (ssize_t)g->mft_record_size ||
		    pread(fd, b, g->mft_record_size,
			  (off_t)(g->mftmirr_lcn * g->cluster_size +
				  (u64)i * g->mft_record_size)) != (ssize_t)g->mft_record_size) {
			close(fd);
			return -1;
		}
		if (memcmp(a, b, g->mft_record_size) != 0) {
			close(fd);
			return (int)i + 1;
		}
	}
	close(fd);
	return 0;
}

/* ---- the geometries ---------------------------------------------------- */

/*
 * @write_clean says a full read-write cycle leaves the volume ntfsck-clean.
 * It is true for every geometry mkntfs can build; it was false for every
 * non-512 sector size until finding 15 was fixed on 2026-09-14. It stays as a
 * field rather than becoming an assumption because the geometries mkntfs
 * cannot build have no answer either way.
 */
struct geom {
	const char *name;
	unsigned sector;
	unsigned cluster;
	bool mkntfs_can;
	bool write_clean;
};

static const struct geom geoms[] = {
	/* cluster == sector */
	{ "s512-c512",	  512,	 512,	true,  true  },
	{ "s1k-c1k",	 1024,	1024,	true,  true  },
	{ "s2k-c2k",	 2048,	2048,	true,  true  },
	{ "s4kn-c4k",	 4096,	4096,	true,  true  },
	/* cluster > sector */
	{ "s512-c4k",	  512,	4096,	true,  true  },
	{ "s512-c64k",	  512, 65536,	true,  true  },
	{ "s4kn-c64k",	 4096, 65536,	true,  true  },
	/* cluster < sector: NTFS forbids it and mkntfs says so */
	{ "s4kn-c2k",	 4096,	2048,	false, false },
	/* sector above the 4096 mkntfs ceiling */
	{ "s8k-c8k",	 8192,	8192,	false, false },
};

#define NGEOM (sizeof(geoms) / sizeof(geoms[0]))
#define SMALL_IMAGE (64ull << 20)

/* ---- mount helpers ----------------------------------------------------- */

struct mounted {
	struct ntfs_bdev *dev;
	ntfs_volume_t *vol;
	int err;
};

static struct mounted mount_image(const char *path, bool rdonly, u32 force_lbs)
{
	struct ntfs_mount_options o = { .flags = rdonly ? NTFS_MOUNT_RDONLY : 0,
					.uid = 0, .gid = 0, .fmask = 022, .dmask = 022 };
	struct mounted m = { 0 };

	m.dev = ntfs_bdev_open_path(path, rdonly);
	if (!m.dev) {
		m.err = -ENOENT;
		return m;
	}
	if (force_lbs)
		ntfs_bdev_file_set_alignment(m.dev, force_lbs);
	m.err = ntfs_mount(m.dev, &o, &m.vol);
	if (m.err) {
		ntfs_bdev_close(m.dev);
		m.dev = NULL;
	}
	return m;
}

static void unmount_image(struct mounted *m)
{
	if (m->vol)
		ntfs_unmount(m->vol);
	m->vol = NULL;
	m->dev = NULL;		/* ntfs_unmount closes the device */
}

/* ---- 1. what mkntfs can actually express ------------------------------- */

/*
 * Designing around a geometry mkntfs cannot create would be inventing
 * coverage, so this runs first and pins the tool's limits:
 *   - sector size 256..4096, powers of two
 *   - cluster size >= sector size ("The cluster size is invalid. It must be
 *     equal to, or larger than, the sector size.")
 * Both refusals match NTFS itself, and both match what the driver's own
 * parse_ntfs_boot_sector() enforces.
 */
static void test_mkntfs_geometry_matrix(void)
{
	char path[1024];
	unsigned made = 0;

	for (size_t i = 0; i < NGEOM; i++) {
		int rc;

		img_path(path, sizeof(path), geoms[i].name);
		rc = make_image(path, SMALL_IMAGE, geoms[i].sector, geoms[i].cluster, NULL);
		if (rc == -ENOENT) {
			fprintf(stderr, "SKIP test_mkntfs_geometry_matrix: no mkntfs\n");
			skipped++;
			return;
		}
		CHECK_MSG((rc == 0) == geoms[i].mkntfs_can,
			  "%s: mkntfs returned %d, expected %s",
			  geoms[i].name, rc, geoms[i].mkntfs_can ? "success" : "refusal");
		if (rc == 0) {
			made++;
			/* A fresh image must be clean before anything touches it,
			 * otherwise every later ntfsck result is meaningless. */
			CHECK_MSG(fsck(path) == 0, "%s: fresh image already fails ntfsck",
				  geoms[i].name);
		}
	}
	CHECK(made >= 4);
	done("test_mkntfs_geometry_matrix");
}

/* ---- 2. probe versus the boot sector ----------------------------------- */

static void test_probe_matches_boot_sector(void)
{
	char path[1024];
	int any = 0;

	for (size_t i = 0; i < NGEOM; i++) {
		struct ntfs_volume_info full, light;
		struct boot_geom g;
		struct ntfs_bdev *dev;
		int rf, rl;

		if (!geoms[i].mkntfs_can)
			continue;
		img_path(path, sizeof(path), geoms[i].name);
		if (access(path, R_OK) != 0 || read_boot(path, &g) != 0)
			continue;
		any = 1;

		dev = ntfs_bdev_open_path(path, true);
		CHECK(dev != NULL);
		if (!dev)
			continue;
		rf = ntfs_probe(dev, &full);
		rl = ntfs_probe_light(dev, &light);
		ntfs_bdev_close(dev);
		CHECK_MSG(rf == 0, "%s: ntfs_probe returned %d", geoms[i].name, rf);
		CHECK_MSG(rl == 0, "%s: ntfs_probe_light returned %d", geoms[i].name, rl);
		if (rf || rl)
			continue;

		/* The geometry the driver reports has to be the geometry on
		 * the disk, including the negative-log2 encodings. */
		CHECK_MSG(full.sector_size == g.sector_size,
			  "%s: probe sector_size %u, boot sector says %u",
			  geoms[i].name, full.sector_size, g.sector_size);
		CHECK_MSG(full.cluster_size == g.cluster_size,
			  "%s: probe cluster_size %u, boot sector says %u",
			  geoms[i].name, full.cluster_size, g.cluster_size);
		CHECK_MSG(full.mft_record_size == g.mft_record_size,
			  "%s: probe mft_record_size %u, boot sector says %u",
			  geoms[i].name, full.mft_record_size, g.mft_record_size);
		CHECK_MSG(full.total_clusters == g.total_sectors / g.sectors_per_cluster,
			  "%s: probe total_clusters %llu, boot sector implies %llu",
			  geoms[i].name, (unsigned long long)full.total_clusters,
			  (unsigned long long)(g.total_sectors / g.sectors_per_cluster));
		/* And what mkntfs was asked for is what it produced. */
		CHECK_MSG(g.sector_size == geoms[i].sector && g.cluster_size == geoms[i].cluster,
			  "%s: asked mkntfs for %u/%u, got %u/%u", geoms[i].name,
			  geoms[i].sector, geoms[i].cluster, g.sector_size, g.cluster_size);

		/* The light probe reads one sector and must not disagree. */
		CHECK(light.sector_size == full.sector_size);
		CHECK(light.cluster_size == full.cluster_size);
		CHECK(light.mft_record_size == full.mft_record_size);
		CHECK(light.total_clusters == full.total_clusters);
	}
	if (!any) {
		fprintf(stderr, "SKIP test_probe_matches_boot_sector: no images\n");
		skipped++;
	}
	done("test_probe_matches_boot_sector");
}

/* ---- 3. multi-sector transfer fixups when the sector is not 512 -------- */

/*
 * The update sequence array protects a record in 512-byte strides no matter
 * what the device's sector size is: mkntfs writes usa_count = size/512 + 1
 * even on a 4Kn volume, and core/ntfs/mst.c uses NTFS_BLOCK_SIZE for the same
 * reason. What the sector size does change is the size of an MFT record --
 * mkntfs never makes one smaller than a sector -- so a 4Kn volume has
 * 4096-byte records with nine fixups each where a 512e volume has 1024-byte
 * records with three. Eight fixup iterations instead of two is a different
 * path, and reading anything at all on such a volume goes through it.
 */
static void test_mft_fixups(void)
{
	char path[1024];
	int any = 0;

	for (size_t i = 0; i < NGEOM; i++) {
		unsigned char rec[65536];
		struct boot_geom g;
		struct mounted m;
		ntfs_inode_t *root;
		u16 usa_ofs, usa_count, usn;
		u64 mft_off;
		int fd;

		if (!geoms[i].mkntfs_can)
			continue;
		img_path(path, sizeof(path), geoms[i].name);
		if (access(path, R_OK) != 0 || read_boot(path, &g) != 0)
			continue;
		any = 1;

		fd = open(path, O_RDONLY);
		CHECK(fd >= 0);
		if (fd < 0)
			continue;
		mft_off = g.mft_lcn * g.cluster_size;
		CHECK(pread(fd, rec, g.mft_record_size, (off_t)mft_off) ==
		      (ssize_t)g.mft_record_size);
		close(fd);

		CHECK_MSG(memcmp(rec, "FILE", 4) == 0,
			  "%s: no FILE magic at the $MFT LCN %llu", geoms[i].name,
			  (unsigned long long)g.mft_lcn);
		usa_ofs = rd16(rec + 4);
		usa_count = rd16(rec + 6);
		CHECK_MSG(usa_count == g.mft_record_size / 512 + 1,
			  "%s: usa_count %u for a %u-byte record; the fixup stride is 512 "
			  "regardless of the %u-byte sector, so it should be %u",
			  geoms[i].name, usa_count, g.mft_record_size, g.sector_size,
			  g.mft_record_size / 512 + 1);
		CHECK_MSG(g.mft_record_size >= g.sector_size,
			  "%s: mkntfs made a %u-byte record on a %u-byte sector",
			  geoms[i].name, g.mft_record_size, g.sector_size);

		/* Every 512-byte stride ends with the update sequence number
		 * while the record is on disk. If any of them does not, the
		 * driver's own stride would have to be wrong to read it. */
		usn = rd16(rec + usa_ofs);
		for (u32 s = 0; s < usa_count - 1u; s++) {
			u16 tail = rd16(rec + (s + 1) * 512 - 2);

			CHECK_MSG(tail == usn,
				  "%s: stride %u of $MFT record 0 ends 0x%04x, usn is 0x%04x",
				  geoms[i].name, s, tail, usn);
		}

		/*
		 * And the driver has to undo all of them. A read-only mount
		 * that reaches the root directory has already applied the
		 * fixups to $MFT record 0 and to record 5.
		 */
		m = mount_image(path, true, 0);
		CHECK_MSG(m.err == 0, "%s: read-only mount failed with %d",
			  geoms[i].name, m.err);
		if (m.err)
			continue;
		CHECK_MSG(ntfs_volume_root(m.vol, &root) == 0,
			  "%s: could not read the root inode", geoms[i].name);
		if (root)
			ntfs_inode_put(root);
		unmount_image(&m);

		/*
		 * Reading a good record does not prove the stride, because a
		 * driver that skipped the fixups entirely would also read most
		 * of $MFT record 0 correctly: mkntfs leaves it 424 bytes long,
		 * so the bytes the update sequence array protects at offsets
		 * 510, 1022, 1534 and so on are all past the end of the data.
		 * Breaking one of those on purpose is what distinguishes a
		 * 512-byte stride from any other: at a 4096-byte stride the
		 * position below is not checked at all and the volume mounts.
		 */
		if (usa_count > 3) {
			char bad[1024], cmd[2200];
			u16 wrong = (u16)(usn ^ 0xa5a5);
			off_t at = (off_t)mft_off + 3 * 512 - 2;

			snprintf(bad, sizeof(bad), "%s/fixup.img", workdir);
			snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", path, bad);
			if (run(cmd) == 0) {
				fd = open(bad, O_WRONLY);
				if (fd >= 0) {
					unsigned char t[2] = { (unsigned char)wrong,
							       (unsigned char)(wrong >> 8) };

					CHECK(pwrite(fd, t, 2, at) == 2);
					close(fd);
					m = mount_image(bad, true, 0);
					CHECK_MSG(m.err != 0,
						  "%s: a broken fixup at byte %lld of $MFT "
						  "record 0 was not noticed; the fixup stride "
						  "is not 512", geoms[i].name, (long long)at);
					if (!m.err)
						unmount_image(&m);
				}
				unlink(bad);
			}
		}
	}
	if (!any) {
		fprintf(stderr, "SKIP test_mft_fixups: no images\n");
		skipped++;
	}
	done("test_mft_fixups");
}

/* ---- 4. index block fixups on a non-512 sector -------------------------- */

struct count_ctx { unsigned n; unsigned found; };

static int count_cb(const struct ntfs_dirent *ent, void *ctx)
{
	struct count_ctx *c = ctx;

	c->n++;
	if (ent->name_len > 8 && strncmp(ent->name, "geoidx", 6) == 0)
		c->found++;
	return 0;
}

/*
 * Index blocks are mst-protected too, and mkntfs keeps them at 4096 bytes even
 * when the sector is 4096, so an INDX block on a 4Kn volume carries nine
 * fixups just like its MFT records. The entries are made with ntfscp rather
 * than with the driver on purpose: reading back blocks a different
 * implementation wrote is what tests our fixup path rather than our own
 * round trip. test_read_write_cycle() covers the driver writing them.
 */
static void test_index_block_fixups(void)
{
	static const char *which[] = { "s512-c4k", "s4kn-c4k" };
	char path[1024], src[1024], cmd[2048], cp[512], name[300];
	int any = 0;

	if (!tool_path(cp, sizeof(cp), ".local/sbin/ntfscp")) {
		fprintf(stderr, "SKIP test_index_block_fixups: no ntfscp\n");
		skipped++;
		return;
	}
	snprintf(src, sizeof(src), "%s/idxsrc.bin", workdir);
	{
		int fd = open(src, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		char buf[1024];

		if (fd < 0)
			return;
		memset(buf, 'i', sizeof(buf));
		if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
			close(fd);
			return;
		}
		close(fd);
	}

	for (size_t i = 0; i < sizeof(which) / sizeof(which[0]); i++) {
		struct count_ctx c = { 0, 0 };
		struct mounted m;
		ntfs_inode_t *root;
		u64 cookie = 0;
		bool eof = false;
		int err;

		img_path(path, sizeof(path), which[i]);
		if (access(path, R_OK) != 0)
			continue;
		any = 1;

		/* Long names so a handful of entries overflow the resident
		 * $INDEX_ROOT and force real INDX blocks. */
		for (int f = 0; f < 20; f++) {
			memset(name, 'n', sizeof(name) - 1);
			name[sizeof(name) - 1] = 0;
			snprintf(name, 12, "geoidx%03d_", f);
			name[11] = 'n';
			snprintf(cmd, sizeof(cmd), "'%s' '%s' '%s' '%s' >/dev/null 2>&1",
				 cp, path, src, name);
			if (run(cmd) != 0)
				break;
		}
		CHECK_MSG(fsck(path) == 0, "%s: ntfscp left the image dirty", which[i]);

		m = mount_image(path, true, 0);
		CHECK_MSG(m.err == 0, "%s: read-only mount failed with %d", which[i], m.err);
		if (m.err)
			continue;
		err = ntfs_volume_root(m.vol, &root);
		CHECK(err == 0);
		if (!err) {
			while (!eof &&
			       ntfs_readdir(root, &cookie, false, count_cb, &c, &eof) == 0)
				;
			/* If the INDX fixups were applied with the wrong
			 * stride the block would fail its magic check and the
			 * directory would come back short or not at all. */
			CHECK_MSG(c.found == 20,
				  "%s: readdir found %u of 20 index entries (%u total)",
				  which[i], c.found, c.n);
			ntfs_inode_put(root);
		}
		unmount_image(&m);
	}
	if (!any) {
		fprintf(stderr, "SKIP test_index_block_fixups: no images\n");
		skipped++;
	}
	done("test_index_block_fixups");
}

/* ---- 5. the read-write cycle, per geometry ------------------------------ */

/*
 * This is the test that found finding 15: until 2026-09-14 the first MFT
 * record allocation on any volume whose sector size was not 512 -- a single
 * mkdir was enough -- overwrote $MFT records 0 to 5 with freshly formatted
 * empty records, and ntfsck then said "Failed to load $MFT(0), recover from
 * $MFTMirr". Two upstream defects, both in units:
 *
 *   - NTFS_B_TO_SECTOR() divided by sb->s_blocksize instead of 512, so on a
 *     4Kn volume every metadata bio landed eight times too close to the start
 *     of the device;
 *   - ntfs_sync_mft_mirror() addressed the mirror record by its offset within
 *     a folio and dropped the folio index, so with 4096-byte MFT records all
 *     four mirror records were written on top of slot 0.
 *
 * Both are fixed in core/ntfs with PORT: comments, so this now runs the real
 * cycle on every geometry: write, sync, unmount, remount, read back, delete,
 * and ntfsck -n. The remount matters -- a read served from the page cache
 * proves nothing about what reached the platter.
 */
static void test_read_write_cycle(void)
{
	static const char pattern[8] = "GEOCYCLE";
	char path[1024];
	int any = 0;

	for (size_t i = 0; i < NGEOM; i++) {
		static char buf[8192], rb[8192];
		static unsigned char before[6][MAX_MFT_REC], after[6][MAX_MFT_REC];
		struct boot_geom g;
		struct mounted m;
		ntfs_inode_t *root, *f, *d;
		int err, ck;

		if (!geoms[i].mkntfs_can || !geoms[i].write_clean)
			continue;
		img_path(path, sizeof(path), geoms[i].name);
		if (access(path, R_OK) != 0)
			continue;
		any = 1;
		read_boot(path, &g);
		save_mft_head(path, &g, before);

		m = mount_image(path, false, 0);
		CHECK_MSG(m.err == 0, "%s: read-write mount failed with %d",
			  geoms[i].name, m.err);
		if (m.err)
			continue;
		err = ntfs_volume_root(m.vol, &root);
		CHECK(err == 0);
		if (err) {
			unmount_image(&m);
			continue;
		}

		CHECK_MSG(ntfs_mkdir(root, "geodir", 0755, &d) == 0,
			  "%s: mkdir failed", geoms[i].name);
		if (d)
			ntfs_inode_put(d);
		err = ntfs_create(root, "geofile.bin", 0100644, &f);
		CHECK_MSG(err == 0, "%s: create failed with %d", geoms[i].name, err);
		if (!err) {
			for (size_t k = 0; k < sizeof(buf); k += 8)
				memcpy(buf + k, pattern, 8);
			CHECK_MSG(ntfs_write(f, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf),
				  "%s: 8 KiB write short", geoms[i].name);
			CHECK_MSG(ntfs_read(f, rb, sizeof(rb), 0) == (ssize_t)sizeof(rb) &&
				  memcmp(buf, rb, sizeof(buf)) == 0,
				  "%s: 8 KiB read back does not match", geoms[i].name);

			/*
			 * Smaller than any logical block, and not aligned to
			 * one: on a 4Kn volume this is the read-modify-write
			 * path in bdev_file.c. The bytes on either side have to
			 * survive it.
			 */
			{
				char small[7];

				memset(small, 'Z', sizeof(small));
				CHECK_MSG(ntfs_write(f, small, sizeof(small), 1000) == 7,
					  "%s: 7-byte write short", geoms[i].name);
				CHECK(ntfs_read(f, rb, sizeof(rb), 0) == (ssize_t)sizeof(rb));
				CHECK_MSG(memcmp(rb + 1000, small, 7) == 0,
					  "%s: 7-byte write did not land", geoms[i].name);
				CHECK_MSG(memcmp(rb, buf, 1000) == 0 &&
					  memcmp(rb + 1007, buf + 1007, sizeof(rb) - 1007) == 0,
					  "%s: 7-byte write damaged the bytes around it",
					  geoms[i].name);
			}
			CHECK(ntfs_fsync(f, false) == 0);
			ntfs_inode_put(f);
		}
		CHECK(ntfs_volume_sync(m.vol) == 0);
		ntfs_inode_put(root);
		unmount_image(&m);

		/*
		 * Remount, read back, delete. The remount is the point: it
		 * drops every cached folio, so the comparison below is against
		 * the bytes that actually reached the image.
		 */
		{
			m = mount_image(path, false, 0);
			CHECK_MSG(m.err == 0, "%s: remount failed with %d",
				  geoms[i].name, m.err);
			if (!m.err) {
				err = ntfs_volume_root(m.vol, &root);
				CHECK(err == 0);
				if (!err) {
					err = ntfs_lookup(root, "geofile.bin", &f);
					CHECK_MSG(err == 0, "%s: file gone after remount",
						  geoms[i].name);
					if (!err) {
						CHECK(ntfs_read(f, rb, sizeof(rb), 0) ==
						      (ssize_t)sizeof(rb));
						CHECK_MSG(memcmp(rb, pattern, 8) == 0,
							  "%s: contents changed across the remount",
							  geoms[i].name);
						ntfs_inode_put(f);
					}
					CHECK_MSG(ntfs_unlink(root, "geofile.bin") == 0,
						  "%s: unlink failed", geoms[i].name);
					CHECK_MSG(ntfs_rmdir(root, "geodir") == 0,
						  "%s: rmdir failed", geoms[i].name);
					CHECK(ntfs_lookup(root, "geofile.bin", &f) == -ENOENT);
					ntfs_inode_put(root);
				}
				CHECK(ntfs_volume_sync(m.vol) == 0);
				unmount_image(&m);
			}
		}

		ck = fsck(path);
		CHECK_MSG(ck == 0,
			  "%s: ntfsck -n returned %d after a read-write cycle",
			  geoms[i].name, ck);

		/*
		 * ntfsck being happy is not enough on its own -- a freshly
		 * formatted $MFT is legal NTFS. These are the six records the
		 * bug reformatted, compared byte for byte with what they were
		 * before the cycle. Everything the cycle created was deleted
		 * again above, so the only records that may legitimately have
		 * moved are the ones whose timestamps or $BITMAP the driver
		 * touched; their identity fields may not.
		 */
		save_mft_head(path, &g, after);
		for (unsigned r = 0; r < 6; r++) {
			const unsigned char *b = before[r], *a = after[r];

			CHECK_MSG(memcmp(a, "FILE", 4) == 0,
				  "%s: $MFT record %u lost its FILE magic",
				  geoms[i].name, r);
			CHECK_MSG(rd16(a + 0x06) == rd16(b + 0x06),
				  "%s: $MFT record %u usa_count %u, was %u",
				  geoms[i].name, r, rd16(a + 0x06), rd16(b + 0x06));
			CHECK_MSG(rd16(a + 0x12) == rd16(b + 0x12),
				  "%s: $MFT record %u link_count %u, was %u",
				  geoms[i].name, r, rd16(a + 0x12), rd16(b + 0x12));
			CHECK_MSG((rd16(a + 0x16) & 1) == 1,
				  "%s: $MFT record %u is no longer marked in use",
				  geoms[i].name, r);
			CHECK_MSG(rd32(a + 0x2c) == rd32(b + 0x2c),
				  "%s: $MFT record %u says it is record %u, was %u",
				  geoms[i].name, r, rd32(a + 0x2c), rd32(b + 0x2c));
			CHECK_MSG(rd32(a + 0x18) >= 0x100,
				  "%s: $MFT record %u bytes_in_use fell to %u, "
				  "which is an empty record",
				  geoms[i].name, r, rd32(a + 0x18));
		}
		/*
		 * And the mirror: ntfs_sync_mft_mirror() used to write every
		 * record it was given on top of slot 0 whenever an MFT record
		 * filled a whole page.
		 */
		CHECK_MSG(mirror_matches(path, &g) == 0,
			  "%s: $MFTMirr does not match $MFT after the cycle",
			  geoms[i].name);
	}
	if (!any) {
		fprintf(stderr, "SKIP test_read_write_cycle: no images\n");
		skipped++;
	}
	done("test_read_write_cycle");
}

/* ---- 6. the sub-block read-modify-write path ---------------------------- */

/*
 * bdev_file.c only takes the read-modify-write path when the device says its
 * logical block is larger than the request, which for a regular file it never
 * does. ntfs_bdev_file_set_alignment() is the hook that makes a file behave
 * like a 4Kn disk, so this is the same code that runs against a real one.
 * The finding this guards is finding 9 in docs/progress/platform-review.md:
 * two unaligned writes to the same block used to lose each other's bytes.
 */
static void test_sub_block_rmw(void)
{
	char path[1024];
	unsigned char before[16384], after[16384], patch[7];
	struct ntfs_bdev *dev;
	u64 base;
	int fd;

	/* Its own image: by now the shared 4Kn one has been through a
	 * read-write cycle and is corrupt, so ntfsck could not say anything
	 * about what this test did to it. */
	img_path(path, sizeof(path), "rmw4kn");
	if (make_image(path, SMALL_IMAGE, 4096, 4096, NULL) != 0) {
		fprintf(stderr, "SKIP test_sub_block_rmw: no 4Kn image\n");
		skipped++;
		return;
	}

	/* Somewhere in the unused tail of the image, so the file system is not
	 * disturbed and ntfsck still has something meaningful to say. */
	base = SMALL_IMAGE - (u64)sizeof(before) - 65536;
	base &= ~4095ull;

	fd = open(path, O_RDWR);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	for (size_t i = 0; i < sizeof(before); i++)
		before[i] = (unsigned char)(i * 7 + 13);
	CHECK(pwrite(fd, before, sizeof(before), (off_t)base) == (ssize_t)sizeof(before));
	close(fd);

	dev = ntfs_bdev_open_path(path, false);
	CHECK(dev != NULL);
	if (!dev)
		return;
	ntfs_bdev_file_set_alignment(dev, 4096);
	CHECK(bdev_logical_block_size(dev) == 4096);

	/* Seven bytes wholly inside one 4096-byte block. */
	memset(patch, 'Z', sizeof(patch));
	CHECK(ntfs_bdev_write(dev, patch, base + 1000, sizeof(patch)) == 0);
	/* And seven bytes straddling the boundary between two of them. */
	CHECK(ntfs_bdev_write(dev, patch, base + 4093, sizeof(patch)) == 0);
	CHECK(ntfs_bdev_flush(dev) == 0);
	ntfs_bdev_close(dev);

	fd = open(path, O_RDONLY);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(pread(fd, after, sizeof(after), (off_t)base) == (ssize_t)sizeof(after));
	close(fd);

	CHECK_MSG(memcmp(after + 1000, patch, 7) == 0, "rmw: the 7 bytes did not land");
	CHECK_MSG(memcmp(after + 4093, patch, 7) == 0,
		  "rmw: the straddling 7 bytes did not land");
	/* Everything else in both blocks, and the blocks on either side, has to
	 * be byte-for-byte what it was. */
	CHECK_MSG(memcmp(after, before, 1000) == 0, "rmw: bytes before the patch changed");
	CHECK_MSG(memcmp(after + 1007, before + 1007, 4093 - 1007) == 0,
		  "rmw: bytes between the two patches changed");
	CHECK_MSG(memcmp(after + 4100, before + 4100, sizeof(after) - 4100) == 0,
		  "rmw: bytes after the patch changed");
	/* And nothing the file system cares about moved. */
	CHECK_MSG(fsck(path) == 0, "rmw: ntfsck -n failed after the sub-block writes");
	unlink(path);

	done("test_sub_block_rmw");
}

/* ---- 7. a 512-byte-sector volume on a 4096-byte-block device ------------ */

/*
 * This is the combination a 512e image gets when it is restored onto a 4Kn
 * disk. parse_ntfs_boot_sector() refuses it ("Sector size is smaller than the
 * device block size"), which is right -- the driver cannot write a 512-byte
 * sector atomically to a device that has no such thing -- but it is worth
 * pinning, because the failure is a bare -EINVAL out of ntfs_mount() and
 * nothing else in the tree says why.
 *
 * Note also what a 512e disk is NOT: the NTFS boot sector records only the
 * logical sector size, so a volume on a 512e disk is byte-identical to one on
 * a 512n disk. mkntfs has no flag for it and there is nothing to test that is
 * not already covered by the s512-* rows. The only place the difference shows
 * is the device's physical block size, which bdev_file.c reports as 4096 for
 * every regular file -- so the fixtures are already the 512e case.
 */
static void test_sector_smaller_than_device_block(void)
{
	char path[1024];
	struct mounted m;
	struct ntfs_bdev *dev;

	img_path(path, sizeof(path), "s512-c4k");
	if (access(path, R_OK) != 0) {
		fprintf(stderr, "SKIP test_sector_smaller_than_device_block: no image\n");
		skipped++;
		return;
	}
	/* The 512e shape as bdev_file.c already reports it for a plain file. */
	dev = ntfs_bdev_open_path(path, true);
	CHECK(dev != NULL);
	if (dev) {
		CHECK(bdev_logical_block_size(dev) == 512);
		CHECK(bdev_physical_block_size(dev) == 4096);
		ntfs_bdev_close(dev);
	}

	m = mount_image(path, false, 4096);
	CHECK_MSG(m.err == -EINVAL,
		  "a 512-byte-sector volume on a 4096-byte-block device gave %d, expected -EINVAL",
		  m.err);
	if (!m.err)
		unmount_image(&m);
	done("test_sector_smaller_than_device_block");
}

/* ---- 8. a large, sparse volume ----------------------------------------- */

#define BIG_BYTES	(16ull << 40)	/* 16 TiB: 2^32-1 clusters of 4 KiB, the
					 * largest cluster count NTFS allows */

/*
 * The reason for the size is not the size. At 16 TiB with 4 KiB clusters the
 * volume holds 0xffffffff clusters, mkntfs puts $MFTMirr at LCN 2147483647 --
 * byte 8796093018112 -- and the driver rewrites the mirror whenever one of the
 * first four MFT records changes. Truncate that byte offset to 32 bits and the
 * write lands somewhere in the first 4 GiB of the disk instead.
 *
 * The image costs about 600 MiB while mkntfs is materialising $Bitmap and
 * about 70 MiB afterwards, once resparsify() has given the zeroes back.
 */
static void test_large_volume(void)
{
	static const char magic[16] = "GEOBIGMAGICGEOBI";
	static char buf[65536], rb[65536];
	char path[1024];
	struct ntfs_volume_info info;
	struct boot_geom g;
	struct mounted m;
	ntfs_inode_t *root, *f;
	unsigned char mirr_before[4096], mirr_after[4096];
	u64 alloc, mirr_off, payload = 0;
	struct stat st;
	int rc, fd, err;

	img_path(path, sizeof(path), "big16t");
	rc = make_image(path, BIG_BYTES, 512, 4096, NULL);
	if (rc == -ENOENT) {
		fprintf(stderr, "SKIP test_large_volume: no mkntfs\n");
		skipped++;
		return;
	}
	CHECK_MSG(rc == 0, "mkntfs refused a %llu-byte sparse image (rc %d)",
		  (unsigned long long)BIG_BYTES, rc);
	if (rc != 0)
		return;

	alloc = resparsify(path);
	CHECK(stat(path, &st) == 0);
	CHECK_MSG((u64)st.st_size == BIG_BYTES, "image is %llu bytes, wanted %llu",
		  (unsigned long long)st.st_size, (unsigned long long)BIG_BYTES);
	/* If this ever fails, CI is about to write terabytes. */
	CHECK_MSG(alloc > 0 && alloc < BIG_BYTES / 1000,
		  "image is not sparse: %llu bytes allocated of %llu",
		  (unsigned long long)alloc, (unsigned long long)BIG_BYTES);
	printf("  16 TiB image: %llu MiB really on disk\n",
	       (unsigned long long)(alloc >> 20));

	CHECK(read_boot(path, &g) == 0);
	CHECK_MSG(g.total_sectors / g.sectors_per_cluster == 0xffffffffull,
		  "expected 2^32-1 clusters, boot sector says %llu",
		  (unsigned long long)(g.total_sectors / g.sectors_per_cluster));
	mirr_off = g.mftmirr_lcn * g.cluster_size;
	CHECK_MSG(g.mftmirr_lcn > (1u << 30),
		  "$MFTMirr is at LCN %llu, too low to be interesting",
		  (unsigned long long)g.mftmirr_lcn);
	CHECK_MSG(mirr_off > 0xffffffffull,
		  "$MFTMirr byte offset %llu fits in 32 bits", (unsigned long long)mirr_off);

	fd = open(path, O_RDONLY);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(pread(fd, mirr_before, sizeof(mirr_before), (off_t)mirr_off) ==
	      (ssize_t)sizeof(mirr_before));
	close(fd);
	CHECK_MSG(memcmp(mirr_before, "FILE", 4) == 0,
		  "no FILE magic at the $MFTMirr offset %llu",
		  (unsigned long long)mirr_off);

	/* Probe has to agree with the boot sector at this size too. */
	{
		struct ntfs_bdev *dev = ntfs_bdev_open_path(path, true);

		CHECK(dev != NULL);
		if (dev) {
			CHECK(ntfs_probe(dev, &info) == 0);
			CHECK_MSG(info.total_clusters == 0xffffffffull,
				  "probe says %llu clusters",
				  (unsigned long long)info.total_clusters);
			CHECK(info.cluster_size == 4096);
			CHECK(info.sector_size == 512);
			ntfs_bdev_close(dev);
		}
	}

	m = mount_image(path, false, 0);
	CHECK_MSG(m.err == 0, "16 TiB read-write mount failed with %d", m.err);
	if (m.err)
		goto out;
	ntfs_volume_get_info(m.vol, &info);
	CHECK(!info.read_only);
	CHECK(info.total_clusters == 0xffffffffull);

	err = ntfs_volume_root(m.vol, &root);
	CHECK(err == 0);
	if (!err) {
		for (size_t i = 0; i < sizeof(buf); i += sizeof(magic))
			memcpy(buf + i, magic, sizeof(magic));
		err = ntfs_create(root, "bigfile.bin", 0100644, &f);
		CHECK_MSG(err == 0, "create on the 16 TiB volume failed with %d", err);
		if (!err) {
			CHECK(ntfs_write(f, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
			CHECK(ntfs_read(f, rb, sizeof(rb), 0) == (ssize_t)sizeof(rb));
			CHECK(memcmp(buf, rb, sizeof(buf)) == 0);
			CHECK(ntfs_fsync(f, false) == 0);
			ntfs_inode_put(f);
		}
		ntfs_inode_put(root);
	}
	CHECK(ntfs_volume_sync(m.vol) == 0);
	unmount_image(&m);

	/*
	 * The mirror had to be rewritten: mounting read-write changes the
	 * volume flags in MFT record 3, and the mirror covers records 0 to 3.
	 * That write went to byte 8796093018112 on a device whose block layer
	 * only ever sees 64-bit offsets. If it did not change, this test is no
	 * longer proving anything about large offsets and needs rethinking.
	 */
	fd = open(path, O_RDONLY);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK(pread(fd, mirr_after, sizeof(mirr_after), (off_t)mirr_off) ==
		      (ssize_t)sizeof(mirr_after));
		close(fd);
		CHECK_MSG(memcmp(mirr_after, "FILE", 4) == 0,
			  "$MFTMirr lost its FILE magic across the read-write cycle");
		CHECK_MSG(memcmp(mirr_before, mirr_after, sizeof(mirr_after)) != 0,
			  "$MFTMirr at byte %llu was not rewritten; the large-offset "
			  "write this test is built around did not happen",
			  (unsigned long long)mirr_off);
	}

	/*
	 * And the file's own data went somewhere past the 32-bit byte mark.
	 * The driver's allocator starts at the end of the MFT zone, which on a
	 * 2^32-cluster volume is LCN 2^29, byte 2 TiB.
	 */
	CHECK_MSG(find_pattern(path, magic, sizeof(magic), &payload),
		  "could not find the written payload anywhere in the image");
	if (payload) {
		CHECK_MSG(payload > 0xffffffffull,
			  "payload landed at byte %llu, which fits in 32 bits",
			  (unsigned long long)payload);
		printf("  payload at byte %llu (LCN %llu)\n",
		       (unsigned long long)payload,
		       (unsigned long long)(payload / 4096));
	}

	/* Remount, read it back, delete it, and check the volume. */
	m = mount_image(path, false, 0);
	CHECK_MSG(m.err == 0, "16 TiB remount failed with %d", m.err);
	if (!m.err) {
		err = ntfs_volume_root(m.vol, &root);
		CHECK(err == 0);
		if (!err) {
			err = ntfs_lookup(root, "bigfile.bin", &f);
			CHECK_MSG(err == 0, "bigfile.bin gone after remount");
			if (!err) {
				CHECK(ntfs_read(f, rb, sizeof(rb), 0) == (ssize_t)sizeof(rb));
				CHECK(memcmp(rb, magic, sizeof(magic)) == 0);
				ntfs_inode_put(f);
			}
			CHECK(ntfs_unlink(root, "bigfile.bin") == 0);
			ntfs_inode_put(root);
		}
		CHECK(ntfs_volume_sync(m.vol) == 0);
		unmount_image(&m);
	}

	CHECK_MSG(fsck(path) == 0, "ntfsck -n failed on the 16 TiB volume");
	alloc = allocated_bytes(path);
	printf("  after the cycle: %llu MiB really on disk\n",
	       (unsigned long long)(alloc >> 20));
out:
	unlink(path);
	done("test_large_volume");
}

/* ---- 9. the ceiling that keeps every LCN inside 32 bits ----------------- */

/*
 * Worth stating plainly, because it is what makes "clusters beyond the 32-bit
 * boundary" untestable rather than untested: NTFS caps a volume at 2^32
 * clusters, and parse_ntfs_boot_sector() refuses anything larger with "Cannot
 * handle 64-bit clusters". mkntfs refuses to format past it as well (17 TiB
 * with 4 KiB clusters is rejected; 256 TiB with 64 KiB clusters is exactly at
 * the cap). So an LCN never needs more than 32 bits on any volume that can
 * exist -- but the byte offsets derived from one need up to 48, which is what
 * test_large_volume() covers.
 *
 * This patches the sector count in a copy of a small image and checks the
 * refusal, so the ceiling is a tested boundary and not a comment.
 */
static void test_cluster_count_ceiling(void)
{
	char src[1024], path[1024];
	struct ntfs_volume_info info;
	struct ntfs_bdev *dev;
	struct boot_geom g;
	unsigned char boot[512];
	u64 too_many;
	int fd, rc;

	img_path(src, sizeof(src), "s512-c4k");
	snprintf(path, sizeof(path), "%s/ceiling.img", workdir);
	if (access(src, R_OK) != 0 || read_boot(src, &g) != 0) {
		fprintf(stderr, "SKIP test_cluster_count_ceiling: no image\n");
		skipped++;
		return;
	}
	{
		char cmd[2200];

		snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", src, path);
		if (run(cmd) != 0)
			return;
	}

	fd = open(path, O_RDWR);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	CHECK(pread(fd, boot, sizeof(boot), 0) == (ssize_t)sizeof(boot));
	/* Exactly one cluster too many. */
	too_many = (1ull << 32) * g.sectors_per_cluster;
	for (int i = 0; i < 8; i++)
		boot[0x28 + i] = (unsigned char)(too_many >> (8 * i));
	CHECK(pwrite(fd, boot, sizeof(boot), 0) == (ssize_t)sizeof(boot));
	close(fd);

	dev = ntfs_bdev_open_path(path, true);
	CHECK(dev != NULL);
	if (dev) {
		ntfs_volume_t *vol = NULL;
		struct ntfs_mount_options o = { .flags = NTFS_MOUNT_RDONLY,
						.fmask = 022, .dmask = 022 };

		rc = ntfs_mount(dev, &o, &vol);
		CHECK_MSG(rc != 0,
			  "a volume claiming 2^32 clusters mounted; the driver says it "
			  "cannot handle 64-bit clusters, so it must refuse");
		if (rc == 0)
			ntfs_unmount(vol);
		else
			ntfs_bdev_close(dev);
	}

	/* One cluster fewer is the largest legal volume and must be accepted
	 * by the probe, which is the boundary rather than the far side of it. */
	fd = open(path, O_RDWR);
	if (fd >= 0) {
		too_many = ((1ull << 32) - 1) * g.sectors_per_cluster;
		for (int i = 0; i < 8; i++)
			boot[0x28 + i] = (unsigned char)(too_many >> (8 * i));
		CHECK(pwrite(fd, boot, sizeof(boot), 0) == (ssize_t)sizeof(boot));
		close(fd);
		dev = ntfs_bdev_open_path(path, true);
		if (dev) {
			CHECK_MSG(ntfs_probe(dev, &info) == 0,
				  "probe refused a volume of 2^32-1 clusters");
			CHECK(info.total_clusters == 0xffffffffull);
			ntfs_bdev_close(dev);
		}
	}
	unlink(path);
	done("test_cluster_count_ceiling");
}

/* ------------------------------------------------------------------------ */

static void cleanup(void)
{
	char path[1024];

	for (size_t i = 0; i < NGEOM; i++) {
		img_path(path, sizeof(path), geoms[i].name);
		unlink(path);
	}
	img_path(path, sizeof(path), "big16t");
	unlink(path);
	img_path(path, sizeof(path), "rmw4kn");
	unlink(path);
	snprintf(path, sizeof(path), "%s/fixup.img", workdir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/idxsrc.bin", workdir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/ceiling.img", workdir);
	unlink(path);
	rmdir(workdir);
}

int main(void)
{
	char probe[512];

	snprintf(workdir, sizeof(workdir), "/tmp/ttntfs-geometry-%d", (int)getpid());
	if (mkdir(workdir, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "cannot create %s: %s\n", workdir, strerror(errno));
		return 1;
	}
	if (!tool_path(probe, sizeof(probe), ".local/sbin/mkntfs") ||
	    !tool_path(probe, sizeof(probe), ".local-plus/sbin/ntfsck")) {
		fprintf(stderr,
			"SKIP test_geometry: mkntfs or ntfsck missing under %s. "
			"See the symlinks in the top-level README.\n", tools_dir());
		rmdir(workdir);
		return 0;
	}

	test_mkntfs_geometry_matrix();
	test_probe_matches_boot_sector();
	test_mft_fixups();
	test_index_block_fixups();
	test_read_write_cycle();
	test_sub_block_rmw();
	test_sector_smaller_than_device_block();
	test_large_volume();
	test_cluster_count_ceiling();

	cleanup();
	printf("%d checks, %d failures, %d skipped groups\n", checks, failures, skipped);
	return failures ? 1 : 0;
}
