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

## Compressed writes: three data-loss bugs (2026-09-14)

Found by `core/tests/test_compress.c`, the first thing ever to write to a
compressed file through this driver. **All three report success to the caller**,
and all three are pinned by tests marked `BUG:` so that fixing one fails the
test that documents it. `ntfsck` is clean in every case: the results are legal
NTFS, just not what the writer asked for.

Together they mean **appending to a compressed file does not work at all**
through the public ABI. That matters before FSKit exposes the path.

| # | Where | Symptom |
|---|---|---|
| 9 | `ntfs_compress_write`, `core/ntfs/compress.c` | Expands the attribute only when `pos + count > allocated_size`, never on `> data_size`. A compressed file's allocation is rounded up to the 64 KiB block, so a write into that rounding **returns success, re-encodes the block, writes it to disk, and leaves `i_size` unchanged**. The bytes are on the platter and permanently unreachable. Measured on a 102400-byte file with 131072 allocated: `ntfs_write(ni, buf, 8192, 102400)` returns 8192 and the file is still 102400 bytes. |
| 10 | same | `initialized_size` is never advanced on the compressed path. When a write does pass `allocated_size`, `ntfs_attr_expand` moves `data_size` and nothing touches `initialized_size`. Confirmed on disk with `ntfsinfo`: `Data size: 201000 / Initialized size: 102400`. Everything past the old EOF reads back as zeroes, before and after a remount, though the compressed data is correctly written. |
| 11 | `ntfs_write_cb`, same file | The `allzeroes` branch does `goto out` with `err = 0` **before** `ntfs_non_resident_attr_punch_hole`. Writing 64 KiB of zeroes over a block that holds data returns 65536 and the previous contents read back byte-for-byte after a remount. Correct and cheap when the block was already a hole; data loss when it was not. |

Minor, not pinned: `ntfs_compress_write` returns `int` but assigns a `size_t`
byte count, so a single write above 2 GiB would overflow the return.

What does work, verified byte-for-byte after remount and `ntfsck`-clean:
in-place overwrite of compressed files (compressible, incompressible via the
store-raw fallback, unaligned tails, writes straddling and spanning compression
blocks) and resident-to-non-resident growth. The sparse read, seek and
`fallocate` paths are consistent.

## Error handling: three findings from fault injection (2026-09-14)

Found by `core/tests/test_faults.c`, the first fault injection this project has
had. All pinned; each pin says "invert when fixed".

| # | Where | Symptom |
|---|---|---|
| 12 | `core/vfs/file.c` `ntfs_fsync`, `core/ntfs/super.c` `ntfs_sync_fs` | **A failed device flush is silently dropped.** With the barrier refused, `ntfs_fsync()` and `ntfs_volume_sync()` both return **0**. fsync's entire contract is broken: the caller is told the data is on stable storage while it sits in a volatile write cache, which is exactly the case a power cut then loses. Both call sites discard the return (`blkdev_issue_flush(...)` with no assignment). **This is a porting bug, not upstream's**: in Linux `blkdev_issue_flush()` returns `void` and the error surfaces through the bdev mapping's writeback error, which this port does not have. Our shim gave it an `int` return (an `F_FULLFSYNC`) and nothing reads it. |
| 13 | `core/vfs/super_glue.c` | **`ro_reason` is never set when errors force a read-only switch.** After every metadata write is refused the volume correctly drops to read-only, but `ntfs_volume_get_info()` reports `ro_reason = NTFS_RO_NONE`. `NTFS_RO_ERRORS` exists in the ABI for exactly this and nothing assigns it, so the menu bar tells the user their disk went read-only for no stated reason when the true reason is failing hardware and they should copy data off now. |
| 14 | mount path | **An I/O error at mount is reported as `-EINVAL`.** A read failure on the boot sector, on `$MFT` record 0, or on the root directory's record all return `-EINVAL`, the same errno as a genuinely non-NTFS partition, and the log says "Not an NTFS volume". A user with a dying disk or a flaky cable is told their partition is not NTFS, which is the sentence that makes people reformat. `-EIO` is the honest answer. `$MFTMirr` is the one case that behaves, returning `-EROFS`. |

Verified not to be problems: `ntfs_volume_sync` returning 0 after a data write
failure is correct, because fsync already reported it and errors report once; a
torn buffered write returning the full count is correct, and the following sync
does report `-EIO`. **No injected fault left a volume `ntfsck` called dirty**,
and no failed mount wrote a single byte, checked with a fingerprint of the whole
image before and after.

