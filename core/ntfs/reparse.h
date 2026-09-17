/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2008-2021 Jean-Pierre Andre
 * Copyright (c) 2025 LG Electronics Co., Ltd.
 */

#ifndef _LINUX_NTFS_REPARSE_H
#define _LINUX_NTFS_REPARSE_H

extern __le16 reparse_index_name[];

/* @dir_record: the MFT record carries MFT_RECORD_IS_DIRECTORY. A junction is
 * only valid on one; a symlink may sit on either. */
unsigned int ntfs_make_symlink(struct ntfs_inode *ni, bool dir_record);

/*
 * Windows symbolic link reparse data, [MS-FSCC] 2.1.2.4. Offsets are from byte
 * 0 of path_buffer, lengths are bytes and exclude any terminator, strings are
 * UTF-16LE. Both names may appear in either order in path_buffer.
 */
struct ntfs_win_symlink {
	__le16 subst_name_offset;
	__le16 subst_name_length;
	__le16 print_name_offset;
	__le16 print_name_length;
	__le32 flags;
	__le16 path_buffer[];
} __packed;

#define SYMLINK_FLAG_RELATIVE	cpu_to_le32(0x00000001)

int ntfs_reparse_set_win_symlink(struct ntfs_inode *ni,
		const __le16 *target, int target_len);

/* True when @target (UTF-16, @len units) can be written as a native Windows
 * symlink and read back unchanged; false when it needs the WSL tag to survive.
 * Refuses the characters Windows forbids in a name, and a literal backslash,
 * which the native form cannot distinguish from a separator. */
bool ntfs_symlink_target_is_windows_safe(const __le16 *target, int len);
unsigned int ntfs_reparse_tag_dt_types(struct ntfs_volume *vol, unsigned long mref);
int ntfs_reparse_set_wsl_symlink(struct ntfs_inode *ni,
			const __le16 *target, int target_len);
int ntfs_reparse_set_wsl_not_symlink(struct ntfs_inode *ni, mode_t mode);
int ntfs_delete_reparse_index(struct ntfs_inode *ni);
int ntfs_remove_ntfs_reparse_data(struct ntfs_inode *ni);

#endif /* _LINUX_NTFS_REPARSE_H */
