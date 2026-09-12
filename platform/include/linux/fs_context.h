/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FS_CONTEXT_H
#define _LINUX_FS_CONTEXT_H
#include <linux/types.h>
#include <linux/fs.h>
enum fs_context_purpose { FS_CONTEXT_FOR_MOUNT, FS_CONTEXT_FOR_SUBMOUNT, FS_CONTEXT_FOR_RECONFIGURE };
enum fs_value_type { fs_value_is_undefined, fs_value_is_flag, fs_value_is_string, fs_value_is_blob, fs_value_is_filename, fs_value_is_file };
struct fs_parameter { const char *key; enum fs_value_type type:8; union { char *string; void *blob; }; size_t size; int dirfd; };
struct fs_context_operations;
struct fs_context {
	const struct fs_context_operations *ops;
	void *fs_private;
	void *sget_key;
	struct dentry *root;
	const char *source;
	void *s_fs_info;
	unsigned int sb_flags;
	unsigned int sb_flags_mask;
	unsigned int s_iflags;
	enum fs_context_purpose purpose:8;
	bool need_free;
	/* Port: the device to mount. The caller sets it before ->get_tree();
	 * get_tree_bdev() hands it to the new super_block as s_bdev. */
	struct ntfs_bdev *bdev;
};
struct fs_context_operations {
	void (*free)(struct fs_context *fc);
	int (*dup)(struct fs_context *fc, struct fs_context *src_fc);
	int (*parse_param)(struct fs_context *fc, struct fs_parameter *param);
	int (*parse_monolithic)(struct fs_context *fc, void *data);
	int (*get_tree)(struct fs_context *fc);
	int (*reconfigure)(struct fs_context *fc);
};
/* Allocate a super_block over fc->bdev (s_fs_info moves from fc to sb as
 * in the kernel), call fill_super, and on success set fc->root. On failure
 * the super_block is freed; fill_super is expected to have released
 * s_fs_info itself (super.c does). platform/src/fs_parser.c. */
int get_tree_bdev(struct fs_context *fc, int (*fill_super)(struct super_block *sb, struct fs_context *fc));
#define errorf(fc, fmt, ...) platform_log(PLATFORM_LOG_ERR, fmt, ##__VA_ARGS__)
#define errorfc(fc, fmt, ...) platform_log(PLATFORM_LOG_ERR, fmt, ##__VA_ARGS__)
#define warnf(fc, fmt, ...) platform_log(PLATFORM_LOG_WARN, fmt, ##__VA_ARGS__)
#define warnfc(fc, fmt, ...) platform_log(PLATFORM_LOG_WARN, fmt, ##__VA_ARGS__)
#define invalf(fc, fmt, ...) (platform_log(PLATFORM_LOG_ERR, fmt, ##__VA_ARGS__), -EINVAL)
#define invalfc(fc, fmt, ...) (platform_log(PLATFORM_LOG_ERR, fmt, ##__VA_ARGS__), -EINVAL)
#define infof(fc, fmt, ...) platform_log(PLATFORM_LOG_INFO, fmt, ##__VA_ARGS__)
#define infofc(fc, fmt, ...) platform_log(PLATFORM_LOG_INFO, fmt, ##__VA_ARGS__)
struct file_system_type {
	const char *name;
	int fs_flags;
	int (*init_fs_context)(struct fs_context *);
	const struct fs_parameter_spec *parameters;
	void (*kill_sb)(struct super_block *);
	void *owner;
};
#define FS_REQUIRES_DEV 1
#define FS_ALLOW_IDMAP 32
static inline int register_filesystem(struct file_system_type *t) { (void)t; return 0; }
static inline int unregister_filesystem(struct file_system_type *t) { (void)t; return 0; }
static inline void kill_block_super(struct super_block *sb) { (void)sb; }
#endif
