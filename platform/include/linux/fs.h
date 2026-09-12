/* SPDX-License-Identifier: GPL-2.0 */
/*
 * VFS data structures for the user-space port. CONTRACT HEADER.
 *
 * struct inode / struct super_block carry exactly the state the ported core
 * reads and writes. The inode table (iget/iput/ilookup) lives in
 * platform/src/inode.c; it is generic, the ntfs-specific parts are supplied
 * through super_operations by core/vfs.
 */
#ifndef _LINUX_FS_H
#define _LINUX_FS_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/mm_types.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/time64.h>
#include <linux/uidgid.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <dirent.h>	/* DT_* match Linux values */
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/pagemap.h>
/* additive (compat stream): full types for the super_operations table. */
#include <linux/writeback.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>

struct super_block;
struct inode;
struct dentry;
struct file;
struct kstat;
struct iattr;
struct mnt_idmap;
struct ntfs_bdev;
struct path;		/* additive: getattr prototype in core/ntfs/inode.h */

/* Mount flags (sb->s_flags). */
#define SB_RDONLY	(1 << 0)
#define SB_NOSUID	(1 << 1)
#define SB_NODEV	(1 << 2)
#define SB_NOEXEC	(1 << 3)
#define SB_SYNCHRONOUS	(1 << 4)
#define SB_NOATIME	(1 << 10)
#define SB_NODIRATIME	(1 << 11)
#define SB_SILENT	(1 << 15)	/* additive: fs_context sb_flags */
#define SB_ACTIVE	(1 << 30)
#define FSLABEL_MAX	256		/* additive: kernel <linux/fs.h> */

/* inode->i_flags */
#define S_SYNC		(1 << 0)
#define S_NOATIME	(1 << 1)
#define S_APPEND	(1 << 2)
#define S_IMMUTABLE	(1 << 3)
#define S_NOCMTIME	(1 << 7)
#define S_DIRSYNC	(1 << 6)
#define S_CASEFOLD	(1 << 15)

/* inode->i_state */
#define I_DIRTY_SYNC		(1 << 0)
#define I_DIRTY_DATASYNC	(1 << 1)
#define I_DIRTY_PAGES		(1 << 2)
#define I_NEW			(1 << 3)
#define I_WILL_FREE		(1 << 4)
#define I_FREEING		(1 << 5)
#define I_CLEAR			(1 << 6)
#define I_SYNC			(1 << 7)
#define I_DIRTY_TIME		(1 << 11)
#define I_CREATING		(1 << 15)	/* additive: set by new_inode() until unlock_new_inode() */
#define I_DIRTY (I_DIRTY_SYNC | I_DIRTY_DATASYNC | I_DIRTY_PAGES)
#define I_DIRTY_ALL (I_DIRTY | I_DIRTY_TIME)

struct inode {
	umode_t			i_mode;
	unsigned short		i_opflags;
	kuid_t			i_uid;
	kgid_t			i_gid;
	unsigned int		i_flags;

	const struct inode_operations *i_op;
	struct super_block	*i_sb;
	struct address_space	*i_mapping;
	struct address_space	i_data;

	unsigned long		i_ino;
	unsigned int		i_nlink;
	dev_t			i_rdev;
	loff_t			i_size;
	struct timespec64	i_atime;
	struct timespec64	i_mtime;
	struct timespec64	i_ctime;
	spinlock_t		i_lock;
	unsigned short		i_bytes;
	u8			i_blkbits;
	blkcnt_t		i_blocks;
	u64			i_version;

	unsigned long		i_state;
	struct rw_semaphore	i_rwsem;

	atomic_t		i_count;
	u32			i_generation;
	const struct file_operations *i_fop;
	void			*i_private;

	/* Inode table bookkeeping (platform/src/inode.c). */
	struct hlist_node	i_hash;
	struct list_head	i_sb_list;
	struct mutex		i_new_lock;	/* held while I_NEW */
	/* additive (compat stream): unreferenced-inode LRU, see inode.c */
	struct list_head	i_lru;
};

/* Member types follow the kernel so super.c's table initialises unchanged. */
struct super_operations {
	struct inode *(*alloc_inode)(struct super_block *sb);
	void (*free_inode)(struct inode *inode);
	void (*destroy_inode)(struct inode *inode);
	int (*write_inode)(struct inode *inode, struct writeback_control *wbc);
	int (*drop_inode)(struct inode *inode);
	void (*evict_inode)(struct inode *inode);
	void (*put_super)(struct super_block *sb);
	int (*sync_fs)(struct super_block *sb, int wait);
	int (*statfs)(struct dentry *dentry, struct kstatfs *kstatfs);
	/* additive (compat stream): used by super.c's table */
	void (*shutdown)(struct super_block *sb);
	int (*show_options)(struct seq_file *seq, struct dentry *root);
};

struct super_block {
	unsigned long		s_blocksize;
	unsigned char		s_blocksize_bits;
	loff_t			s_maxbytes;
	unsigned long		s_flags;
	unsigned long		s_magic;
	const struct super_operations *s_op;
	const struct xattr_handler * const *s_xattr;
	const void		*s_export_op;
	struct dentry		*s_root;
	void			*s_fs_info;
	u32			s_time_gran;
	errseq_t		s_wb_err;
	char			s_id[32];

	/* Block device: the port's vtable, not a kernel block_device. */
	struct ntfs_bdev	*s_bdev;

	/* Inode table (platform/src/inode.c). */
	spinlock_t		s_inode_list_lock;
	struct list_head	s_inodes;
	struct hlist_head	*s_inode_hash;
	unsigned int		s_inode_hash_bits;
	struct rw_semaphore	s_umount;
};

static inline bool sb_rdonly(const struct super_block *sb) { return sb->s_flags & SB_RDONLY; }

/* Size accessors. i_size is protected by i_lock on 64-bit here (trivial). */
static inline loff_t i_size_read(const struct inode *inode) { return READ_ONCE(inode->i_size); }
static inline void i_size_write(struct inode *inode, loff_t size) { WRITE_ONCE(inode->i_size, size); }

/* Time accessors (kernel >= 6.6 style). */
static inline struct timespec64 inode_get_atime(const struct inode *i) { return i->i_atime; }
static inline struct timespec64 inode_get_mtime(const struct inode *i) { return i->i_mtime; }
static inline struct timespec64 inode_get_ctime(const struct inode *i) { return i->i_ctime; }
static inline struct timespec64 inode_set_atime_to_ts(struct inode *i, struct timespec64 ts) { i->i_atime = ts; return ts; }
static inline struct timespec64 inode_set_mtime_to_ts(struct inode *i, struct timespec64 ts) { i->i_mtime = ts; return ts; }
static inline struct timespec64 inode_set_ctime_to_ts(struct inode *i, struct timespec64 ts) { i->i_ctime = ts; return ts; }
struct timespec64 current_time(struct inode *inode);
static inline struct timespec64 inode_set_ctime_current(struct inode *i) { return inode_set_ctime_to_ts(i, current_time(i)); }
static inline void inode_set_iversion(struct inode *i, u64 v) { i->i_version = v; }
static inline u64 inode_peek_iversion(const struct inode *i) { return i->i_version; }
static inline void inode_inc_iversion(struct inode *i) { i->i_version++; }

/* uid/gid accessors. */
static inline uid_t i_uid_read(const struct inode *i) { return i->i_uid.val; }
static inline gid_t i_gid_read(const struct inode *i) { return i->i_gid.val; }
static inline void i_uid_write(struct inode *i, uid_t uid) { i->i_uid.val = uid; }
static inline void i_gid_write(struct inode *i, gid_t gid) { i->i_gid.val = gid; }

/* Link count. */
static inline void set_nlink(struct inode *i, unsigned int n) { i->i_nlink = n; }
static inline void inc_nlink(struct inode *i) { i->i_nlink++; }
static inline void drop_nlink(struct inode *i) { if (i->i_nlink) i->i_nlink--; }
static inline void clear_nlink(struct inode *i) { i->i_nlink = 0; }

/* Locking. */
static inline void inode_lock(struct inode *i) { down_write(&i->i_rwsem); }
static inline void inode_unlock(struct inode *i) { up_write(&i->i_rwsem); }
static inline void inode_lock_shared(struct inode *i) { down_read(&i->i_rwsem); }
static inline void inode_unlock_shared(struct inode *i) { up_read(&i->i_rwsem); }
static inline int inode_trylock(struct inode *i) { return down_write_trylock(&i->i_rwsem); }
#define inode_lock_nested(i, subclass) inode_lock(i)
static inline unsigned long inode_state_read_once(const struct inode *i) { return READ_ONCE(i->i_state); }

/* Dirty state. mark_inode_dirty() records that the VFS inode fields changed;
 * the port flushes them through s_op->write_inode from pagecache_sync_sb()
 * and on iput of the last reference. */
void __mark_inode_dirty(struct inode *inode, int flags);
static inline void mark_inode_dirty(struct inode *i) { __mark_inode_dirty(i, I_DIRTY); }
static inline void mark_inode_dirty_sync(struct inode *i) { __mark_inode_dirty(i, I_DIRTY_SYNC); }
int write_inode_now(struct inode *inode, int sync);

/* Inode table. */
struct inode *new_inode(struct super_block *sb);
struct inode *iget5_locked(struct super_block *sb, unsigned long hashval,
			   int (*test)(struct inode *, void *),
			   int (*set)(struct inode *, void *), void *data);
struct inode *ilookup5(struct super_block *sb, unsigned long hashval,
		       int (*test)(struct inode *, void *), void *data);
struct inode *ilookup5_nowait(struct super_block *sb, unsigned long hashval,
			      int (*test)(struct inode *, void *), void *data);
struct inode *find_inode_nowait(struct super_block *sb, unsigned long hashval,
				int (*match)(struct inode *, unsigned long, void *), void *data);
/* additive (compat stream): Linux 7.1 spells the hash value u64, and mft.c's
 * match callback follows suit. The macro routes a u64 callback to this
 * variant and leaves other callers on the original; both are implemented. */
struct inode *find_inode_nowait_u64(struct super_block *sb, u64 hashval,
				    int (*match)(struct inode *, u64, void *), void *data);
#define find_inode_nowait(sb, hashval, match, data) _Generic((match), \
	int (*)(struct inode *, u64, void *): find_inode_nowait_u64, \
	default: find_inode_nowait)((sb), (hashval), (match), (data))
void unlock_new_inode(struct inode *inode);
void iget_failed(struct inode *inode);
void ihold(struct inode *inode);
struct inode *igrab(struct inode *inode);
void iput(struct inode *inode);
void insert_inode_hash(struct inode *inode);
void remove_inode_hash(struct inode *inode);
static inline bool inode_unhashed(const struct inode *inode) { return hlist_unhashed(&inode->i_hash); }	/* additive */
void inode_init_once(struct inode *inode);
void inode_init_owner(struct mnt_idmap *idmap, struct inode *inode,
		      const struct inode *dir, umode_t mode);
void clear_inode(struct inode *inode);
int generic_delete_inode(struct inode *inode);
int generic_drop_inode(struct inode *inode);
void truncate_inode_pages_final(struct address_space *mapping);
/* Walk and evict every inode of @sb (unmount). */
void evict_inodes(struct super_block *sb);
int sync_inodes_sb(struct super_block *sb);
int sync_filesystem(struct super_block *sb);

/* Super block lifecycle. */
struct super_block *sb_alloc(void);
void sb_free(struct super_block *sb);
int sb_set_blocksize(struct super_block *sb, int size);
int sb_min_blocksize(struct super_block *sb, int size);
/* additive (compat stream): the inode table keeps up to this many
 * unreferenced inodes cached per process; older ones are evicted. */
#ifndef NTFS_INODE_CACHE_MAX
#define NTFS_INODE_CACHE_MAX 4096
#endif
unsigned long inode_cache_count(void);	/* cached unreferenced inodes (tests) */

/* Error sequence helpers (errseq.h in the kernel). */
static inline int errseq_check(errseq_t *eseq, errseq_t since) { (void)eseq; (void)since; return 0; }
static inline errseq_t errseq_sample(errseq_t *eseq) { return *eseq; }
static inline int errseq_set(errseq_t *eseq, int err) { if (err && !*eseq) *eseq = (errseq_t)-err; return err; }

#include <linux/dcache.h>
#include <linux/rbtree.h>
#include <linux/uio.h>

/* FITRIM argument. */
struct fstrim_range { u64 start; u64 len; u64 minlen; };

/* Minimal struct file: the vfs layer builds one per open handle so the
 * core's readdir/write paths (which take a file) work unchanged. */
struct file {
	struct inode *f_inode;
	struct address_space *f_mapping;
	struct file_ra_state f_ra;
	loff_t f_pos;
	unsigned int f_flags;
	fmode_t f_mode;
	void *private_data;
};
static inline struct inode *file_inode(const struct file *f) { return f->f_inode; }
static inline int file_write_and_wait_range(struct file *f, loff_t s, loff_t e) { return filemap_write_and_wait_range(f->f_mapping, s, e); }

/* readdir callback protocol (kernel semantics). */
struct dir_context;
typedef bool (*filldir_t)(struct dir_context *, const char *, int, loff_t, u64, unsigned);
struct dir_context { filldir_t actor; loff_t pos; };
static inline bool dir_emit(struct dir_context *ctx, const char *name, int namelen, u64 ino, unsigned type)
{ return ctx->actor(ctx, name, namelen, ctx->pos, ino, type); }
static inline bool dir_emit_dot(struct file *file, struct dir_context *ctx)
{ return ctx->actor(ctx, ".", 1, ctx->pos, file->f_inode->i_ino, DT_DIR); }
bool dir_emit_dotdot(struct file *file, struct dir_context *ctx);	/* vfs: needs parent ino */
static inline bool dir_emit_dots(struct file *file, struct dir_context *ctx)
{
	if (ctx->pos == 0) { if (!dir_emit_dot(file, ctx)) return false; ctx->pos = 1; }
	if (ctx->pos == 1) { if (!dir_emit_dotdot(file, ctx)) return false; ctx->pos = 2; }
	return true;
}

/* Operation tables. The core still defines its tables (ntfs_dir_ops,
 * ntfs_sops); the vfs layer reaches the static functions through them. */
struct file_operations {
	loff_t (*llseek)(struct file *, loff_t, int);
	ssize_t (*read)(struct file *, char *, size_t, loff_t *);
	ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
	int (*iterate_shared)(struct file *, struct dir_context *);
	long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
	long (*compat_ioctl)(struct file *, unsigned int, unsigned long);
	int (*open)(struct inode *, struct file *);
	int (*release)(struct inode *, struct file *);
	int (*fsync)(struct file *, loff_t, loff_t, int datasync);
	int (*setlease)(struct file *, int, void **, void **);
	long (*fallocate)(struct file *, int, loff_t, loff_t);
	int (*flush)(struct file *, void *);
};
struct inode_operations {
	struct dentry *(*lookup)(struct inode *, struct dentry *, unsigned int);
	int (*create)(struct mnt_idmap *, struct inode *, struct dentry *, umode_t, bool);
	int (*link)(struct dentry *, struct inode *, struct dentry *);
	int (*unlink)(struct inode *, struct dentry *);
	int (*symlink)(struct mnt_idmap *, struct inode *, struct dentry *, const char *);
	int (*mkdir)(struct mnt_idmap *, struct inode *, struct dentry *, umode_t);
	int (*rmdir)(struct inode *, struct dentry *);
	int (*rename)(struct mnt_idmap *, struct inode *, struct dentry *, struct inode *, struct dentry *, unsigned int);
	int (*setattr)(struct mnt_idmap *, struct dentry *, struct iattr *);
	int (*getattr)(struct mnt_idmap *, const void *, struct kstat *, u32, unsigned int);
	ssize_t (*listxattr)(struct dentry *, char *, size_t);
	int (*fiemap)(struct inode *, void *, u64, u64);
	int (*update_time)(struct inode *, int);
	int (*permission)(struct mnt_idmap *, struct inode *, int);
	const char *(*get_link)(struct dentry *, struct inode *, void *);
};
struct export_operations { void *dummy; };
#define MAX_LFS_FILESIZE ((loff_t)0x7fffffffffffffffLL)
loff_t generic_file_llseek(struct file *file, loff_t offset, int whence);
ssize_t generic_read_dir(struct file *filp, char *buf, size_t siz, loff_t *ppos);
int generic_setlease(struct file *filp, int arg, void **flp, void **priv);
#define generic_file_open NULL
#define generic_file_fsync NULL
#define IS_RDONLY(inode) sb_rdonly((inode)->i_sb)
#define IS_IMMUTABLE(inode) ((inode)->i_flags & S_IMMUTABLE)
#define IS_APPEND(inode) ((inode)->i_flags & S_APPEND)
#define IS_NOCMTIME(inode) ((inode)->i_flags & S_NOCMTIME)
#define IS_SYNC(inode) ((inode)->i_flags & S_SYNC)
#define IS_DIRSYNC(inode) ((inode)->i_flags & (S_SYNC | S_DIRSYNC))
#define IS_NOATIME(inode) ((inode)->i_flags & S_NOATIME)
#define IS_CASEFOLDED(inode) ((inode)->i_flags & S_CASEFOLD)

/* Shutdown flags used by super.c. */
#define FS_SHUTDOWN_FLAGS_DEFAULT 0
#define FS_SHUTDOWN_FLAGS_LOGFLUSH 1
#define FS_SHUTDOWN_FLAGS_NOLOGFLUSH 2

#endif /* _LINUX_FS_H */
