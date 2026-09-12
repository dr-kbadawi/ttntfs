# Bugs found in upstream Linux 7.1 `fs/ntfs` while porting

Worked around in `core/vfs/api.c` (see the `PORT:` comments there). Worth
reporting upstream once reduced to kernel reproducers.

| # | Where | Symptom | Our workaround |
|---|---|---|---|
| 1 | `ntfs_mark_quotas_out_of_date` (`quota.c`) | Looks up `$Quota` under the `$I30` index and reads the key as data | Skipped on our mount path |
| 2 | `ntfs_attr_rm` (`attrib.c`) | Returns the freed-cluster count instead of 0 on success, so callers treating nonzero as error fail | Result normalised at the call site |
| 3 | `ntfs_attr_set_initialized_size` on an attribute inode | Maps a stale copy of the MFT record | Re-map after the call |
| 4 | Attribute `pwrite` over a hole | Leaves dirty folios over the hole, later written as data | Zero/invalidate around direct spans |

Found 2026-09-12 by the vfs stream; verified by `tools/run-tests.sh write`.
