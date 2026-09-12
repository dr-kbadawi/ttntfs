// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NTFS kernel file operations.
 *
 * Copyright (c) 2001-2015 Anton Altaparmakov and Tuxera Inc.
 * Copyright (c) 2025 LG Electronics Co., Ltd.
 *
 * PORT: the kernel's file_operations / inode_operations (read_iter,
 * write_iter, direct I/O, mmap, llseek, splice, ioctl, fiemap, get_link)
 * are gone: file data goes from the run list to the device in api.c
 * (docs/PORTING.md §3, rule 3). What remains here is the NTFS logic of
 * fsync, setattr/truncate, fallocate and punch-hole, taking inodes instead
 * of files and dentries. iomap_zero_range() became ntfs_vfs_zero_range().
 */

#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/uio.h>
#include <linux/falloc.h>

#include "lcnalloc.h"
#include "ntfs.h"
#include "reparse.h"
#include "ea.h"
#include "bitmap.h"
#include "vfs.h"

/*
 * Trim preallocated space on file release.
 *
 * When the preallo_size mount option is set (default 64KB), writes extend
 * allocated_size and runlist in units of preallocated size to reduce
 * runlist merge overhead for small writes. This can leave
 * allocated_size > data_size if not all preallocated space is used.
 *
 * PORT: the kernel calls this from ->release(); the port calls it from
 * ntfs_inode_put() when the last handle goes away.
 *
 * Returns 0 on success, or negative error on failure.
 */
int ntfs_vfs_trim_prealloc(struct inode *vi)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_volume *vol = ni->vol;
	struct runlist_element *rl;
	s64 aligned_data_size;
	s64 vcn_ds, vcn_tr;
	ssize_t rc;
	int err = 0;

	if (NInoCompressed(ni) || !NInoNonResident(ni))
		return 0;

	inode_lock(vi);
	mutex_lock(&ni->mrec_lock);
	down_write(&ni->runlist.lock);

	aligned_data_size = round_up(ni->data_size, vol->cluster_size);
	if (aligned_data_size >= ni->allocated_size)
		goto out_unlock;

	vcn_ds = ntfs_bytes_to_cluster(vol, aligned_data_size);
	vcn_tr = -1;
	rc = ni->runlist.count - 2;
	rl = ni->runlist.rl;

	while (rc >= 0 && rl[rc].lcn == LCN_HOLE && vcn_ds <= rl[rc].vcn) {
		vcn_tr = rl[rc].vcn;
		rc--;
	}

	if (vcn_tr >= 0) {
		err = ntfs_rl_truncate_nolock(vol, &ni->runlist, vcn_tr);
		if (err) {
			kvfree(ni->runlist.rl);
			ni->runlist.rl = NULL;
			ntfs_error(vol->sb, "Preallocated block rollback failed");
		} else {
			ni->allocated_size = ntfs_cluster_to_bytes(vol, vcn_tr);
			err = ntfs_attr_update_mapping_pairs(ni, 0);
			if (err)
				ntfs_error(vol->sb,
					   "Failed to rollback mapping pairs for prealloc");
		}
	}

out_unlock:
	up_write(&ni->runlist.lock);
	mutex_unlock(&ni->mrec_lock);
	inode_unlock(vi);

	return err;
}

/*
 * ntfs_vfs_fsync - sync a file to disk
 * @vi:		inode to be synced
 * @datasync:	if non-zero only flush user data and not metadata
 *
 * Data integrity sync of a file to disk.  Used for fsync, fdatasync, and msync
 * system calls.  This function is inspired by fs/buffer.c::file_fsync().
 *
 * If @datasync is false, write the mft record and all associated extent mft
 * records as well as the $DATA attribute and then sync the block device.
 *
 * If @datasync is true and the attribute is non-resident, we skip the writing
 * of the mft record and all associated extent mft records (this might still
 * happen due to the write_inode_now() call).
 *
 * Also, if @datasync is true, we do not wait on the inode to be written out
 * but we always wait on the page cache pages to be written out.
 */
int ntfs_vfs_fsync(struct inode *vi, bool datasync)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_volume *vol = ni->vol;
	int err, ret = 0;
	struct inode *parent_vi, *ia_vi;
	struct ntfs_attr_search_ctx *ctx;

	ntfs_debug("Entering for inode 0x%llx.", ni->mft_no);

	if (NVolShutdown(vol))
		return -EIO;

	if (S_ISDIR(vi->i_mode) && vi->i_fop && vi->i_fop->fsync) {
		/* dir.c::ntfs_dir_fsync() through the directory file ops. */
		struct file file = { .f_inode = vi, .f_mapping = vi->i_mapping };

		return vi->i_fop->fsync(&file, 0, LLONG_MAX, datasync);
	}

	err = filemap_write_and_wait_range(vi->i_mapping, 0, LLONG_MAX);
	if (err)
		return err;

	if (!datasync || !NInoNonResident(NTFS_I(vi)))
		ret = __ntfs_write_inode(vi, 1);
	write_inode_now(vi, !datasync);

	ctx = ntfs_attr_get_search_ctx(ni, NULL);
	if (!ctx)
		return -ENOMEM;

	mutex_lock_nested(&ni->mrec_lock, NTFS_INODE_MUTEX_NORMAL_CHILD);
	while (!(err = ntfs_attr_lookup(AT_UNUSED, NULL, 0, 0, 0, NULL, 0, ctx))) {
		if (ctx->attr->type == AT_FILE_NAME) {
			struct file_name_attr *fn = (struct file_name_attr *)((u8 *)ctx->attr +
					le16_to_cpu(ctx->attr->data.resident.value_offset));

			parent_vi = ntfs_iget(vi->i_sb, MREF_LE(fn->parent_directory));
			if (IS_ERR(parent_vi))
				continue;
			mutex_lock_nested(&NTFS_I(parent_vi)->mrec_lock, NTFS_INODE_MUTEX_NORMAL);
			ia_vi = ntfs_index_iget(parent_vi, I30, 4);
			mutex_unlock(&NTFS_I(parent_vi)->mrec_lock);
			if (IS_ERR(ia_vi)) {
				iput(parent_vi);
				continue;
			}
			write_inode_now(ia_vi, 1);
			iput(ia_vi);
			write_inode_now(parent_vi, 1);
			iput(parent_vi);
		} else if (ctx->attr->non_resident) {
			struct inode *attr_vi;
			__le16 *name;

			name = (__le16 *)((u8 *)ctx->attr + le16_to_cpu(ctx->attr->name_offset));
			if (ctx->attr->type == AT_DATA && ctx->attr->name_length == 0)
				continue;

			attr_vi = ntfs_attr_iget(vi, ctx->attr->type,
						 name, ctx->attr->name_length);
			if (IS_ERR(attr_vi))
				continue;
			spin_lock(&attr_vi->i_lock);
			if (inode_state_read_once(attr_vi) & I_DIRTY_PAGES) {
				spin_unlock(&attr_vi->i_lock);
				filemap_write_and_wait(attr_vi->i_mapping);
			} else
				spin_unlock(&attr_vi->i_lock);
			iput(attr_vi);
		}
	}
	mutex_unlock(&ni->mrec_lock);
	ntfs_attr_put_search_ctx(ctx);

	write_inode_now(vol->mftbmp_ino, 1);
	down_write(&vol->lcnbmp_lock);
	write_inode_now(vol->lcnbmp_ino, 1);
	up_write(&vol->lcnbmp_lock);
	write_inode_now(vol->mft_ino, 1);

	/*
	 * NOTE: If we were to use mapping->private_list (see ext2 and
	 * fs/buffer.c) for dirty blocks then we could optimize the below to be
	 * sync_mapping_buffers(vi->i_mapping).
	 */
	err = sync_blockdev(vi->i_sb->s_bdev);
	if (unlikely(err && !ret))
		ret = err;
	if (likely(!ret))
		ntfs_debug("Done.");
	else
		ntfs_warning(vi->i_sb,
				"Failed to f%ssync inode 0x%llx.  Error %u.",
				datasync ? "data" : "", ni->mft_no, -ret);
	if (!ret)
		blkdev_issue_flush(vi->i_sb->s_bdev);
	return ret;
}

static int ntfs_setattr_size(struct inode *vi, struct iattr *attr)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	int err;
	loff_t old_size = vi->i_size;

	if (NInoCompressed(ni) || NInoEncrypted(ni)) {
		ntfs_warning(vi->i_sb,
			"Changes in inode size are not supported yet for %s files, ignoring.",
			NInoCompressed(ni) ? "compressed" : "encrypted");
		return -EOPNOTSUPP;
	}

	err = inode_newsize_ok(vi, attr->ia_size);
	if (err)
		return err;

	/* PORT: flush cached edge pages before the size changes under them. */
	if (attr->ia_size < old_size)
		filemap_write_and_wait(vi->i_mapping);
	truncate_setsize(vi, attr->ia_size);
	err = ntfs_truncate_vfs(vi, attr->ia_size, old_size);
	if (err) {
		i_size_write(vi, old_size);
		return err;
	}

	if (NInoNonResident(ni) && attr->ia_size > old_size &&
	    old_size % PAGE_SIZE != 0) {
		loff_t len = min_t(loff_t,
				round_up(old_size, PAGE_SIZE) - old_size,
				attr->ia_size - old_size);
		err = ntfs_vfs_zero_range(vi, old_size, len);
	}

	return err;
}

/*
 * ntfs_vfs_setattr
 *
 * PORT: the kernel's ntfs_setattr() minus setattr_prepare() (permission
 * checks belong to the caller) and POSIX ACL mode propagation.
 * setattr_copy() is inlined.
 *
 * NOTE: Changes in inode size are not supported yet for compressed or
 * encrypted files.
 */
int ntfs_vfs_setattr(struct inode *vi, struct iattr *attr)
{
	int err = 0;
	unsigned int ia_valid = attr->ia_valid;
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_volume *vol = ni->vol;

	if (NVolShutdown(vol))
		return -EIO;
	if (IS_RDONLY(vi))
		return -EROFS;

	if (!(vol->vol_flags & VOLUME_IS_DIRTY))
		ntfs_set_volume_flags(vol, VOLUME_IS_DIRTY);

	if (ia_valid & ATTR_SIZE) {
		err = ntfs_setattr_size(vi, attr);
		if (err)
			goto out;

		ia_valid |= ATTR_MTIME | ATTR_CTIME;
		attr->ia_mtime = attr->ia_ctime = current_time(vi);
	}

	/* setattr_copy() */
	if (ia_valid & ATTR_UID)
		vi->i_uid = attr->ia_uid;
	if (ia_valid & ATTR_GID)
		vi->i_gid = attr->ia_gid;
	if (ia_valid & ATTR_ATIME)
		inode_set_atime_to_ts(vi, attr->ia_atime);
	if (ia_valid & ATTR_MTIME)
		inode_set_mtime_to_ts(vi, attr->ia_mtime);
	if (ia_valid & ATTR_CTIME)
		inode_set_ctime_to_ts(vi, attr->ia_ctime);
	if (ia_valid & ATTR_MODE)
		vi->i_mode = (vi->i_mode & S_IFMT) | (attr->ia_mode & ~S_IFMT);

	if (ia_valid & ATTR_MODE) {
		if (0222 & vi->i_mode)
			ni->flags &= ~FILE_ATTR_READONLY;
		else
			ni->flags |= FILE_ATTR_READONLY;
	}

	if (ia_valid & (ATTR_UID | ATTR_GID | ATTR_MODE)) {
		unsigned int flags = 0;

		if (ia_valid & ATTR_UID)
			flags |= NTFS_EA_UID;
		if (ia_valid & ATTR_GID)
			flags |= NTFS_EA_GID;
		if (ia_valid & ATTR_MODE)
			flags |= NTFS_EA_MODE;

		if (S_ISDIR(vi->i_mode))
			vi->i_mode &= ~vol->dmask;
		else
			vi->i_mode &= ~vol->fmask;

		mutex_lock(&ni->mrec_lock);
		ntfs_ea_set_wsl_inode(vi, 0, NULL, flags);
		mutex_unlock(&ni->mrec_lock);
	}

	if (ia_valid & (ATTR_SIZE | ATTR_MTIME | ATTR_CTIME | ATTR_ATIME))
		NInoSetFileNameDirty(ni);
	mark_inode_dirty(vi);
out:
	return err;
}

static int ntfs_allocate_range(struct ntfs_inode *ni, int mode, loff_t offset,
		loff_t len)
{
	struct inode *vi = VFS_I(ni);
	struct ntfs_volume *vol = ni->vol;
	s64 need_space;
	loff_t old_size, new_size;
	s64 start_vcn, end_vcn;
	int err;

	old_size = i_size_read(vi);
	new_size = max_t(loff_t, old_size, offset + len);
	start_vcn = ntfs_bytes_to_cluster(vol, offset);
	end_vcn = ntfs_bytes_to_cluster(vol, offset + len - 1) + 1;

	err = inode_newsize_ok(vi, new_size);
	if (err)
		goto out;

	need_space = ntfs_bytes_to_cluster(vol, ni->allocated_size);
	if (need_space > start_vcn)
		need_space = end_vcn - need_space;
	else
		need_space = end_vcn - start_vcn;
	if (need_space > 0 &&
	    need_space > (atomic64_read(&vol->free_clusters) -
			  atomic64_read(&vol->dirty_clusters))) {
		err = -ENOSPC;
		goto out;
	}

	err = ntfs_attr_fallocate(ni, offset, len,
			mode & FALLOC_FL_KEEP_SIZE ? true : false);

	if (!(mode & FALLOC_FL_KEEP_SIZE) && new_size != old_size)
		i_size_write(vi, ni->data_size);
out:
	return err;
}

static int ntfs_punch_hole(struct ntfs_inode *ni, int mode, loff_t offset,
		loff_t len)
{
	struct ntfs_volume *vol = ni->vol;
	struct inode *vi = VFS_I(ni);
	loff_t end_offset;
	s64 start_vcn, end_vcn;
	int err = 0;

	loff_t offset_down = round_down(offset, max_t(unsigned int,
				vol->cluster_size, PAGE_SIZE));

	if (NVolDisableSparse(vol)) {
		err = -EOPNOTSUPP;
		goto out;
	}

	if (offset >= ni->data_size)
		goto out;

	if (offset + len > ni->data_size)
		end_offset = ni->data_size;
	else
		end_offset = offset + len;

	err = filemap_write_and_wait_range(vi->i_mapping, offset_down, LLONG_MAX);
	if (err)
		goto out;
	truncate_pagecache(vi, offset_down);

	start_vcn = ntfs_bytes_to_cluster(vol, offset);
	end_vcn = ntfs_bytes_to_cluster(vol, end_offset - 1) + 1;

	if (offset & vol->cluster_size_mask) {
		if (offset < ni->initialized_size) {
			loff_t to;

			to = min_t(loff_t,
				   ntfs_cluster_to_bytes(vol, start_vcn + 1),
				   end_offset);
			err = ntfs_vfs_zero_range(vi, offset, to - offset);
			if (err < 0)
				goto out;
		}
		if (end_vcn - start_vcn == 1)
			goto out;
		start_vcn++;
	}

	if (end_offset & vol->cluster_size_mask) {
		loff_t from;

		from = ntfs_cluster_to_bytes(vol, end_vcn - 1);
		if (from < ni->initialized_size) {
			err = ntfs_vfs_zero_range(vi, from, end_offset - from);
			if (err < 0)
				goto out;
		}
		if (end_vcn - start_vcn == 1)
			goto out;
		end_vcn--;
	}

	mutex_lock_nested(&ni->mrec_lock, NTFS_INODE_MUTEX_NORMAL);
	err = ntfs_non_resident_attr_punch_hole(ni, start_vcn,
			end_vcn - start_vcn);
	mutex_unlock(&ni->mrec_lock);
out:
	return err;
}

#define NTFS_FALLOC_FL_SUPPORTED					\
		(FALLOC_FL_ALLOCATE_RANGE | FALLOC_FL_KEEP_SIZE |	\
		 FALLOC_FL_PUNCH_HOLE)

/*
 * ntfs_vfs_fallocate - preallocate or punch a hole
 * @mode:	FALLOC_FL_KEEP_SIZE and/or FALLOC_FL_PUNCH_HOLE (Linux values)
 *
 * PORT: collapse/insert range are not part of the port's ABI and were
 * dropped. Locking: caller holds i_rwsem.
 */
int ntfs_vfs_fallocate(struct inode *vi, int mode, loff_t offset, loff_t len)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	struct ntfs_volume *vol = ni->vol;
	int err = 0;
	loff_t old_size;
	bool map_locked = false;

	if (mode & ~(NTFS_FALLOC_FL_SUPPORTED))
		return -EOPNOTSUPP;
	if (NVolShutdown(vol))
		return -EIO;
	if (IS_RDONLY(vi))
		return -EROFS;
	if (!S_ISREG(vi->i_mode))
		return -EINVAL;

	if (!NVolFreeClusterKnown(vol))
		wait_event(vol->free_waitq, NVolFreeClusterKnown(vol));

	if ((ni->vol->mft_zone_end - ni->vol->mft_zone_start) == 0)
		return -ENOSPC;

	if (NInoNonResident(ni) && !NInoFullyMapped(ni)) {
		down_write(&ni->runlist.lock);
		err = ntfs_attr_map_whole_runlist(ni);
		up_write(&ni->runlist.lock);
		if (err)
			return err;
	}

	if (!(vol->vol_flags & VOLUME_IS_DIRTY)) {
		err = ntfs_set_volume_flags(vol, VOLUME_IS_DIRTY);
		if (err)
			return err;
	}

	old_size = i_size_read(vi);

	if (NInoCompressed(ni) || NInoEncrypted(ni)) {
		err = -EOPNOTSUPP;
		goto out;
	}

	if (mode & FALLOC_FL_PUNCH_HOLE) {
		filemap_invalidate_lock(vi->i_mapping);
		map_locked = true;
		if (!(mode & FALLOC_FL_KEEP_SIZE)) {
			err = -EOPNOTSUPP;
			goto out;
		}
		err = ntfs_punch_hole(ni, mode, offset, len);
	} else {
		err = ntfs_allocate_range(ni, mode, offset, len);
	}

	if (err)
		goto out;

out:
	if (map_locked)
		filemap_invalidate_unlock(vi->i_mapping);
	if (!err) {
		if (mode == 0 && NInoNonResident(ni) &&
		    offset > old_size && old_size % PAGE_SIZE != 0) {
			loff_t zlen = min_t(loff_t,
					    round_up(old_size, PAGE_SIZE) - old_size,
					    offset - old_size);
			err = ntfs_vfs_zero_range(vi, old_size, zlen);
		}
		NInoSetFileNameDirty(ni);
		inode_set_mtime_to_ts(vi, inode_set_ctime_current(vi));
		mark_inode_dirty(vi);
	}

	return err;
}

/*
 * PORT: dir.c's ntfs_dir_ops table references ntfs_ioctl(); there are no
 * ioctls in the port.
 */
long ntfs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return -ENOTTY;
}

/* PORT: super.c installs these on $MFTMirr; no operations exist in the port. */
const struct file_operations ntfs_empty_file_ops = { 0 };
const struct inode_operations ntfs_empty_inode_ops = { 0 };
