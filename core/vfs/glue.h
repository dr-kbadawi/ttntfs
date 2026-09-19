/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 TechTag GmbH */
/*
 * glue.h - the volume handle behind ntfscore.h's ntfs_volume_t.
 *
 * PORT: only api.c and super_glue.c include this, after renaming the three
 * identifiers that ntfscore.h and core/ntfs/inode.h both define
 * (struct ntfs_attr, ntfs_getattr, ntfs_setattr) away from the internal
 * declarations - see the top of those files.
 */
#ifndef NTFS_GLUE_H
#define NTFS_GLUE_H

#include <linux/fs.h>
#include <linux/fs_context.h>
#include "ntfscore.h"

struct ntfs_volume_handle {
	struct super_block *sb;
	struct ntfs_volume *vol;
	struct fs_context fc;
	struct ntfs_mount_options opts;
	int ro_reason;			/* enum ntfs_ro_reason */
	bool dirty;			/* VOLUME_IS_DIRTY as found on disk */
	bool hibernated;
	bool logfile_clean;
};

/*
 * Zero hiberfil.sys's header so check_windows_hibernation_status() (and our
 * ntfs_glue_hibernated()) stop seeing a saved session. Implemented in api.c,
 * where the write path lives; the volume must already be read-write.
 */
int ntfs_glue_zero_hiberfil(struct ntfs_volume *vol);

#endif /* NTFS_GLUE_H */
