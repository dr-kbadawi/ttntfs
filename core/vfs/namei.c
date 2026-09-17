// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NTFS kernel directory inode operations.
 *
 * Copyright (c) 2001-2006 Anton Altaparmakov
 * Copyright (c) 2025 LG Electronics Co., Ltd.
 */

#include <linux/iversion.h>
#include <linux/bio.h>	/* PORT: before ntfs.h */

#include "ntfs.h"
#include "vfs.h"	/* PORT */
#include "time.h"
#include "index.h"
#include "reparse.h"
#include "object_id.h"
#include "ea.h"

static const __le16 aux_name_le[3] = {
	cpu_to_le16('A'), cpu_to_le16('U'), cpu_to_le16('X')
};

static const __le16 con_name_le[3] = {
	cpu_to_le16('C'), cpu_to_le16('O'), cpu_to_le16('N')
};

static const __le16 com_name_le[3] = {
	cpu_to_le16('C'), cpu_to_le16('O'), cpu_to_le16('M')
};

static const __le16 lpt_name_le[3] = {
	cpu_to_le16('L'), cpu_to_le16('P'), cpu_to_le16('T')
};

static const __le16 nul_name_le[3] = {
	cpu_to_le16('N'), cpu_to_le16('U'), cpu_to_le16('L')
};

static const __le16 prn_name_le[3] = {
	cpu_to_le16('P'), cpu_to_le16('R'), cpu_to_le16('N')
};

static inline int ntfs_check_bad_char(const __le16 *wc, unsigned int wc_len)
{
	int i;

	for (i = 0; i < wc_len; i++) {
		u16 c = le16_to_cpu(wc[i]);

		if (c < 0x0020 ||
		    c == 0x0022 || c == 0x002A || c == 0x002F ||
		    c == 0x003A || c == 0x003C || c == 0x003E ||
		    c == 0x003F || c == 0x005C || c == 0x007C)
			return -EINVAL;
	}

	return 0;
}

int ntfs_check_bad_windows_name(struct ntfs_volume *vol,
				const __le16 *wc,
				unsigned int wc_len)
{
	if (ntfs_check_bad_char(wc, wc_len))
		return -EINVAL;

	if (!NVolCheckWindowsNames(vol))
		return 0;

	/* Check for trailing space or dot. */
	if (wc_len > 0 &&
	    (wc[wc_len - 1] == cpu_to_le16(' ') ||
	    wc[wc_len - 1] == cpu_to_le16('.')))
		return -EINVAL;

	if (wc_len == 3 || (wc_len > 3 && wc[3] == cpu_to_le16('.'))) {
		__le16 *upcase = vol->upcase;
		u32 size = vol->upcase_len;

		if (ntfs_are_names_equal(wc, 3, aux_name_le, 3, IGNORE_CASE, upcase, size) ||
		    ntfs_are_names_equal(wc, 3, con_name_le, 3, IGNORE_CASE, upcase, size) ||
		    ntfs_are_names_equal(wc, 3, nul_name_le, 3, IGNORE_CASE, upcase, size) ||
		    ntfs_are_names_equal(wc, 3, prn_name_le, 3, IGNORE_CASE, upcase, size))
			return -EINVAL;
	}

	if (wc_len == 4 || (wc_len > 4 && wc[4] == cpu_to_le16('.'))) {
		__le16 *upcase = vol->upcase;
		u32 size = vol->upcase_len, port;

		if (ntfs_are_names_equal(wc, 3, com_name_le, 3, IGNORE_CASE, upcase, size) ||
		    ntfs_are_names_equal(wc, 3, lpt_name_le, 3, IGNORE_CASE, upcase, size)) {
			port = le16_to_cpu(wc[3]);
			if (port >= '1' && port <= '9')
				return -EINVAL;
		}
	}
	return 0;
}

/*
 * ntfs_vfs_lookup - find the inode of a name in a directory inode
 * @dir_ino:	directory inode in which to look for the inode
 * @name:	name (UTF-8, not NUL-terminated) to look for
 * @len:	length of @name in bytes
 *
 * PORT: the dentry based ntfs_lookup() and its dcache aliasing logic (cases
 * 2 and 3 in the kernel) are gone; there is no dcache. The name is converted
 * to Unicode, the directory index is searched (case sensitivity per the
 * volume flags) and the matching inode is loaded with ntfs_iget().
 *
 * Return the referenced inode, ERR_PTR(-ENOENT) if the name does not exist,
 * or another ERR_PTR() on error.
 */
struct inode *ntfs_vfs_lookup(struct inode *dir_ino, const char *name, int len)
{
	struct ntfs_volume *vol = NTFS_SB(dir_ino->i_sb);
	struct inode *dent_inode;
	__le16 *uname;
	struct ntfs_name *nm = NULL;
	u64 mref;
	unsigned long dent_ino;
	int uname_len;

	ntfs_debug("Looking up %.*s in directory inode 0x%llx.", len, name,
			NTFS_I(dir_ino)->mft_no);
	/* Convert the name to Unicode. */
	uname_len = ntfs_nlstoucs(vol, name, len, &uname, NTFS_MAX_NAME_LEN);
	if (uname_len < 0) {
		if (uname_len != -ENAMETOOLONG)
			ntfs_debug("Failed to convert name to Unicode.");
		return ERR_PTR(uname_len);
	}
	mutex_lock(&NTFS_I(dir_ino)->mrec_lock);
	mref = ntfs_lookup_inode_by_name(NTFS_I(dir_ino), uname, uname_len,
			&nm);
	mutex_unlock(&NTFS_I(dir_ino)->mrec_lock);
	kmem_cache_free(ntfs_name_cache, uname);
	/* The properly cased / long name is only needed for a dcache. */
	kfree(nm);
	if (IS_ERR_MREF(mref)) {
		if (MREF_ERR(mref) == -ENOENT) {
			ntfs_debug("Entry was not found.");
			return ERR_PTR(-ENOENT);
		}
		ntfs_error(vol->sb, "ntfs_lookup_ino_by_name() failed with error code %i.",
				-MREF_ERR(mref));
		return ERR_PTR(MREF_ERR(mref));
	}
	dent_ino = MREF(mref);
	ntfs_debug("Found inode 0x%lx. Calling ntfs_iget.", dent_ino);
	dent_inode = ntfs_iget(vol->sb, dent_ino);
	if (IS_ERR(dent_inode)) {
		ntfs_error(vol->sb, "ntfs_iget(0x%lx) failed with error code %li.",
				dent_ino, PTR_ERR(dent_inode));
		return dent_inode;
	}
	/* Consistency check. */
	if (MSEQNO(mref) != NTFS_I(dent_inode)->seq_no && dent_ino != FILE_MFT) {
		ntfs_error(vol->sb,
			"Found stale reference to inode 0x%lx (reference sequence number = 0x%x, inode sequence number = 0x%x), returning -EIO. Run chkdsk.",
			dent_ino, MSEQNO(mref),
			NTFS_I(dent_inode)->seq_no);
		iput(dent_inode);
		return ERR_PTR(-EIO);
	}
	ntfs_debug("Done.");
	return dent_inode;
}

static int ntfs_sd_add_everyone(struct ntfs_inode *ni)
{
	struct security_descriptor_relative *sd;
	struct ntfs_acl *acl;
	struct ntfs_ace *ace;
	struct ntfs_sid *sid;
	int ret, sd_len;

	/* Create SECURITY_DESCRIPTOR attribute (everyone has full access). */
	/*
	 * Calculate security descriptor length. We have 2 sub-authorities in
	 * owner and group SIDs, So add 8 bytes to every SID.
	 */
	sd_len = sizeof(struct security_descriptor_relative) + 2 *
		(sizeof(struct ntfs_sid) + 8) + sizeof(struct ntfs_acl) +
		sizeof(struct ntfs_ace) + 4;
	sd = kzalloc(sd_len, GFP_NOFS);
	if (!sd)
		return -ENOMEM;

	sd->revision = 1;
	sd->control = SE_DACL_PRESENT | SE_SELF_RELATIVE;

	sid = (struct ntfs_sid *)((u8 *)sd + sizeof(struct security_descriptor_relative));
	sid->revision = 1;
	sid->sub_authority_count = 2;
	sid->sub_authority[0] = cpu_to_le32(SECURITY_BUILTIN_DOMAIN_RID);
	sid->sub_authority[1] = cpu_to_le32(DOMAIN_ALIAS_RID_ADMINS);
	sid->identifier_authority.value[5] = 5;
	sd->owner = cpu_to_le32((u8 *)sid - (u8 *)sd);

	sid = (struct ntfs_sid *)((u8 *)sid + sizeof(struct ntfs_sid) + 8);
	sid->revision = 1;
	sid->sub_authority_count = 2;
	sid->sub_authority[0] = cpu_to_le32(SECURITY_BUILTIN_DOMAIN_RID);
	sid->sub_authority[1] = cpu_to_le32(DOMAIN_ALIAS_RID_ADMINS);
	sid->identifier_authority.value[5] = 5;
	sd->group = cpu_to_le32((u8 *)sid - (u8 *)sd);

	acl = (struct ntfs_acl *)((u8 *)sid + sizeof(struct ntfs_sid) + 8);
	acl->revision = 2;
	acl->size = cpu_to_le16(sizeof(struct ntfs_acl) + sizeof(struct ntfs_ace) + 4);
	acl->ace_count = cpu_to_le16(1);
	sd->dacl = cpu_to_le32((u8 *)acl - (u8 *)sd);

	ace = (struct ntfs_ace *)((u8 *)acl + sizeof(struct ntfs_acl));
	ace->type = ACCESS_ALLOWED_ACE_TYPE;
	ace->flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
	ace->size = cpu_to_le16(sizeof(struct ntfs_ace) + 4);
	ace->mask = cpu_to_le32(0x1f01ff);
	ace->sid.revision = 1;
	ace->sid.sub_authority_count = 1;
	ace->sid.sub_authority[0] = 0;
	ace->sid.identifier_authority.value[5] = 1;

	ret = ntfs_attr_add(ni, AT_SECURITY_DESCRIPTOR, AT_UNNAMED, 0, (u8 *)sd,
			sd_len);
	if (ret)
		ntfs_error(ni->vol->sb, "Failed to add SECURITY_DESCRIPTOR\n");

	kfree(sd);
	return ret;
}

static struct ntfs_inode *__ntfs_create(struct mnt_idmap *idmap, struct inode *dir,
		__le16 *name, u8 name_len, mode_t mode, dev_t dev,
		__le16 *target, int target_len)
{
	struct ntfs_inode *dir_ni = NTFS_I(dir);
	struct ntfs_volume *vol = dir_ni->vol;
	struct ntfs_inode *ni;
	bool rollback_data = false, rollback_sd = false, rollback_reparse = false;
	struct file_name_attr *fn = NULL;
	struct standard_information *si = NULL;
	int err = 0, fn_len, si_len;
	struct inode *vi;
	struct mft_record *ni_mrec, *dni_mrec;
	struct super_block *sb = dir_ni->vol->sb;
	__le64 parent_mft_ref;
	u64 child_mft_ref;
	__le16 ea_size;

	vi = new_inode(vol->sb);
	if (!vi)
		return ERR_PTR(-ENOMEM);

	ntfs_init_big_inode(vi);
	ni = NTFS_I(vi);
	ni->vol = dir_ni->vol;
	ni->name_len = 0;
	ni->name = NULL;

	/*
	 * Set the appropriate mode, attribute type, and name.  For
	 * directories, also setup the index values to the defaults.
	 */
	if (S_ISDIR(mode)) {
		mode &= ~vol->dmask;

		NInoSetMstProtected(ni);
		ni->itype.index.block_size = 4096;
		ni->itype.index.block_size_bits = ntfs_ffs(4096) - 1;
		ni->itype.index.collation_rule = COLLATION_FILE_NAME;
		if (vol->cluster_size <= ni->itype.index.block_size) {
			ni->itype.index.vcn_size = vol->cluster_size;
			ni->itype.index.vcn_size_bits =
				vol->cluster_size_bits;
		} else {
			ni->itype.index.vcn_size = vol->sector_size;
			ni->itype.index.vcn_size_bits =
				vol->sector_size_bits;
		}
	} else {
		mode &= ~vol->fmask;
	}

	if (IS_RDONLY(vi))
		mode &= ~0222;

	inode_init_owner(idmap, vi, dir, mode);

	mode = vi->i_mode;

	/* PORT: no POSIX ACLs, no S_NOSEC. */

	if (uid_valid(vol->uid))
		vi->i_uid = vol->uid;

	if (gid_valid(vol->gid))
		vi->i_gid = vol->gid;

	/*
	 * Set the file size to 0, the ntfs inode sizes are set to 0 by
	 * the call to ntfs_init_big_inode() below.
	 */
	vi->i_size = 0;
	vi->i_blocks = 0;

	inode_inc_iversion(vi);

	simple_inode_init_ts(vi);
	ni->i_crtime = inode_get_ctime(vi);

	inode_set_mtime_to_ts(dir, ni->i_crtime);
	inode_set_ctime_to_ts(dir, ni->i_crtime);
	mark_inode_dirty(dir);

	err = ntfs_mft_record_alloc(dir_ni->vol, mode, &ni, NULL,
				    &ni_mrec);
	if (err) {
		iput(vi);
		return ERR_PTR(err);
	}

	/*
	 * Prevent iget and writeback from finding this inode.
	 * Caller must call d_instantiate_new instead of d_instantiate.
	 */
	ntfs_vfs_mark_new(vi);	/* PORT */

	/* Add the inode to the inode hash for the superblock. */
	vi->i_ino = (unsigned long)ni->mft_no;
	inode_set_iversion(vi, 1);
	insert_inode_hash(vi);

	mutex_lock_nested(&ni->mrec_lock, NTFS_INODE_MUTEX_NORMAL);
	mutex_lock_nested(&dir_ni->mrec_lock, NTFS_INODE_MUTEX_PARENT);
	if (NInoBeingDeleted(dir_ni)) {
		err = -ENOENT;
		goto err_out;
	}

	dni_mrec = map_mft_record(dir_ni);
	if (IS_ERR(dni_mrec)) {
		ntfs_error(dir_ni->vol->sb, "failed to map mft record for file 0x%llx.\n",
			   dir_ni->mft_no);
		err = -EIO;
		goto err_out;
	}
	parent_mft_ref = MK_LE_MREF(dir_ni->mft_no,
				    le16_to_cpu(dni_mrec->sequence_number));
	unmap_mft_record(dir_ni);

	/*
	 * Create STANDARD_INFORMATION attribute. Write STANDARD_INFORMATION
	 * version 1.2, windows will upgrade it to version 3 if needed.
	 */
	si_len = offsetof(struct standard_information, file_attributes) +
		sizeof(__le32) + 12;
	si = kzalloc(si_len, GFP_NOFS);
	if (!si) {
		err = -ENOMEM;
		goto err_out;
	}

	si->creation_time = si->last_data_change_time = utc2ntfs(ni->i_crtime);
	si->last_mft_change_time = si->last_access_time = si->creation_time;

	/*
	 * PORT: upstream marks every non-regular, non-directory inode as a system
	 * file. We exclude symlinks.
	 *
	 * FILE_ATTR_SYSTEM hides a file from `dir` and from Explorer unless the
	 * user has turned on "show protected operating system files". That is
	 * defensible for a device node or a FIFO, which Windows can do nothing
	 * with. A symlink is not in that category: Windows has its own symlinks,
	 * lists them in `dir` as <SYMLINK>, and a user who copied a tree
	 * containing one expects to see it.
	 *
	 * Measured on Windows 10, 2026-09-17 (finding 20): a directory holding two
	 * of our symlinks showed 4 entries under `dir` and 6 under `dir /a`. The
	 * links were simply invisible.
	 *
	 * This does not make them followable -- they carry the WSL reparse tag,
	 * which native Windows declines to follow. That is a separate decision
	 * recorded in the same finding. This change only stops us hiding them.
	 */
	if (!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode))
		si->file_attributes = FILE_ATTR_SYSTEM;

	/* Add STANDARD_INFORMATION to inode. */
	err = ntfs_attr_add(ni, AT_STANDARD_INFORMATION, AT_UNNAMED, 0, (u8 *)si,
			si_len);
	if (err) {
		ntfs_error(sb, "Failed to add STANDARD_INFORMATION attribute.\n");
		goto err_out;
	}

	err = ntfs_sd_add_everyone(ni);
	if (err)
		goto err_out;
	rollback_sd = true;

	if (S_ISDIR(mode)) {
		struct index_root *ir = NULL;
		struct index_entry *ie;
		int ir_len, index_len;

		/* Create struct index_root attribute. */
		index_len = sizeof(struct index_header) + sizeof(struct index_entry_header);
		ir_len = offsetof(struct index_root, index) + index_len;
		ir = kzalloc(ir_len, GFP_NOFS);
		if (!ir) {
			err = -ENOMEM;
			goto err_out;
		}
		ir->type = AT_FILE_NAME;
		ir->collation_rule = COLLATION_FILE_NAME;
		ir->index_block_size = cpu_to_le32(ni->vol->index_record_size);
		if (ni->vol->cluster_size <= ni->vol->index_record_size)
			ir->clusters_per_index_block =
				NTFS_B_TO_CLU(vol, ni->vol->index_record_size);
		else
			ir->clusters_per_index_block =
				ni->vol->index_record_size >> ni->vol->sector_size_bits;
		ir->index.entries_offset = cpu_to_le32(sizeof(struct index_header));
		ir->index.index_length = cpu_to_le32(index_len);
		ir->index.allocated_size = cpu_to_le32(index_len);
		ie = (struct index_entry *)((u8 *)ir + sizeof(struct index_root));
		ie->length = cpu_to_le16(sizeof(struct index_entry_header));
		ie->key_length = 0;
		ie->flags = INDEX_ENTRY_END;

		/* Add struct index_root attribute to inode. */
		err = ntfs_attr_add(ni, AT_INDEX_ROOT, I30, 4, (u8 *)ir, ir_len);
		if (err) {
			kfree(ir);
			ntfs_error(vi->i_sb, "Failed to add struct index_root attribute.\n");
			goto err_out;
		}
		kfree(ir);
		err = ntfs_attr_open(ni, AT_INDEX_ROOT, I30, 4);
		if (err)
			goto err_out;
	} else {
		/* Add DATA attribute to inode. */
		err = ntfs_attr_add(ni, AT_DATA, AT_UNNAMED, 0, NULL, 0);
		if (err) {
			ntfs_error(dir_ni->vol->sb, "Failed to add DATA attribute.\n");
			goto err_out;
		}
		rollback_data = true;

		err = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
		if (err)
			goto err_out;

		if (S_ISLNK(mode)) {
			err = ntfs_reparse_set_wsl_symlink(ni, target, target_len);
			if (!err)
				rollback_reparse = true;
		} else if (S_ISBLK(mode) || S_ISCHR(mode) || S_ISSOCK(mode) ||
			   S_ISFIFO(mode)) {
			si->file_attributes = FILE_ATTRIBUTE_RECALL_ON_OPEN;
			ni->flags = FILE_ATTRIBUTE_RECALL_ON_OPEN;
			err = ntfs_reparse_set_wsl_not_symlink(ni, mode);
			if (!err)
				rollback_reparse = true;
		}
		if (err)
			goto err_out;
	}

	err = ntfs_ea_set_wsl_inode(vi, dev, &ea_size,
			NTFS_EA_UID | NTFS_EA_GID | NTFS_EA_MODE);
	if (err)
		goto err_out;

	/* Create FILE_NAME attribute. */
	fn_len = sizeof(struct file_name_attr) + name_len * sizeof(__le16);
	fn = kzalloc(fn_len, GFP_NOFS);
	if (!fn) {
		err = -ENOMEM;
		goto err_out;
	}

	fn->file_attributes |= ni->flags;
	fn->parent_directory = parent_mft_ref;
	fn->file_name_length = name_len;
	fn->file_name_type = FILE_NAME_POSIX;
	fn->type.ea.packed_ea_size = ea_size;
	if (S_ISDIR(mode)) {
		fn->file_attributes = FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT;
		fn->allocated_size = fn->data_size = 0;
	} else {
		fn->data_size = cpu_to_le64(ni->data_size);
		fn->allocated_size = cpu_to_le64(ni->allocated_size);
	}
	/* PORT: same exclusion as the $STANDARD_INFORMATION copy above, and this
	 * is the one that matters -- `dir` reads the index entry's cached
	 * attributes, not the MFT record's. A symlink still gets
	 * FILE_ATTR_REPARSE_POINT, which is what makes Windows show it as a link;
	 * it just no longer gets FILE_ATTR_SYSTEM, which was hiding it. */
	if (!S_ISREG(mode) && !S_ISDIR(mode)) {
		if (!S_ISLNK(mode))
			fn->file_attributes = FILE_ATTR_SYSTEM;
		if (rollback_reparse)
			fn->file_attributes |= FILE_ATTR_REPARSE_POINT;
	}
	if (NVolHideDotFiles(vol) && name_len > 0 && name[0] == cpu_to_le16('.'))
		fn->file_attributes |= FILE_ATTR_HIDDEN;
	fn->creation_time = fn->last_data_change_time = utc2ntfs(ni->i_crtime);
	fn->last_mft_change_time = fn->last_access_time = fn->creation_time;
	memcpy(fn->file_name, name, name_len * sizeof(__le16));

	/* Add FILE_NAME attribute to inode. */
	err = ntfs_attr_add(ni, AT_FILE_NAME, AT_UNNAMED, 0, (u8 *)fn, fn_len);
	if (err) {
		ntfs_error(sb, "Failed to add FILE_NAME attribute.\n");
		goto err_out;
	}

	child_mft_ref = MK_MREF(ni->mft_no,
				le16_to_cpu(ni_mrec->sequence_number));
	/* Set hard links count and directory flag. */
	ni_mrec->link_count = cpu_to_le16(1);
	mark_mft_record_dirty(ni);

	/* Add FILE_NAME attribute to index. */
	err = ntfs_index_add_filename(dir_ni, fn, child_mft_ref);
	if (err) {
		ntfs_debug("Failed to add entry to the index");
		goto err_out;
	}

	unmap_mft_record(ni);
	mutex_unlock(&dir_ni->mrec_lock);
	mutex_unlock(&ni->mrec_lock);

	ni->flags = fn->file_attributes;
	/* Set the sequence number. */
	vi->i_generation = ni->seq_no;
	set_nlink(vi, 1);
	ntfs_set_vfs_operations(vi, mode, dev);

	/* Done! */
	kfree(fn);
	kfree(si);
	ntfs_debug("Done.\n");
	return ni;

err_out:
	if (rollback_sd)
		ntfs_attr_remove(ni, AT_SECURITY_DESCRIPTOR, AT_UNNAMED, 0);

	if (rollback_data)
		ntfs_attr_remove(ni, AT_DATA, AT_UNNAMED, 0);

	if (rollback_reparse)
		ntfs_delete_reparse_index(ni);
	/*
	 * Free extent MFT records (should not exist any with current
	 * ntfs_create implementation, but for any case if something will be
	 * changed in the future).
	 */
	while (ni->nr_extents != 0) {
		int err2;

		err2 = ntfs_mft_record_free(ni->vol, *(ni->ext.extent_ntfs_inos));
		if (err2)
			ntfs_error(sb,
				"Failed to free extent MFT record. Leaving inconsistent metadata.\n");
		ntfs_inode_close(*(ni->ext.extent_ntfs_inos));
	}
	if (ntfs_mft_record_free(ni->vol, ni))
		ntfs_error(sb,
			"Failed to free MFT record. Leaving inconsistent metadata. Run chkdsk.\n");
	unmap_mft_record(ni);
	kfree(fn);
	kfree(si);

	mutex_unlock(&dir_ni->mrec_lock);
	mutex_unlock(&ni->mrec_lock);

	/* PORT: discard_new_inode(): unhash, wake waiters, drop. i_nlink is
	 * still 1 so eviction does not touch the (already freed) record. */
	remove_inode_hash(vi);
	unlock_new_inode(vi);
	iput(vi);
	return ERR_PTR(err);
}

static int ntfs_check_unlinkable_dir(struct ntfs_attr_search_ctx *ctx, struct file_name_attr *fn)
{
	int link_count;
	int ret;
	struct ntfs_inode *ni = ctx->base_ntfs_ino ? ctx->base_ntfs_ino : ctx->ntfs_ino;
	struct mft_record *ni_mrec = ctx->base_mrec ? ctx->base_mrec : ctx->mrec;

	ret = ntfs_check_empty_dir(ni, ni_mrec);
	if (!ret || ret != -ENOTEMPTY)
		return ret;

	link_count = le16_to_cpu(ni_mrec->link_count);
	/*
	 * Directory is non-empty, so we can unlink only if there is more than
	 * one "real" hard link, i.e. links aren't different DOS and WIN32 names
	 */
	if ((link_count == 1) ||
	    (link_count == 2 && fn->file_name_type == FILE_NAME_DOS)) {
		ret = -ENOTEMPTY;
		ntfs_debug("Non-empty directory without hard links\n");
		goto no_hardlink;
	}

	ret = 0;
no_hardlink:
	return ret;
}

static int ntfs_test_inode_attr(struct inode *vi, void *data)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	u64 mft_no = (u64)(uintptr_t)data;

	if (ni->mft_no != mft_no)
		return 0;
	if (NInoAttr(ni) || ni->nr_extents == -1)
		return 1;
	else
		return 0;
}

/*
 * ntfs_delete - delete file or directory from ntfs volume
 * @ni:         ntfs inode for object to delte
 * @dir_ni:     ntfs inode for directory in which delete object
 * @name:       unicode name of the object to delete
 * @name_len:   length of the name in unicode characters
 * @need_lock:  whether mrec lock is needed or not
 *
 * Delete the specified name from the directory index @dir_ni and decrement
 * the link count of the target inode @ni.
 *
 * Return 0 on success and -errno on error.
 */
static int ntfs_delete(struct ntfs_inode *ni, struct ntfs_inode *dir_ni,
		__le16 *name, u8 name_len, bool need_lock)
{
	struct ntfs_attr_search_ctx *actx = NULL;
	struct file_name_attr *fn = NULL;
	bool looking_for_dos_name = false, looking_for_win32_name = false;
	bool case_sensitive_match = true;
	int err = 0;
	struct mft_record *ni_mrec;
	struct super_block *sb;
	bool link_count_zero = false;

	ntfs_debug("Entering.\n");

	if (need_lock == true) {
		mutex_lock_nested(&ni->mrec_lock, NTFS_INODE_MUTEX_NORMAL);
		mutex_lock_nested(&dir_ni->mrec_lock, NTFS_INODE_MUTEX_PARENT);
	}

	sb = dir_ni->vol->sb;

	if (ni->nr_extents == -1)
		ni = ni->ext.base_ntfs_ino;
	if (dir_ni->nr_extents == -1)
		dir_ni = dir_ni->ext.base_ntfs_ino;
	/*
	 * Search for FILE_NAME attribute with such name. If it's in POSIX or
	 * WIN32_AND_DOS namespace, then simply remove it from index and inode.
	 * If filename in DOS or in WIN32 namespace, then remove DOS name first,
	 * only then remove WIN32 name.
	 */
	actx = ntfs_attr_get_search_ctx(ni, NULL);
	if (!actx) {
		ntfs_error(sb, "%s, Failed to get search context", __func__);
		if (need_lock) {
			mutex_unlock(&dir_ni->mrec_lock);
			mutex_unlock(&ni->mrec_lock);
		}
		return -ENOMEM;
	}
search:
	while ((err = ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE,
				0, NULL, 0, actx)) == 0) {
#ifdef DEBUG
		unsigned char *s;
#endif
		bool case_sensitive = IGNORE_CASE;

		fn = (struct file_name_attr *)((u8 *)actx->attr +
				le16_to_cpu(actx->attr->data.resident.value_offset));
#ifdef DEBUG
		s = ntfs_attr_name_get(ni->vol, fn->file_name, fn->file_name_length);
		ntfs_debug("name: '%s'  type: %d  dos: %d  win32: %d case: %d\n",
				s, fn->file_name_type,
				looking_for_dos_name, looking_for_win32_name,
				case_sensitive_match);
		ntfs_attr_name_free(&s);
#endif
		if (looking_for_dos_name) {
			if (fn->file_name_type == FILE_NAME_DOS)
				break;
			continue;
		}
		if (looking_for_win32_name) {
			if  (fn->file_name_type == FILE_NAME_WIN32)
				break;
			continue;
		}

		/* Ignore hard links from other directories */
		if (dir_ni->mft_no != MREF_LE(fn->parent_directory)) {
			ntfs_debug("MFT record numbers don't match (%llu != %lu)\n",
					dir_ni->mft_no,
					MREF_LE(fn->parent_directory));
			continue;
		}

		if (fn->file_name_type == FILE_NAME_POSIX || case_sensitive_match)
			case_sensitive = CASE_SENSITIVE;

		if (ntfs_names_are_equal(fn->file_name, fn->file_name_length,
					name, name_len, case_sensitive,
					ni->vol->upcase, ni->vol->upcase_len)) {
			if (fn->file_name_type == FILE_NAME_WIN32) {
				looking_for_dos_name = true;
				ntfs_attr_reinit_search_ctx(actx);
				continue;
			}
			if (fn->file_name_type == FILE_NAME_DOS)
				looking_for_dos_name = true;
			break;
		}
	}
	if (err) {
		/*
		 * If case sensitive search failed, then try once again
		 * ignoring case.
		 */
		if (err == -ENOENT && case_sensitive_match) {
			case_sensitive_match = false;
			ntfs_attr_reinit_search_ctx(actx);
			goto search;
		}
		goto err_out;
	}

	err = ntfs_check_unlinkable_dir(actx, fn);
	if (err)
		goto err_out;

	err = ntfs_index_remove(dir_ni, fn, le32_to_cpu(actx->attr->data.resident.value_length));
	if (err)
		goto err_out;

	err = ntfs_attr_record_rm(actx);
	if (err)
		goto err_out;

	ni_mrec = actx->base_mrec ? actx->base_mrec : actx->mrec;
	ni_mrec->link_count = cpu_to_le16(le16_to_cpu(ni_mrec->link_count) - 1);
	if (!S_ISDIR(VFS_I(ni)->i_mode))
		drop_nlink(VFS_I(ni));

	mark_mft_record_dirty(ni);
	if (looking_for_dos_name) {
		looking_for_dos_name = false;
		looking_for_win32_name = true;
		ntfs_attr_reinit_search_ctx(actx);
		goto search;
	}

	/*
	 * For directories, Drop VFS nlink only when mft record link count
	 * becomes zero. Because we fixes VFS nlink to 1 for directories.
	 */
	if (S_ISDIR(VFS_I(ni)->i_mode) && !le16_to_cpu(ni_mrec->link_count))
		drop_nlink(VFS_I(ni));

	/*
	 * If hard link count is not equal to zero then we are done. In other
	 * case there are no reference to this inode left, so we should free all
	 * non-resident attributes and mark all MFT record as not in use.
	 */
	if (ni_mrec->link_count == 0) {
		NInoSetBeingDeleted(ni);
		ntfs_delete_reparse_index(ni);
		ntfs_delete_object_id_index(ni);
		link_count_zero = true;
	}

	ntfs_attr_put_search_ctx(actx);
	if (need_lock == true) {
		mutex_unlock(&dir_ni->mrec_lock);
		mutex_unlock(&ni->mrec_lock);
	}

	/*
	 * If hard link count is not equal to zero then we are done. In other
	 * case there are no reference to this inode left, so we should free all
	 * non-resident attributes and mark all MFT record as not in use.
	 */
	if (link_count_zero == true) {
		struct inode *attr_vi;

		while ((attr_vi = ilookup5(sb, ni->mft_no, ntfs_test_inode_attr,
					   (void *)(uintptr_t)ni->mft_no)) != NULL) {
			clear_nlink(attr_vi);
			iput(attr_vi);
		}
	}
	ntfs_debug("Done.\n");
	return 0;
err_out:
	ntfs_attr_put_search_ctx(actx);
	if (need_lock) {
		mutex_unlock(&dir_ni->mrec_lock);
		mutex_unlock(&ni->mrec_lock);
	}
	return err;
}

/*
 * __ntfs_link - create hard link for file or directory
 * @ni:		ntfs inode for object to create hard link
 * @dir_ni:	ntfs inode for directory in which new link should be placed
 * @name:	unicode name of the new link
 * @name_len:	length of the name in unicode characters
 *
 * Create a new hard link. This involves adding an entry to the directory
 * index and adding a new FILE_NAME attribute to the target inode.
 *
 * Return 0 on success and -errno on error.
 */
static int __ntfs_link(struct ntfs_inode *ni, struct ntfs_inode *dir_ni,
		__le16 *name, u8 name_len)
{
	struct super_block *sb;
	struct inode *vi = VFS_I(ni);
	struct file_name_attr *fn = NULL;
	int fn_len, err = 0;
	struct mft_record *dir_mrec = NULL, *ni_mrec = NULL;

	ntfs_debug("Entering.\n");

	sb = dir_ni->vol->sb;
	if (NInoBeingDeleted(dir_ni) || NInoBeingDeleted(ni))
		return -ENOENT;

	ni_mrec = map_mft_record(ni);
	if (IS_ERR(ni_mrec)) {
		err = -EIO;
		goto err_out;
	}

	if (le16_to_cpu(ni_mrec->link_count) == 0) {
		err = -ENOENT;
		goto err_out;
	}

	/* Create FILE_NAME attribute. */
	fn_len = sizeof(struct file_name_attr) + name_len * sizeof(__le16);

	fn = kzalloc(fn_len, GFP_NOFS);
	if (!fn) {
		err = -ENOMEM;
		goto err_out;
	}

	dir_mrec = map_mft_record(dir_ni);
	if (IS_ERR(dir_mrec)) {
		err = -EIO;
		goto err_out;
	}

	fn->parent_directory = MK_LE_MREF(dir_ni->mft_no,
			le16_to_cpu(dir_mrec->sequence_number));
	unmap_mft_record(dir_ni);
	fn->file_name_length = name_len;
	fn->file_name_type = FILE_NAME_POSIX;
	fn->file_attributes = ni->flags;
	if (ni_mrec->flags & MFT_RECORD_IS_DIRECTORY) {
		fn->file_attributes |= FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT;
		fn->allocated_size = fn->data_size = 0;
	} else {
		if (NInoSparse(ni) || NInoCompressed(ni))
			fn->allocated_size =
				cpu_to_le64(ni->itype.compressed.size);
		else
			fn->allocated_size = cpu_to_le64(ni->allocated_size);
		fn->data_size = cpu_to_le64(ni->data_size);
	}
	if (NVolHideDotFiles(dir_ni->vol) && name_len > 0 && name[0] == cpu_to_le16('.'))
		fn->file_attributes |= FILE_ATTR_HIDDEN;

	fn->creation_time = utc2ntfs(ni->i_crtime);
	fn->last_data_change_time = utc2ntfs(inode_get_mtime(vi));
	fn->last_mft_change_time = utc2ntfs(inode_get_ctime(vi));
	fn->last_access_time = utc2ntfs(inode_get_atime(vi));
	memcpy(fn->file_name, name, name_len * sizeof(__le16));

	/* Add FILE_NAME attribute to index. */
	err = ntfs_index_add_filename(dir_ni, fn, MK_MREF(ni->mft_no,
					le16_to_cpu(ni_mrec->sequence_number)));
	if (err) {
		ntfs_error(sb, "Failed to add filename to the index");
		goto err_out;
	}
	/* Add FILE_NAME attribute to inode. */
	err = ntfs_attr_add(ni, AT_FILE_NAME, AT_UNNAMED, 0, (u8 *)fn, fn_len);
	if (err) {
		ntfs_error(sb, "Failed to add FILE_NAME attribute.\n");
		/* Try to remove just added attribute from index. */
		if (ntfs_index_remove(dir_ni, fn, fn_len))
			goto rollback_failed;
		goto err_out;
	}
	/* Increment hard links count. */
	ni_mrec->link_count = cpu_to_le16(le16_to_cpu(ni_mrec->link_count) + 1);
	if (!S_ISDIR(vi->i_mode))
		inc_nlink(VFS_I(ni));

	/* Done! */
	mark_mft_record_dirty(ni);
	kfree(fn);
	unmap_mft_record(ni);

	ntfs_debug("Done.\n");

	return 0;
rollback_failed:
	ntfs_error(sb, "Rollback failed. Leaving inconsistent metadata.\n");
err_out:
	kfree(fn);
	if (!IS_ERR_OR_NULL(ni_mrec))
		unmap_mft_record(ni);
	return err;
}

/* PORT: super.c installs this; the port exports nothing over NFS. */
const struct export_operations ntfs_export_ops = { 0 };

/*
 * PORT: the kernel's dentry based inode_operations (ntfs_create, ntfs_unlink,
 * ntfs_mkdir, ntfs_rmdir, ntfs_rename, ntfs_symlink, ntfs_mknod, ntfs_link)
 * and the NFS export operations are replaced by the name based entry points
 * below. They keep the kernel's locking and on-disk logic (__ntfs_create,
 * ntfs_delete, __ntfs_link) unchanged.
 */

/*
 * ntfs_vfs_validate_uname - apply the mount's name policy
 *
 * Unless the volume was mounted with NTFS_MOUNT_ALLOW_WINDOWS_ILLEGAL (which
 * clears NVolCheckWindowsNames), reject characters and reserved names that
 * Windows refuses (docs/PORTING.md §6).
 */
int ntfs_vfs_validate_uname(struct ntfs_volume *vol, const __le16 *uname, int len)
{
	if (len <= 0 || len > NTFS_MAX_NAME_LEN)
		return len <= 0 ? -EINVAL : -ENAMETOOLONG;
	if (!NVolCheckWindowsNames(vol))
		return 0;
	return ntfs_check_bad_windows_name(vol, uname, len);
}

/* Convert @name to Unicode (ntfs_name_cache allocation), optionally
 * validating it. Returns the length in Unicode characters or -errno. */
static int ntfs_vfs_uname(struct ntfs_volume *vol, const char *name, int len,
		__le16 **uname, bool check)
{
	int uname_len, err;

	uname_len = ntfs_nlstoucs(vol, name, len, uname, NTFS_MAX_NAME_LEN);
	if (uname_len < 0) {
		if (uname_len != -ENAMETOOLONG)
			ntfs_error(vol->sb, "Failed to convert name to Unicode.");
		return uname_len;
	}
	if (check) {
		err = ntfs_vfs_validate_uname(vol, *uname, uname_len);
		if (err) {
			kmem_cache_free(ntfs_name_cache, *uname);
			*uname = NULL;
			return err;
		}
	}
	return uname_len;
}

static void ntfs_vfs_set_dirty(struct ntfs_volume *vol)
{
	if (!(vol->vol_flags & VOLUME_IS_DIRTY))
		ntfs_set_volume_flags(vol, VOLUME_IS_DIRTY);
}

/*
 * ntfs_vfs_create - create a file, directory or symlink
 * @dir:	parent directory inode
 * @name/@len:	name (UTF-8)
 * @mode:	S_IFREG | S_IFDIR | S_IFLNK plus permission bits
 * @target:	symlink target (NUL-terminated) for S_IFLNK, else NULL
 *
 * Return the new, referenced inode or an ERR_PTR().
 */
struct inode *ntfs_vfs_create(struct inode *dir, const char *name, int len,
		umode_t mode, const char *target)
{
	struct ntfs_volume *vol = NTFS_SB(dir->i_sb);
	struct ntfs_inode *ni;
	struct inode *vi;
	__le16 *uname, *utarget = NULL;
	int uname_len, utarget_len = 0;

	if (NVolShutdown(vol))
		return ERR_PTR(-EIO);

	uname_len = ntfs_vfs_uname(vol, name, len, &uname, true);
	if (uname_len < 0)
		return ERR_PTR(uname_len);

	if (S_ISLNK(mode)) {
		utarget_len = ntfs_nlstoucs(vol, target, strlen(target),
					    &utarget, PATH_MAX);
		if (utarget_len < 0) {
			if (utarget_len != -ENAMETOOLONG)
				ntfs_error(vol->sb, "Failed to convert target name to Unicode.");
			kmem_cache_free(ntfs_name_cache, uname);
			return ERR_PTR(utarget_len);
		}
	}

	ntfs_vfs_set_dirty(vol);

	ni = __ntfs_create(NULL, dir, uname, uname_len, mode, 0, utarget,
			   utarget_len);
	kmem_cache_free(ntfs_name_cache, uname);
	kvfree(utarget);
	if (IS_ERR(ni))
		return ERR_CAST(ni);

	vi = VFS_I(ni);
	if (S_ISLNK(mode))
		vi->i_size = strlen(target);
	/* d_instantiate_new() in the kernel: publish the inode. */
	unlock_new_inode(vi);
	return vi;
}

/*
 * ntfs_vfs_unlink - remove a name (unlink or rmdir)
 *
 * Locking: caller holds the directory's i_rwsem.
 */
int ntfs_vfs_unlink(struct inode *dir, const char *name, int len, bool rmdir)
{
	struct ntfs_volume *vol = NTFS_SB(dir->i_sb);
	struct inode *vi;
	__le16 *uname;
	int uname_len, err;

	if (NVolShutdown(vol))
		return -EIO;

	vi = ntfs_vfs_lookup(dir, name, len);
	if (IS_ERR(vi))
		return PTR_ERR(vi);
	if (rmdir && !S_ISDIR(vi->i_mode)) {
		err = -ENOTDIR;
		goto out;
	}
	if (!rmdir && S_ISDIR(vi->i_mode)) {
		err = -EISDIR;
		goto out;
	}
	if (NVolSysImmutable(vol) && (vi->i_flags & S_IMMUTABLE)) {
		err = -EPERM;
		goto out;
	}

	/* No name policy check: names made elsewhere must be removable. */
	uname_len = ntfs_vfs_uname(vol, name, len, &uname, false);
	if (uname_len < 0) {
		err = uname_len;
		goto out;
	}

	ntfs_vfs_set_dirty(vol);

	err = ntfs_delete(NTFS_I(vi), NTFS_I(dir), uname, uname_len, true);
	kmem_cache_free(ntfs_name_cache, uname);
	if (err)
		goto out;

	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);
	inode_set_ctime_to_ts(vi, inode_get_ctime(dir));
	if (vi->i_nlink)
		mark_inode_dirty(vi);
out:
	iput(vi);
	return err;
}

/*
 * ntfs_vfs_link - create a hard link @name in @dir to inode @vi
 *
 * Locking: caller holds @dir's i_rwsem.
 */
int ntfs_vfs_link(struct inode *vi, struct inode *dir, const char *name, int len)
{
	struct ntfs_volume *vol = NTFS_SB(vi->i_sb);
	struct ntfs_inode *ni = NTFS_I(vi), *dir_ni = NTFS_I(dir);
	__le16 *uname;
	int uname_len, err;

	if (NVolShutdown(vol))
		return -EIO;
	if (S_ISDIR(vi->i_mode))
		return -EPERM;
	if (vi->i_sb != dir->i_sb)
		return -EXDEV;

	uname_len = ntfs_vfs_uname(vol, name, len, &uname, true);
	if (uname_len < 0)
		return uname_len;

	ntfs_vfs_set_dirty(vol);

	mutex_lock_nested(&ni->mrec_lock, NTFS_INODE_MUTEX_NORMAL);
	mutex_lock_nested(&dir_ni->mrec_lock, NTFS_INODE_MUTEX_PARENT);
	err = __ntfs_link(ni, dir_ni, uname, uname_len);
	if (err) {
		mutex_unlock(&dir_ni->mrec_lock);
		mutex_unlock(&ni->mrec_lock);
		pr_err("failed to create link, err = %d\n", err);
		goto out;
	}

	inode_inc_iversion(dir);
	simple_inode_init_ts(dir);
	mark_inode_dirty(dir);

	inode_inc_iversion(vi);
	inode_set_ctime_current(vi);
	mark_inode_dirty(vi);

	mutex_unlock(&dir_ni->mrec_lock);
	mutex_unlock(&ni->mrec_lock);
out:
	kmem_cache_free(ntfs_name_cache, uname);
	return err;
}

/*
 * __ntfs_vfs_rename - the kernel's ntfs_rename() without dentries
 * @new_inode:	inode currently at the target name, or NULL
 */
static int __ntfs_vfs_rename(struct inode *old_dir, struct inode *old_inode,
		__le16 *uname_old, int old_name_len,
		struct inode *new_dir, struct inode *new_inode,
		__le16 *uname_new, int new_name_len)
{
	int err = 0;
	int is_dir;
	struct super_block *sb = old_dir->i_sb;
	struct ntfs_inode *old_ni, *new_ni = NULL;
	struct ntfs_inode *old_dir_ni = NTFS_I(old_dir), *new_dir_ni = NTFS_I(new_dir);

	old_ni = NTFS_I(old_inode);

	mutex_lock_nested(&old_ni->mrec_lock, NTFS_INODE_MUTEX_NORMAL);
	mutex_lock_nested(&old_dir_ni->mrec_lock, NTFS_INODE_MUTEX_PARENT);

	if (NInoBeingDeleted(old_ni) || NInoBeingDeleted(old_dir_ni)) {
		err = -ENOENT;
		goto unlock_old;
	}

	is_dir = S_ISDIR(old_inode->i_mode);

	if (new_inode) {
		new_ni = NTFS_I(new_inode);
		mutex_lock_nested(&new_ni->mrec_lock, NTFS_INODE_MUTEX_NORMAL_2);
		if (old_dir != new_dir) {
			mutex_lock_nested(&new_dir_ni->mrec_lock, NTFS_INODE_MUTEX_PARENT_2);
			if (NInoBeingDeleted(new_dir_ni)) {
				err = -ENOENT;
				goto err_out;
			}
		}

		if (NInoBeingDeleted(new_ni)) {
			err = -ENOENT;
			goto err_out;
		}

		if (is_dir) {
			struct mft_record *ni_mrec;

			ni_mrec = map_mft_record(NTFS_I(new_inode));
			if (IS_ERR(ni_mrec)) {
				err = -EIO;
				goto err_out;
			}
			err = ntfs_check_empty_dir(NTFS_I(new_inode), ni_mrec);
			unmap_mft_record(NTFS_I(new_inode));
			if (err)
				goto err_out;
		}

		err = ntfs_delete(new_ni, new_dir_ni, uname_new, new_name_len, false);
		if (err)
			goto err_out;
	} else {
		if (old_dir != new_dir) {
			mutex_lock_nested(&new_dir_ni->mrec_lock, NTFS_INODE_MUTEX_PARENT_2);
			if (NInoBeingDeleted(new_dir_ni)) {
				err = -ENOENT;
				goto err_out;
			}
		}
	}

	err = __ntfs_link(old_ni, new_dir_ni, uname_new, new_name_len);
	if (err)
		goto err_out;

	err = ntfs_delete(old_ni, old_dir_ni, uname_old, old_name_len, false);
	if (err) {
		int err2;

		ntfs_error(sb, "Failed to delete old ntfs inode(%llu) in old dir, err : %d\n",
				old_ni->mft_no, err);
		err2 = ntfs_delete(old_ni, new_dir_ni, uname_new, new_name_len, false);
		if (err2)
			ntfs_error(sb, "Failed to delete old ntfs inode in new dir, err : %d\n",
					err2);
		goto err_out;
	}

	/* simple_rename_timestamp() */
	{
		struct timespec64 now = inode_set_ctime_current(old_dir);

		inode_set_mtime_to_ts(old_dir, now);
		if (new_dir != old_dir) {
			inode_set_mtime_to_ts(new_dir, now);
			inode_set_ctime_to_ts(new_dir, now);
		}
		inode_set_ctime_to_ts(old_inode, now);
		if (new_inode)
			inode_set_ctime_to_ts(new_inode, now);
	}
	mark_inode_dirty(old_inode);
	mark_inode_dirty(old_dir);
	if (old_dir != new_dir)
		mark_inode_dirty(new_dir);
	if (new_inode)
		mark_inode_dirty(new_inode);

	inode_inc_iversion(new_dir);

err_out:
	if (old_dir != new_dir)
		mutex_unlock(&new_dir_ni->mrec_lock);
	if (new_inode)
		mutex_unlock(&new_ni->mrec_lock);

unlock_old:
	mutex_unlock(&old_dir_ni->mrec_lock);
	mutex_unlock(&old_ni->mrec_lock);

	return err;
}

/*
 * ntfs_vfs_rename - POSIX rename
 *
 * Replaces an existing target when the types are compatible (a directory
 * only by an empty directory). A rename that only changes the case of a
 * name on a case-insensitive volume goes through a temporary name because
 * $I30 collates the two names equal.
 *
 * Locking: caller holds both directories' i_rwsem (and serializes renames).
 */
int ntfs_vfs_rename(struct inode *old_dir, const char *old_name, int old_len,
		struct inode *new_dir, const char *new_name, int new_len)
{
	struct ntfs_volume *vol = NTFS_SB(old_dir->i_sb);
	struct inode *old_inode, *new_inode = NULL;
	__le16 *uname_old = NULL, *uname_new = NULL;
	int old_name_len, new_name_len, err;

	if (NVolShutdown(vol))
		return -EIO;
	if (old_dir->i_sb != new_dir->i_sb)
		return -EXDEV;

	old_inode = ntfs_vfs_lookup(old_dir, old_name, old_len);
	if (IS_ERR(old_inode))
		return PTR_ERR(old_inode);

	new_inode = ntfs_vfs_lookup(new_dir, new_name, new_len);
	if (IS_ERR(new_inode)) {
		err = PTR_ERR(new_inode);
		new_inode = NULL;
		if (err != -ENOENT)
			goto out;
	}

	new_name_len = ntfs_vfs_uname(vol, new_name, new_len, &uname_new, true);
	if (new_name_len < 0) {
		err = new_name_len;
		goto out;
	}
	old_name_len = ntfs_vfs_uname(vol, old_name, old_len, &uname_old, false);
	if (old_name_len < 0) {
		err = old_name_len;
		goto out;
	}

	if (new_inode == old_inode) {
		/* Same inode: identical name is a no-op, otherwise this is
		 * a case-only rename (or a rename onto a hard link of
		 * itself, which POSIX also treats as a no-op). */
		if (old_dir != new_dir ||
		    ntfs_are_names_equal(uname_old, old_name_len, uname_new,
					 new_name_len, CASE_SENSITIVE,
					 vol->upcase, vol->upcase_len)) {
			err = 0;
			goto out;
		}
		iput(new_inode);
		new_inode = NULL;
		if (!NVolCaseSensitive(vol)) {
			static const __le16 tmp_suffix[] = {
				cpu_to_le16('.'), cpu_to_le16('n'),
				cpu_to_le16('t'), cpu_to_le16('f'),
				cpu_to_le16('s'), cpu_to_le16('t'),
				cpu_to_le16('m'), cpu_to_le16('p') };
			__le16 *utmp;
			int tmp_len = old_name_len + ARRAY_SIZE(tmp_suffix);

			if (tmp_len > NTFS_MAX_NAME_LEN)
				tmp_len = NTFS_MAX_NAME_LEN;
			utmp = kmem_cache_alloc(ntfs_name_cache, GFP_NOFS);
			if (!utmp) {
				err = -ENOMEM;
				goto out;
			}
			memcpy(utmp, uname_old, (tmp_len - ARRAY_SIZE(tmp_suffix)) *
					sizeof(__le16));
			memcpy(utmp + tmp_len - ARRAY_SIZE(tmp_suffix), tmp_suffix,
					sizeof(tmp_suffix));
			utmp[tmp_len] = 0;
			ntfs_vfs_set_dirty(vol);
			err = __ntfs_vfs_rename(old_dir, old_inode, uname_old,
					old_name_len, old_dir, NULL, utmp, tmp_len);
			if (!err)
				err = __ntfs_vfs_rename(old_dir, old_inode, utmp,
						tmp_len, old_dir, NULL, uname_new,
						new_name_len);
			kmem_cache_free(ntfs_name_cache, utmp);
			goto out;
		}
	}

	if (new_inode) {
		if (S_ISDIR(old_inode->i_mode) && !S_ISDIR(new_inode->i_mode)) {
			err = -ENOTDIR;
			goto out;
		}
		if (!S_ISDIR(old_inode->i_mode) && S_ISDIR(new_inode->i_mode)) {
			err = -EISDIR;
			goto out;
		}
	}

	ntfs_vfs_set_dirty(vol);
	err = __ntfs_vfs_rename(old_dir, old_inode, uname_old, old_name_len,
				new_dir, new_inode, uname_new, new_name_len);
out:
	if (uname_new)
		kmem_cache_free(ntfs_name_cache, uname_new);
	if (uname_old)
		kmem_cache_free(ntfs_name_cache, uname_old);
	iput(new_inode);
	iput(old_inode);
	return err;
}

/*
 * ntfs_vfs_parent_ino - inode number of the parent directory of @vi
 *
 * Taken from the kernel's ntfs_get_parent() (NFS export). Uses the first
 * $FILE_NAME attribute; returns (u64)-1 on error.
 */
u64 ntfs_vfs_parent_ino(struct inode *vi)
{
	struct ntfs_inode *ni = NTFS_I(vi);
	struct mft_record *mrec;
	struct ntfs_attr_search_ctx *ctx;
	struct attr_record *attr;
	struct file_name_attr *fn;
	u64 parent_ino = (u64)-1;
	int err;

	mrec = map_mft_record(ni);
	if (IS_ERR(mrec))
		return parent_ino;
	ctx = ntfs_attr_get_search_ctx(ni, mrec);
	if (unlikely(!ctx)) {
		unmap_mft_record(ni);
		return parent_ino;
	}
try_next:
	err = ntfs_attr_lookup(AT_FILE_NAME, NULL, 0, CASE_SENSITIVE, 0, NULL,
			0, ctx);
	if (unlikely(err))
		goto out;
	attr = ctx->attr;
	if (unlikely(attr->non_resident))
		goto try_next;
	fn = (struct file_name_attr *)((u8 *)attr +
			le16_to_cpu(attr->data.resident.value_offset));
	if (unlikely((u8 *)fn + le32_to_cpu(attr->data.resident.value_length) >
	    (u8 *)attr + le32_to_cpu(attr->length)))
		goto try_next;
	parent_ino = MREF_LE(fn->parent_directory);
out:
	ntfs_attr_put_search_ctx(ctx);
	unmap_mft_record(ni);
	return parent_ino;
}
