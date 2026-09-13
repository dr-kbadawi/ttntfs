/* Wrap a file-backed bdev with a counting shim and histogram every device
 * transfer the core makes, so a 4 KiB file write can be traced to its real
 * device I/O size. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "ntfscore.h"
#include "ntfsport/bdev.h"

#define NB 24
static unsigned long rd[NB], wr[NB];
static unsigned long rd_bytes, wr_bytes;
static int trace_on;
static const struct ntfs_bdev_ops *inner;

static int bucket(size_t n){ int b=0; while ((1u<<b) < n && b < NB-1) b++; return b; }

static ssize_t t_pread(struct ntfs_bdev *d, void *buf, size_t c, u64 off)
{ if (trace_on){ rd[bucket(c)]++; rd_bytes+=c; } return inner->pread(d,buf,c,off); }
static ssize_t t_pwrite(struct ntfs_bdev *d, const void *buf, size_t c, u64 off)
{ if (trace_on){ wr[bucket(c)]++; wr_bytes+=c; } return inner->pwrite(d,buf,c,off); }
static int t_flush(struct ntfs_bdev *d){ return inner->flush(d); }
static void t_close(struct ntfs_bdev *d){ inner->close(d); }
static struct ntfs_bdev_ops tops;

static void dump(const char *what)
{
	int b; printf("  %-22s", what);
	printf("reads %lu (%lu KiB)  writes %lu (%lu KiB)\n",
	       0UL,0UL,0UL,0UL);
	printf("    size      reads   writes\n");
	for (b=0;b<NB;b++) if (rd[b]||wr[b])
		printf("    %6u  %9lu %8lu\n", 1u<<b, rd[b], wr[b]);
	printf("    total   %9lu KiB %5lu KiB\n", rd_bytes>>10, wr_bytes>>10);
}

int main(int argc, char **argv)
{
	struct ntfs_bdev *dev; ntfs_volume_t *vol; ntfs_inode_t *root, *f;
	struct ntfs_mount_options o = { .flags = 0, .uid = 0, .gid = 0, .fmask = 022, .dmask = 022 };
	char *buf; int i, err; unsigned seed = 7;
	const char *img = argv[1];
	int nwrites = argc > 2 ? atoi(argv[2]) : 500;

	dev = ntfs_bdev_open_path(img, false);
	if (!dev) { fprintf(stderr,"open %s failed\n", img); return 1; }
	inner = dev->ops; tops = *inner;
	tops.pread = t_pread; tops.pwrite = t_pwrite; tops.flush = t_flush; tops.close = t_close;
	dev->ops = &tops;

	if ((err = ntfs_mount(dev, &o, &vol))) { fprintf(stderr,"mount: %d\n", err); return 1; }
	if ((err = ntfs_volume_root(vol, &root))) { fprintf(stderr,"root: %d\n", err); return 1; }

	buf = malloc(1<<20); memset(buf, 0xA5, 1<<20);

	/* Create an 8 MiB file. */
	err = ntfs_create(root, "trace.bin", 0100644, &f);
	if (err) { fprintf(stderr,"create: %d\n", err); return 1; }
	for (i = 0; i < 8; i++)
		if (ntfs_write(f, buf, 1<<20, (uint64_t)i<<20) < 0) { fprintf(stderr,"setup write\n"); return 1; }
	ntfs_volume_sync(vol);

	/* --- traced phase: random 4 KiB writes into the existing file --- */
	memset(rd,0,sizeof rd); memset(wr,0,sizeof wr); rd_bytes=wr_bytes=0;
	trace_on = 1;
	for (i = 0; i < nwrites; i++) {
		uint64_t off = ((uint64_t)(rand_r(&seed) % (2048 - 1))) * 4096;
		if (ntfs_write(f, buf, 4096, off) < 0) { fprintf(stderr,"rand write %d\n", i); return 1; }
	}
	trace_on = 0;
	printf("random 4 KiB writes: %d\n", nwrites);
	dump("during writes");

	/* --- traced phase: the sync that follows --- */
	memset(rd,0,sizeof rd); memset(wr,0,sizeof wr); rd_bytes=wr_bytes=0;
	trace_on = 1;
	ntfs_volume_sync(vol);
	trace_on = 0;
	printf("\nthe following sync:\n");
	dump("during sync");

	ntfs_inode_put(f); ntfs_inode_put(root); ntfs_unmount(vol);
	free(buf);
	return 0;
}
