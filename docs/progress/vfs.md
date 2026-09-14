# vfs stream — status

Owner: vfs agent. Paths: `core/vfs/`, `core/include/`.

## Done
- `core/vfs/` compiles into `build/libntfscore.a` (`cmake -S . -B build && cmake --build build`;
  `ctest` 4/4). Harness: `cmake -S tools/ntfscli -B build-cli -DNTFSCLI_USE_STUB=OFF`.
- `inode.c`: upstream kept; PORT edits: `ntfs_drop_big_inode` -> `generic_drop_inode`,
  `ntfs_iget` drops (instead of caching) an inode whose read failed, `ntfs_set_vfs_operations`
  without inode_operations tables, `ntfs_show_options` stub, `iomap_zero_range` ->
  `ntfs_vfs_zero_range`, compressed named `$DATA` attribute inodes allowed.
- `namei.c`: `__ntfs_create`/`ntfs_delete`/`__ntfs_link` kept; dentry wrappers replaced by
  `ntfs_vfs_lookup/create/unlink/link/rename` (+ case-only rename via a temp name), `mknod`
  and NFS export ops dropped, `ntfs_check_bad_windows_name` exported.
- `file.c`: fsync, setattr/truncate, fallocate, punch-hole kept (inode based); iomap/dio/mmap/
  llseek/splice/ioctl/fiemap dropped.
- `aops.c`: `ntfs_aops` (resident copy from/to the mft record, run list -> `ntfs_bdev_*`,
  compressed via `ntfs_read_compressed_block`, holes/initialized_size zeroing) and
  `ntfs_mft_aops` (`ntfs_write_mft_block` from mft.c); direct read/write and zero-range helpers.
- `super_glue.c`: mount through `ntfs_fs_type`'s fs_context ops: always read-only first, then
  `reconfigure` to rw only if clean; `ro_reason` = requested/device/hibernated/dirty/
  unsupported/logfile/errors; probe, unmount, sync, statfs -> `ntfs_volume_get_info`, label.
- `api.c`: whole `ntfscore.h`. Read/write: resident from the mft record, compressed through the
  cache, otherwise whole pages direct from the run list to the device with cached edges
  (flush+invalidate around direct spans); write allocates clusters up front, zeroes the gap
  to `initialized_size`, extends it afterwards. xattrs = named `$DATA` streams. `noowners`
  mode/uid/gid policy. `preallocated_size=0` so `allocated_size` never carries slack.
- `compat_extra.c`: `dir_emit_dotdot`, `writeback_iter`.
- Read path: `ntfscli verify` passes 0 mismatches on all 7 fixtures (basic-4k 234, names 5058,
  compressed-sparse 23, cluster-512 231, cluster-64k 231, attrlist 11, empty 0);
  `tools/run-tests.sh --quick ground-truth` PASS (20/20 vs ntfs-3g).
- Write path: `tools/run-tests.sh write` on all 7 fixtures: every step passes (mkdir, cp-in 3 MiB
  and small, overwrite, mv, replacing mv, truncate shrink/grow, xattr set small/3 MiB/list/rm,
  rm, rmdir, sync, `ntfsfix -n` clean, ntfsls identical to pristine, manifest re-verify) except
  the runner's own "ntfsls after mv" expectation (see below). Also run under
  `-DNTFS_SANITIZE=ON`: verify on all fixtures and the write sequence on basic-4k and
  cluster-512 are clean (one UBSan alignment note, see below).
- Mount policy: read-only first; rw switch = `ntfs_reconfigure`'s checks minus
  `ntfs_mark_quotas_out_of_date` (upstream looks up `$Quota` under the name `$I30` and reads
  the entry key as data, so it always fails; the kernel's own rw mount never calls it).

## PORT edits outside core/vfs (minimal, all marked `/* PORT: */`)
- `core/ntfs/mft.c`, `mft.h`: `ntfs_write_mft_block` exported ($MFT write_folio backend).
- `core/ntfs/super.c`: `ntfs_fs_type` no longer static (mount glue uses its fs_context ops).
- `core/ntfs/ea.h`: `#define ntfs_listxattr ntfs_ea_listxattr` (public ABI owns the name).
- `core/ntfs/debug.c`: messages formatted with vsnprintf instead of `%pV` (was printing a
  pointer; every `ntfs_error` was unreadable).
- `core/ntfs/dir.c`: several POSIX names differing only in case no longer abort the lookup
  ("Found already allocated name"); the first candidate is kept while scanning for an exact match.
- `core/ntfs/compress.c`: compressed *named* `$DATA` streams accepted by
  `ntfs_read_compressed_block` (fixture compressed-sparse has one).

## Needs the integrator (outside core/vfs)
- **platform/pagecache/pagecache.c `read_mapping_folio()`** (uncommitted, in the working tree
  next to the integrator's own in-progress edits): kernel `read_cache_folio()` returns an
  already-uptodate folio *without* taking its lock; the port always took it. mft.c allocates an
  extent record with the `$MFT` folio locked and then maps that folio again through
  `map_mft_record()`, so every extent-record allocation (attribute lists, large ADS, many
  attributes) deadlocked. Fix = try `__filemap_get_folio(FGP_ACCESSED)` first and return it if
  uptodate; otherwise the existing locked read path. Please keep it when committing pagecache.c.
- `platform/include/asm/byteorder.h`: `le16_to_cpup()` & co take typed pointers; compress.c
  (LZNT1 tokens) calls them on odd addresses, UBSan `-fsanitize=alignment` reports it
  (harmless on arm64). Taking `const void *` would silence it.
Both resolved. The `ntfsls after mv` expectation was fixed in 98ed05d, and
`platform_inode` passes -- it was failing against an uncommitted pagecache
rewrite that has long since landed. `tools/run-tests.sh all` is 265/0 and
`ctest` is 15/15.

## Known problems / open questions
- Compressed files: writing works, including appending, since three upstream
  data-loss bugs were fixed on 2026-09-14 (U5-U7 in UPSTREAM-BUGS.md). Changing
  the size of one is still refused, as is everything about encrypted files.
- No `->release()`: pre-allocation is disabled instead of trimmed on close.
- Windows symlinks (IO_REPARSE_TAG_SYMLINK) are readable through `ntfs_readlink` (print name);
  created symlinks are WSL-style (upstream behaviour).

- Upstream `ntfs_attr_rm()` returns `ntfs_cluster_free()`'s positive cluster count for a
  non-resident attribute; api.c treats > 0 as success.
- Upstream `ntfs_attr_set_initialized_size()` opens its search context on the inode passed and
  so maps a stale second copy of the base record for attribute inodes; api.c uses its own
  variant on the base inode for ADS writes.

## 2026-09-13: directory deletion degrades with directory size

Measured on a 1 GiB NTFS image on internal NVMe, so the medium is not the limit.
Deleting the **same 1000 files** from a directory that holds 1000 takes 1.61 s
(622/s); from one that holds 8000 it takes 7.67 s (130/s). Scaling across sizes:

| files in dir | create/s | delete/s | stat/s |
|---|---|---|---|
| 500 | 863 | 854 | 193802 |
| 1000 | 809 | 654 | 206253 |
| 2000 | 866 | 478 | 209474 |
| 4000 | 876 | 362 | 205378 |
| 8000 | 840 | 224 | 193998 |

Create and stat are flat; only delete degrades, roughly as n^0.4. It is not I/O:
the block layer's own counters (`io window` lines, see fskit/README.md) show the
device 10-34% busy through the whole phase, with uniform ~1.9 KiB writes at
0.05 ms each, so 70-90% of the time is our own code. `sample` attributes what
little it catches to the synchronous write path.

The unlink path is `ntfs_unlink` -> `namei.c` (searches the *inode's* $FILE_NAME
attributes, which does not depend on directory size) -> `ntfs_index_remove`
(`core/ntfs/index.c:1930`), which loops lookup + `ntfs_index_rm` retrying on
-EAGAIN. The growth has to be inside `ntfs_index_rm` or the retry count, not in
the lookup, which is O(log n). Not yet diagnosed further, and not yet fixed:
changing B-tree deletion in ported kernel code needs its own test coverage.

**Cause and fix (same day).** It was not the B-tree. `sample` put the time in
`NTFSVolume.synchronize` -> `ntfs_volume_sync` -> `sync_filesystem`, and FSKit
asks for a sync about every second operation. `pagecache_sync_sb()` walked every
mapping on the volume twice per sync, so each unlink cost O(cached mappings).
Fixed in the platform layer with a maybe-dirty mapping list (finding 15 in
platform-review.md); the B-tree code is untouched.

| files | delete/s before | delete/s after |
|---|---|---|
| 500 | 854 | ~900 |
| 1000 | 654 | ~830 |
| 2000 | 478 | ~740 |
| 4000 | 362 | ~615 |
| 8000 | 224 | ~445 |

The inode side had the same problem and got the same treatment (finding 16), so
the final figures are:

| files | delete/s before | mappings only | both |
|---|---|---|---|
| 500 | 854 | ~900 | ~1000 |
| 1000 | 654 | ~830 | ~890 |
| 2000 | 478 | ~740 | ~945 |
| 4000 | 362 | ~615 | ~906 |
| 8000 | 224 | ~445 | ~720 |

**Corrected by an A/B on the same machine, same session** (the earlier
"before" column was measured hours earlier with different disks attached, and
is not comparable). Building the pre-fix platform and the current one
back-to-back on a freshly made image:

| files | create/s pre | create/s now | delete/s pre | delete/s now |
|---|---|---|---|---|
| 500 | 669 | 675 | 800 | 1024 |
| 2000 | 655 | 674 | 443 | 986 |
| 8000 | 677 | 701 | 210 | 735 |

Delete is 1.3x / 2.2x / 3.5x faster and close to flat. **Creates did not
regress**; they are marginally faster. The ~12% create regression reported
earlier was an artefact of comparing across sessions, not a real effect, and
the claim in commits 08c6f10 and b58b5e8 is wrong.

## Next
- Windows `chkdsk /f` round trip on real media (phase 2 gate needs the PC). More
  urgent since 6835a53: `ntfs_glue_logfile_clean()` had required the journal to
  be both closed and flagged clean, which held nearly every real Windows volume
  at read-only. With the rule corrected to match `logfile.h`, the unvalidated
  write path now runs on ordinary disks rather than on fixtures alone.
- Hand `readdir` want_attr the index entry's sizes/times without an iget (needs a dir.c hook).
- Compressed files: writing now works, including appending (three upstream
  data-loss bugs fixed 2026-09-14, U5-U7 in UPSTREAM-BUGS.md, covered by
  `test_compress`). Still refused: changing the size of a compressed file
  (`-EOPNOTSUPP`), creating one (`ntfs_create` never sets the flag, so a
  compressed file can only be inherited from a volume Windows wrote), and
  everything about encrypted files.
- **Finding 15** is fixed (2026-09-14): any logical sector size but 512 used to
  corrupt the volume on the first metadata write. Two upstream unit errors,
  U8 and U9 in UPSTREAM-BUGS.md. True 4Kn disks are writable and the read-write
  mount guard in `super_glue.c` is gone.

## Write benchmarks, 2026-09-13

Ours (NTFS) against Apple's exFAT FSKit module, matched 2 GiB images on internal
NVMe, so neither is device-bound:

| | ours | Apple exFAT | ratio |
|---|---|---|---|
| stream 512 MiB | 1782 MB/s | 1551 | **1.15x** |
| overwrite 256 MiB | 2192 MB/s | 2160 | 1.01x |
| append 64 KiB x2048 | 1992 MB/s | 1834 | 1.09x |
| small file + fsync | 575/s | 859 | 0.67x |
| small file, no fsync | 684/s | 1059 | 0.65x |
| random 4 KiB write | 3929 IOPS | 9616 | **0.41x** |

Streaming write was 0.41x before the dirty-list sync fix and is now ahead of
Apple's; it was the largest beneficiary, since a streaming write dirties folios
constantly and the old sync walked everything on every call.

Everything meets the phase 5 "within 2x of exFAT" bar except random 4 KiB
writes.

**Where the 16 KiB comes from: the macOS kernel's page, not our code.**
Settled 2026-09-14. (The first explanation written here was wrong and the second
was undetermined; this one is measured end to end.)

`tools/harness/iotrace.c` wraps the block device with a counting shim and
histograms every transfer the core makes, called directly, with no FSKit and no
kernel in the way:

| through the core | device I/O |
|---|---|
| 500 random 4 KiB writes | 500 writes of 4 KiB, **zero reads** |
| 500 random 16 KiB writes | 500 writes of 16 KiB, zero reads |

So the core neither amplifies nor splits: it passes the size it is given
straight through, coalescing contiguous folios into one call. The amplification
is therefore introduced above it. Through a real mount:

| from userspace | device I/O seen by the bdev |
|---|---|
| 3000 random 4 KiB writes | 16.0 KiB per call, reads present |
| 1500 random 1 KiB writes | 15.8 KiB per call, no reads |

A 1 KiB write producing a 16 KiB device write is the decisive one. `hw.pagesize`
on this machine is **16384**: the unified buffer cache works in 16 KiB pages, so
a sub-page write makes the kernel fetch the whole page from us if it is not
resident (that is the read-modify-write, and the reads appear exactly when the
file is big enough for pages to have been evicted) and write the whole page back
later. Our driver never sees the small write at all.

Consequences:

* **It is not our defect and not ours to fix.** Sub-folio dirty tracking in our
  page cache would change nothing: our cache is handed 16 KiB and faithfully
  passes 16 KiB. The earlier note proposing buffer heads was aimed at the wrong
  layer.
* **It cannot explain the gap against exFAT.** Apple's module is a FSKit
  filesystem under the same UBC and receives the same 16 KiB requests. Both pay
  the same amplification.

So the real question is narrower than it looked: given both drivers receive
identical 16 KiB requests, why do we do 3929 IOPS against exFAT's 9616? That is
per-request cost, not amplification, and it is still open. One caveat for
whoever takes it: the 4 KiB benchmark ran on a 128 MiB file where pages had been
evicted, so part of what it measured was UBC misses; the 1 KiB run on a 64 MiB
file had full residency, no reads, and reached 6653 IOPS. Control for residency
before comparing.

Reads on real hardware (Samsung 860 EVO, UAS bridge, read-only because the
volume is dirty): ours 334 MB/s sequential and 3273 IOPS random 4 KiB, against
Apple's built-in ntfs at 377 MB/s and 3012 IOPS. Device busy 97% of a sequential
read, so there is no idle gap for readahead to fill; the ~11% difference and the
spread across regions both sit inside what the medium itself varies by.

## 2026-09-14: where the exFAT write gap actually is

Following the 16 KiB question. With buffer-cache residency held equal (32 MiB
file, fully warmed, so neither side is paying page faults), the random 4 KiB
write benchmark splits cleanly in two:

| | ours | Apple exFAT |
|---|---|---|
| the 2000 writes themselves | ~630k IOPS | ~630k IOPS |
| the `fsync` that follows | **70-75 ms** | **22 ms** |

The writes are equal because neither filesystem is involved: userspace is only
dirtying UBC pages. **The entire gap is the flush**, and the earlier "0.41x on
random 4 KiB writes" was really this fsync measured through a benchmark that
attributed it to the writes.

What the flush costs us, measured:

* 55-71% of fsync wall time is the extension's own CPU (`ps -o time` around the
  call, three trials).
* only ~20 ms of a 70-109 ms fsync is spent inside the device write calls
  (`io window` mean × count), so the device is not the limit.
* `sample` puts the time in the **write** path, not the sync path:
  `NTFSVolume.write` → `ntfs_write` → `do_write` → `ntfs_vfs_direct_write` →
  `walk_runs` → `write_extent` → `submit_bio_wait` → `ntfs_bdev_write` →
  `fskit_pwrite` → `pwrite` carried 233 samples against 26 for
  `synchronize` → `ntfs_volume_sync`. The fsync is the kernel handing us the
  dirty pages as write requests; the sync call itself is cheap.

For scale: 32 MiB of dirty pages written back as ~514 device calls in 70 ms is
457 MB/s, where exFAT manages 1450 MB/s for the same 32 MiB, and an uncached
`pwrite` of 32 MiB in 16 KiB chunks on this machine takes 23 ms. exFAT is at
that ceiling; we are 3x below it.

**Found and fixed on the way, but not the cause.** `linux/blkdev.h` mapped both
`sync_blockdev()` and `blkdev_issue_flush()` to `ntfs_bdev_flush()`. In Linux
only the second is a device cache flush; the first is
`filemap_write_and_wait(bdev->bd_mapping)`. Because the vendored driver calls
them in sequence -- `ntfs_fsync()` in core/vfs/file.c and `ntfs_sync_fs()` in
core/ntfs/super.c both do -- every fsync issued two full device flushes, and a
volume sync three once `sync_filesystem()` added its own. Now one each, matching
upstream. It made no measurable difference (a flush with nothing left to flush
is cheap), but it was wrong, and it is another entry for the
platform-versus-Linux semantics table.

**Still open:** whether the excess per-request cost is in the Swift/FSKit
boundary or in our C write path. The core alone writes 32 MiB in 6.9 ms through
a file-backed bdev, but that path has the host page cache under it and is not
comparable to the resource-backed one. Separating them needs timing inside
`NTFSVolume.write` against the `ntfs_write` it wraps, which needs an
instrumented build.

## Directories leave `$INDEX_ROOT` at the fourth entry (2026-09-14)

Measured by `core/tests/test_bigdir.c` on directories this driver creates:
11-character names, 4 KiB index blocks, 1 KiB MFT records, 4 KiB clusters.

| entries | tree |
|---|---|
| 4 | index moves out of `$INDEX_ROOT` into `$INDEX_ALLOCATION` |
| 79 | second level (root -> node -> leaf) |
| 1459 | third level |
| 3000 | 4 levels, 159 index blocks |
| 20000 | 5 levels, 1059 index blocks |

Four, not the few dozen a Windows-created directory manages. Name length barely
moves it: 4-character names also give 4, 40-character names give 3.

The cause is what else is in the MFT record. A directory we create carries
`$STANDARD_INFORMATION`, `$FILE_NAME`, a **resident** `$SECURITY_DESCRIPTOR`,
`$INDEX_ROOT`, `$INDEX_ALLOCATION`, `$BITMAP`, and an `$EA_INFORMATION`/`$EA`
pair -- the WSL metadata EA from `ntfs_ea_set_wsl_inode()`
(`core/vfs/namei.c`), which costs 120 bytes. That leaves roughly 360 bytes free
in a 1024-byte record, so `ntfs_ir_make_space()` hits `-ENOSPC` and reparents on
the third or fourth entry.

Not a correctness bug: every shape passes `ntfsck -n`. But every non-trivial
directory this driver creates is immediately a B-tree with an index allocation,
an index bitmap and extra clusters, where Windows would have stayed resident.
If that is not wanted, larger MFT records or a non-resident security descriptor
or EA would buy the resident case back. The test asserts a range rather than
`== 4` and prints the measured value on every run.

Second observation from the same work: emptying a directory collapses the tree
back into the resident root (`ntfs_ir_leafify` clears `LARGE_INDEX`) but leaves
the `$INDEX_ALLOCATION` attribute and its clusters allocated, so a directory
that briefly held 800 files keeps 34 index blocks forever. `ntfsck` calls that
clean and ntfs-3g does the same, so it is pinned as current behaviour rather
than asserted to be the only correct one.

