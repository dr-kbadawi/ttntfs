/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub implementation of ntfscore.h. Lets fskit/ and tools/ link and run
 * before core/vfs/ exists. Every operation fails with -ENOSYS except the
 * ones needed to exercise plumbing (probe, mount of nothing, logger).
 *
 * Build: cc -I core/include -c core/stub/ntfscore_stub.c
 */
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include "ntfscore.h"

static ntfs_log_fn g_log;
static void *g_log_ctx;

void ntfs_set_logger(ntfs_log_fn fn, void *ctx) { g_log = fn; g_log_ctx = ctx; }
const char *ntfs_core_version(void) { return "stub-0.0"; }

int ntfs_probe(struct ntfs_bdev *dev, struct ntfs_volume_info *info)
{
	(void)dev;
	if (info) memset(info, 0, sizeof(*info));
	if (g_log) g_log(NTFS_LOG_INFO, "stub: probe -> ENXIO", g_log_ctx);
	return -ENXIO;
}

int ntfs_mount(struct ntfs_bdev *dev, const struct ntfs_mount_options *opts, ntfs_volume_t **vol_out)
{ (void)dev; (void)opts; *vol_out = NULL; return -ENOSYS; }
int ntfs_unmount(ntfs_volume_t *vol) { (void)vol; return -ENOSYS; }
int ntfs_volume_get_info(ntfs_volume_t *vol, struct ntfs_volume_info *info) { (void)vol; (void)info; return -ENOSYS; }
int ntfs_volume_sync(ntfs_volume_t *vol) { (void)vol; return -ENOSYS; }
int ntfs_volume_root(ntfs_volume_t *vol, ntfs_inode_t **root_out) { (void)vol; *root_out = NULL; return -ENOSYS; }
int ntfs_volume_set_label(ntfs_volume_t *vol, const char *label) { (void)vol; (void)label; return -ENOSYS; }

int ntfs_inode_get(ntfs_volume_t *vol, uint64_t ino, ntfs_inode_t **out) { (void)vol; (void)ino; *out = NULL; return -ENOSYS; }
void ntfs_inode_ref(ntfs_inode_t *ni) { (void)ni; }
void ntfs_inode_put(ntfs_inode_t *ni) { (void)ni; }
uint64_t ntfs_inode_number(ntfs_inode_t *ni) { (void)ni; return 0; }
int ntfs_getattr(ntfs_inode_t *ni, struct ntfs_attr *attr) { (void)ni; (void)attr; return -ENOSYS; }
int ntfs_setattr(ntfs_inode_t *ni, const struct ntfs_attr *attr, uint32_t valid) { (void)ni; (void)attr; (void)valid; return -ENOSYS; }

int ntfs_lookup(ntfs_inode_t *dir, const char *name, ntfs_inode_t **out) { (void)dir; (void)name; *out = NULL; return -ENOSYS; }
int ntfs_create(ntfs_inode_t *dir, const char *name, uint32_t mode, ntfs_inode_t **out) { (void)dir; (void)name; (void)mode; *out = NULL; return -ENOSYS; }
int ntfs_mkdir(ntfs_inode_t *dir, const char *name, uint32_t mode, ntfs_inode_t **out) { (void)dir; (void)name; (void)mode; *out = NULL; return -ENOSYS; }
int ntfs_symlink(ntfs_inode_t *dir, const char *name, const char *target, ntfs_inode_t **out) { (void)dir; (void)name; (void)target; *out = NULL; return -ENOSYS; }
int ntfs_readlink(ntfs_inode_t *ni, char *buf, size_t bufsize, size_t *len_out) { (void)ni; (void)buf; (void)bufsize; *len_out = 0; return -ENOSYS; }
int ntfs_link(ntfs_inode_t *ni, ntfs_inode_t *dir, const char *name) { (void)ni; (void)dir; (void)name; return -ENOSYS; }
int ntfs_unlink(ntfs_inode_t *dir, const char *name) { (void)dir; (void)name; return -ENOSYS; }
int ntfs_rmdir(ntfs_inode_t *dir, const char *name) { (void)dir; (void)name; return -ENOSYS; }
int ntfs_rename(ntfs_inode_t *od, const char *on, ntfs_inode_t *nd, const char *nn) { (void)od; (void)on; (void)nd; (void)nn; return -ENOSYS; }
int ntfs_readdir(ntfs_inode_t *dir, uint64_t *cookie, bool want_attr, ntfs_readdir_cb cb, void *ctx, bool *eof)
{ (void)dir; (void)cookie; (void)want_attr; (void)cb; (void)ctx; *eof = true; return -ENOSYS; }

ssize_t ntfs_read(ntfs_inode_t *ni, void *buf, size_t count, uint64_t off) { (void)ni; (void)buf; (void)count; (void)off; return -ENOSYS; }
ssize_t ntfs_write(ntfs_inode_t *ni, const void *buf, size_t count, uint64_t off) { (void)ni; (void)buf; (void)count; (void)off; return -ENOSYS; }
int ntfs_truncate(ntfs_inode_t *ni, uint64_t size) { (void)ni; (void)size; return -ENOSYS; }
int ntfs_fallocate(ntfs_inode_t *ni, uint64_t off, uint64_t len, bool keep) { (void)ni; (void)off; (void)len; (void)keep; return -ENOSYS; }
int ntfs_fsync(ntfs_inode_t *ni, bool datasync) { (void)ni; (void)datasync; return -ENOSYS; }
int ntfs_seek_data_hole(ntfs_inode_t *ni, uint64_t off, bool hole, uint64_t *res) { (void)ni; (void)off; (void)hole; *res = 0; return -ENOSYS; }

int ntfs_getxattr(ntfs_inode_t *ni, const char *name, void *buf, size_t size, size_t *len) { (void)ni; (void)name; (void)buf; (void)size; *len = 0; return -ENOSYS; }
int ntfs_setxattr(ntfs_inode_t *ni, const char *name, const void *buf, size_t size, int flags) { (void)ni; (void)name; (void)buf; (void)size; (void)flags; return -ENOSYS; }
int ntfs_removexattr(ntfs_inode_t *ni, const char *name) { (void)ni; (void)name; return -ENOSYS; }
int ntfs_listxattr(ntfs_inode_t *ni, char *buf, size_t size, size_t *len) { (void)ni; (void)buf; (void)size; *len = 0; return -ENOSYS; }

int ntfs_validate_name(ntfs_volume_t *vol, const char *name) { (void)vol; (void)name; return -ENOSYS; }
int ntfs_logfile_check(ntfs_volume_t *vol, bool *clean) { (void)vol; *clean = false; return -ENOSYS; }
int ntfs_volume_replay_journal(ntfs_volume_t *vol) { (void)vol; return -ENOSYS; }
int ntfs_logfile_analyse(struct ntfs_bdev *dev, struct ntfs_logfile_analysis *out) { (void)dev; (void)out; return -ENOSYS; }
int ntfs_logfile_replay_device(struct ntfs_bdev *dev, struct ntfs_logfile_analysis *out) { (void)dev; (void)out; return -ENOSYS; }
