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

## Found 2026-09-14 by the new test suites

These are in our own code or our own ABI rather than upstream, and none is
fixed. Each is pinned by a test that will fail when the behaviour changes, so
fixing one means updating the test that documents it.

| # | Where | Symptom | Status |
|---|---|---|---|
| 5 | `core/ntfs/time.h` `ntfs2utc()` | Any pre-1970 instant that is not on a whole second comes back **denormalised**: `div_s64_rem` truncates toward zero, so `tv_nsec` is negative. NTFS tick 1 reads back as `tv_sec = -11644473599, tv_nsec = -999999900`. The value is arithmetically right and round-trips, so nothing on disk is harmed, but `struct ntfs_timespec` goes straight out over the public ABI to FSKit, which will assume `0 <= nsec < 10^9`. Hits any file Windows stamped before 1970: restored archives, deliberately backdated files. | pinned in `test_links.c: test_time_conversion` |
| 6 | `core/include/ntfscore.h` | The `ntfs_setxattr()` comment names `XATTR_CREATE` and `XATTR_REPLACE` and **defines neither**. The core is built against `platform/include/linux/xattr.h`, where they are 1 and 2; Darwin's `<sys/xattr.h>` uses **2 and 4** for the same names, and `fskit/Bridge/NTFSExtension-Bridging-Header.h` hardcodes `0x1`/`0x2` as a third copy. A caller who includes the system header and means CREATE passes 2, which the core reads as REPLACE and fails with `ENOATTR`; one who means REPLACE passes 4, which reads as no flags and silently creates. | pinned by value in `test_names.c: xattr_as_stream` |
| 7 | `core/vfs/namei.c` `__ntfs_link` | **There is no 1023 hard-link cap.** `docs/PORTING.md` and this project's notes have referred to one; no such check exists anywhere in `core/` or `platform/`, and 1100 links were created successfully with `ntfsck -n` clean afterwards. `link_count` is a `__le16` incremented without a bound. Whether NTFS needs a cap is a separate question, but the documented one is not implemented. | pinned at 1030 links in `test_links.c: test_many_links` |
| 8 | `core/vfs/api.c` symlinks | `ntfs_write()` on a symlink inode succeeds and writes to its `$DATA` stream, while `getattr` reports `size` as the target length, so the two disagree. Not asserted either way: it was not possible to establish which behaviour is intended. | noted, not pinned |

