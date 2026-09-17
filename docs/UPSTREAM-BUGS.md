# Known defects

Two ledgers, two numbering schemes, because they need different actions:

* **U1-U7** are bugs in the vendored Linux 7.1 `fs/ntfs` source. They are worth
  reporting upstream once reduced to kernel reproducers, and each is worked
  around or fixed here behind a `PORT:` comment marking the divergence.
* **5-16** are defects in this port, its ABI or its tooling, found mostly by the
  test suites written on 2026-09-14. The sequence starts at 5 because these two
  lists were one list until the upstream table grew past four rows; the numbers
  are referenced from commit messages and test comments, so they stay as they
  are.

Every open finding is pinned by a test that fails when the behaviour changes, so
fixing one means updating the test that documents it.

## Upstream: Linux 7.1 `fs/ntfs`

| # | Where | Symptom | Ours |
|---|---|---|---|
| U1 | `ntfs_mark_quotas_out_of_date` (`quota.c`) | Looks up `$Quota` under the `$I30` index and reads the key as data | Skipped on our mount path |
| U2 | `ntfs_attr_rm` (`attrib.c`) | Returns the freed-cluster count instead of 0 on success, so callers treating nonzero as error fail | Result normalised at the call site |
| U3 | `ntfs_attr_set_initialized_size` on an attribute inode | Maps a stale copy of the MFT record | Re-map after the call |
| U4 | Attribute `pwrite` over a hole | Leaves dirty folios over the hole, later written as data | Zero/invalidate around direct spans |
| U5 | `ntfs_compress_write` (`compress.c`) | Expands the attribute on `pos + count > allocated_size`, never on `> data_size`, so a write into the 64 KiB allocation rounding past EOF writes the block to disk and leaves `i_size` unchanged. The data is unreachable. | Condition changed to `data_size`, under `mrec_lock` as the uncompressed path does. `PORT:` comment at the divergence. |
| U6 | same | `initialized_size` is never advanced on the compressed path, so everything past the old EOF reads back as zeroes though the data is correct on disk. | Advanced to the end of what was written, once the loop completes, so a failure still leaves a size to roll back to. |
| U7 | `ntfs_write_cb` (`compress.c`) | The `allzeroes` shortcut returns before the punch-hole call, so writing zeroes over a block that holds data leaves the old contents. | `cb_is_allocated()` walks the run list: hole-blocks keep the fast path, allocated blocks punch. An unmappable run list counts as allocated, because a needless punch wastes a write while a skipped one loses data. |
| U8 | `NTFS_B_TO_SECTOR()` and `ntfs_bytes_to_sector()` (`ntfs.h`) | Shift by `sb->s_blocksize_bits`. Both results only ever reach `bio->bi_iter.bi_sector`, which the block layer defines in **512-byte units** whatever the device's logical block size is. `parse_ntfs_boot_sector()` raises `s_blocksize` to `vol->sector_size`, so on a volume whose sector size is not 512 the writer shifts by more than the reader and every metadata bio lands at `offset / (s_blocksize / 512)` -- one eighth of its intended place on 4Kn, straight over `$MFT` records 0-5. Upstream contradicts itself two functions later: `ntfs_write_mft_block()` compares `bio_end_sector(bio) >> (cluster_size_bits - 9)` against an LCN, which is only right in 512-byte units. | Both shift by `SECTOR_SHIFT`. `PORT:` comments. |
| U9 | `ntfs_sync_mft_mirror()` (`mft.c`) | Builds the mirror offset from the record's offset *inside its folio* and drops `folio->index` entirely. Invisible while several records share a page, which is every volume with 1024-byte MFT records; on 4Kn, `mkntfs` makes 4096-byte records, one per page, so the in-folio offset is 0 for all of them and every mirror copy is written onto slot 0. | Offset includes `(u64)folio->index << PAGE_SHIFT`. `PORT:` comment. |

U5-U7 were confirmed present verbatim in `upstream/linux-v7.1/fs/ntfs/compress.c`: the only differences between it and `core/ntfs/compress.c` are folio and bio accessors. They are Linux bugs, not porting bugs.

Found 2026-09-12 by the vfs stream; verified by `tools/run-tests.sh write`.

## This port: found 2026-09-14 by the new test suites

In our own code, our ABI or our tooling rather than upstream.

| # | Where | Symptom | Status |
|---|---|---|---|
| 5 **FIXED** | `core/ntfs/time.h` `ntfs2utc()` | Any pre-1970 instant that is not on a whole second comes back **denormalised**: `div_s64_rem` truncates toward zero, so `tv_nsec` is negative. NTFS tick 1 reads back as `tv_sec = -11644473599, tv_nsec = -999999900`. The value is arithmetically right and round-trips, so nothing on disk is harmed, but `struct ntfs_timespec` goes straight out over the public ABI to FSKit, which will assume `0 <= nsec < 10^9`. Hits any file Windows stamped before 1970: restored archives, deliberately backdated files. | **FIXED** 2026-09-14. `core/ntfs/time.h` is byte-identical to upstream, and upstream is not wrong to leave it: in the kernel the value only ever goes back through `utc2ntfs()`, which is exact for either representation. It matters here only because we copy the struct into `struct ntfs_timespec` and hand it over the public ABI, so the fix is port-level and carries a `PORT:` comment. Borrows a second when the remainder is negative; round trip re-verified at the ticks either side of both the 1601 and 1970 epochs, where the sign flips. |
| 6 **FIXED** |`core/include/ntfscore.h` | The `ntfs_setxattr()` comment names `XATTR_CREATE` and `XATTR_REPLACE` and **defines neither**. The core is built against `platform/include/linux/xattr.h`, where they are 1 and 2; Darwin's `<sys/xattr.h>` uses **2 and 4** for the same names, and `fskit/Bridge/NTFSExtension-Bridging-Header.h` hardcodes `0x1`/`0x2` as a third copy. A caller who includes the system header and means CREATE passes 2, which the core reads as REPLACE and fails with `ENOATTR`; one who means REPLACE passes 4, which reads as no flags and silently creates. | **FIXED** 2026-09-14, and there were **five** copies, not three: the fifth was `tools/ntfscli/main.c`, which defined Darwin's 2/4 **and** included `<sys/xattr.h>`, so anyone adding a `--create` option would have reached for the obvious name and got a silent REPLACE. FSKit's own `FSSetXattrPolicy` also numbers mustCreate=1 and mustReplace=2, identical to the core's by coincidence. The numbers now exist once, in `core/include/ntfs_xattr_flags.h` -- a separate file because `platform/include/linux/xattr.h` must derive from it and cannot include `ntfscore.h` (the public names collide with `core/ntfs`'s internal ones). Three guards: `_Static_assert` in `api.c`, which is the one translation unit that sees both spellings; another in `test_names.c` pinning that our values differ from Darwin's and that Darwin's CREATE is our REPLACE; and behavioural checks. Introducing a drift now fails the build. |
| 7 **FIXED** | `core/vfs/namei.c` `__ntfs_link` | **There is no 1023 hard-link cap.** `docs/PORTING.md` and this project's notes have referred to one; no such check exists anywhere in `core/` or `platform/`, and 1100 links were created successfully with `ntfsck -n` clean afterwards. `link_count` is a `__le16` incremented without a bound. Whether NTFS needs a cap is a separate question, but the documented one is not implemented. | **FIXED** 2026-09-14, and the finding was half wrong. No cap existed, correctly. But the 1023 came from `fskit/NTFSExtension/NTFSVolume.swift`'s `maximumLinkCount`, present since the first scaffold commit and enforced nowhere -- `docs/PORTING.md` never mentioned it. The right number is **1024 total**: Microsoft documents `CreateHardLinkW` as capping at 1023 *additional* links, past which Windows returns `ERROR_TOO_MANY_LINKS`. It is driver policy, not structure (`$FILE_NAME` spills into extents via `$ATTRIBUTE_LIST`, and the on-disk field holds 65535). ntfs3 uses 4000 and ntfs-3g has none, so both build volumes Windows cannot extend; we chose Windows interop. Now `NTFS_LINK_MAX` in `ntfscore.h`, enforced in `ntfs_link()` with `-EMLINK`, with the Swift pathconf reading the same constant rather than keeping a second copy. Deliberately **not** in `__ntfs_link()`, which `ntfs_rename()` also uses: a cap there would refuse renaming a file that sits at the limit even though the net count is unchanged, and the test asserts that rename still works. |
| 8 **FIXED** | `core/vfs/api.c` symlinks | `ntfs_write()` on a symlink inode succeeds and writes to its `$DATA` stream, while `getattr` reports `size` as the target length, so the two disagree. Not asserted either way: it was not possible to establish which behaviour is intended. | **FIXED** 2026-09-14. Characterised further first: the write persists across unmount, `ntfsck` stays clean, and `readlink` still works, because the target lives in `$REPARSE_POINT` rather than `$DATA`. So it was not corruption but a stray stream nothing reads plus an ABI self-contradiction. `ntfs_read`, `ntfs_write`, `ntfs_truncate` and `NTFS_SETATTR_SIZE` now return `-EINVAL` on a symlink, which is what Linux gives for an operation a file type does not have; mode and timestamps stay settable. Checked first that nothing legitimate needs the path: macOS resolves symlinks above the extension, and `ntfs_symlink` builds its target through `do_create`. |

## Compressed writes: three data-loss bugs (2026-09-14)

Found by `core/tests/test_compress.c`, the first thing ever to write to a
compressed file through this driver. **All three report success to the caller**,
and all three are pinned by tests marked `BUG:` so that fixing one fails the
test that documents it. `ntfsck` is clean in every case: the results are legal
NTFS, just not what the writer asked for.

Together they meant **appending to a compressed file did not work at all**
through the public ABI. All three are fixed as of 2026-09-14 and appending is
covered end to end by `test_compress.c: append`: 40 sequential 7000-byte writes
at EOF, a size that is no factor of 64 KiB, so the run crosses a block boundary
at every offset within a block -- inside the allocation rounding, exactly on it,
and past it -- compared byte-for-byte after a remount.

| # | Where | Symptom |
|---|---|---|
| 9 **FIXED** (= U5) | `ntfs_compress_write`, `core/ntfs/compress.c` | Expands the attribute only when `pos + count > allocated_size`, never on `> data_size`. A compressed file's allocation is rounded up to the 64 KiB block, so a write into that rounding **returns success, re-encodes the block, writes it to disk, and leaves `i_size` unchanged**. The bytes are on the platter and permanently unreachable. Measured on a 102400-byte file with 131072 allocated: `ntfs_write(ni, buf, 8192, 102400)` returns 8192 and the file is still 102400 bytes. |
| 10 **FIXED** (= U6) | same | `initialized_size` is never advanced on the compressed path. When a write does pass `allocated_size`, `ntfs_attr_expand` moves `data_size` and nothing touches `initialized_size`. Confirmed on disk with `ntfsinfo`: `Data size: 201000 / Initialized size: 102400`. Everything past the old EOF reads back as zeroes, before and after a remount, though the compressed data is correctly written. |
| 11 **FIXED** (= U7) | `ntfs_write_cb`, same file | The `allzeroes` branch does `goto out` with `err = 0` **before** `ntfs_non_resident_attr_punch_hole`. Writing 64 KiB of zeroes over a block that holds data returns 65536 and the previous contents read back byte-for-byte after a remount. Correct and cheap when the block was already a hole; data loss when it was not. |

Minor, not pinned: `ntfs_compress_write` returns `int` but assigns a `size_t`
byte count, so a single write above 2 GiB would overflow the return.

What does work, verified byte-for-byte after remount and `ntfsck`-clean:
in-place overwrite of compressed files (compressible, incompressible via the
store-raw fallback, unaligned tails, writes straddling and spanning compression
blocks) and resident-to-non-resident growth. The sparse read, seek and
`fallocate` paths are consistent.

## Error handling: three findings from fault injection (2026-09-14)

Found by `core/tests/test_faults.c`, the first fault injection this project has
had. All three were pinned as bugs; all three are fixed as of 2026-09-14 and
every pin now asserts the correct behaviour instead. Each one was reproduced
first, inverted and watched to fail, then fixed.

| # | Where | Symptom | Status |
|---|---|---|---|
| 12 **FIXED** | `core/vfs/file.c` `ntfs_fsync`, `core/ntfs/super.c` `ntfs_sync_fs` | **A failed device flush is silently dropped.** With the barrier refused, `ntfs_fsync()` and `ntfs_volume_sync()` both return **0**. fsync's entire contract is broken: the caller is told the data is on stable storage while it sits in a volatile write cache, which is exactly the case a power cut then loses. Both call sites discard the return (`blkdev_issue_flush(...)` with no assignment). | **FIXED** 2026-09-14 by keeping the return at both call sites, with a `PORT:` comment at each. Two corrections to the finding as written. (a) The reason given for it being ours was wrong: `blkdev_issue_flush()` does not return `void` in a 7.1-era kernel, it returns `int`, and ext4 and xfs both check it -- so the dropped return came over verbatim from upstream and is upstream's bug as much as ours. The vendored tree carries only `fs/ntfs`, so this cannot be confirmed against `upstream/` here; it is the reason the divergence is marked `PORT:` rather than silently matched to upstream. (b) Implementing the `errseq_t` path instead would **not** have fixed it: `errseq_t` carries *writeback* errors (`mapping_set_error()` during page writeback), and a refused device barrier never enters it in Linux either. The port already has that path in the simpler per-mapping form -- `address_space.wb_err`, drained by `filemap_write_and_wait()` -- so `sync_blockdev()` already returns writeback errors and nothing was missing there. The live divergence in `docs/progress/platform-review.md` stays, with a note on its scope. |
| 13 **FIXED** | `core/vfs/super_glue.c` | **`ro_reason` is never set when errors force a read-only switch.** After every metadata write is refused the volume correctly drops to read-only, but `ntfs_volume_get_info()` reports `ro_reason = NTFS_RO_NONE`. `NTFS_RO_ERRORS` exists in the ABI for exactly this and nothing assigns it, so the menu bar tells the user their disk went read-only for no stated reason when the true reason is failing hardware and they should copy data off now. | **FIXED** 2026-09-14. The reason ladder runs once, inside `ntfs_mount()`, before anything can fail, so the fix is in `ntfs_volume_get_info()`: a volume that is read-only now with `ro_reason == NTFS_RO_NONE` can only have got there through `ntfs_handle_error()` (`errors=remount-ro`), which is `NTFS_RO_ERRORS` by definition. Checked that this is the only way: every other `s_flags \|= SB_RDONLY` in the tree runs inside `ntfs_fill_super()`, before the ladder, and the port always enters `fill_super` read-only so those branches are dead anyway. The reason is latched into the handle because the switch is one-way for the life of the mount, and a mount that was already read-only for a stated reason keeps that reason. Two changes on the Swift side, because the core reporting it is no use if nothing shows it: `MountStatus.reasonText(7)` now leads with the action ("Copy your files off this volume now, then check it in Windows with chkdsk /f") instead of naming the error, and `NTFSVolume.synchronize()` calls a new `noticeReadOnlySwitch()` -- nothing called back to the extension when the core switched, so the menu bar kept showing the mount-time status forever. A sync is the first thing that happens after a failed write. |
| 14 **FIXED** | mount path | **An I/O error at mount is reported as `-EINVAL`.** A read failure on the boot sector, on `$MFT` record 0, or on the root directory's record all return `-EINVAL`, the same errno as a genuinely non-NTFS partition, and the log says "Not an NTFS volume". A user with a dying disk or a flaky cable is told their partition is not NTFS, which is the sentence that makes people reformat. `-EIO` is the honest answer. `$MFTMirr` is the one case that behaves, returning `-EROFS`. | **FIXED** 2026-09-14. The errno is not lost in one place: `ntfs_fill_super()` funnels every failure through one `return -EINVAL`, and the helpers above it return `bool` (`load_system_files`) or `NULL` (`read_ntfs_boot_sector`), so no errno ever exists to lose. Rather than unwind those signatures through upstream-derived code, the *device* now says whether it failed: `struct ntfs_bdev` gained a sticky `io_err`, set by `ntfs_bdev_read()`/`ntfs_bdev_write()` when `ops->pread`/`pwrite` fails or transfers short, and `ntfs_mount()` clears it before each attempt. `ntfs_fill_super()`'s single exit then returns `-EIO` when the device reported an error during the attempt and `-EINVAL` otherwise, with a `PORT:` comment. Deliberately **not** set by the wrapper's own range check, which means the caller asked past the end of the device -- not that the device is failing. `-EINVAL` staying put for a partition that is not NTFS is the load-bearing half, because Disk Arbitration reads it as "offer this disk to another driver": `test_not_ntfs_is_einval` pins it over a zeroed image and over a FAT-shaped boot sector, both on a device that never fails a read, and it was confirmed to catch an over-broad fix (widening the condition to "any block device" makes both cases fail). |

Verified not to be problems: `ntfs_volume_sync` returning 0 after a data write
failure is correct, because fsync already reported it and errors report once; a
torn buffered write returning the full count is correct, and the following sync
does report `-EIO`. **No injected fault left a volume `ntfsck` called dirty**,
and no failed mount wrote a single byte, checked with a fingerprint of the whole
image before and after.

## Finding 15 **FIXED**: any sector size but 512 destroyed the volume on the first write

The most serious defect found on 2026-09-14, by `core/tests/test_geometry.c`.

A volume whose logical sector size is 1024, 2048 or 4096 bytes mounted, read
correctly, and is then destroyed by the first metadata write. One `mkdir` is
enough: MFT records 0 to 5 come back freshly formatted -- record 0 with
`link_count` 0 and `bytes_in_use` 0x50, record 5 all zeroes -- and `ntfsck`
then says `Failed to load $MFT(0), recover from $MFTMirr`. Reproduced
independently outside the test: format with `mkntfs -s 4096 -c 4096`, confirm
`ntfsck` clean, mount read-write, one `mkdir` returning 0, volume unmountable.

Isolated carefully:

* a read-only mount is clean; a read-write mount that changes nothing is clean;
  the first MFT record allocation is what does it
* **not** the block layer: forcing `logical_block_size` to 512 on a 4Kn volume
  corrupts it identically
* **not** the MFT record size: sector 512 with 1024-byte records is clean,
  sector 1024 with 1024-byte records is not
* the one variable is `vol->sector_size`, which mount hands to
  `sb_set_blocksize()` (`core/ntfs/super.c`)

**FIXED 2026-09-14** (commit 2f7adcc). It was **two** unit errors, both
upstream's, now U8 and U9 in the table above: bio sector numbers divided by the
filesystem block size instead of 512, and the MFT mirror offset dropping the
page index. A single `mkdir` produced 38 device writes stepping by 512 instead
of 4096, straight across `$MFT` records 0-5 -- which is why forcing
`logical_block_size` to 512 changed nothing (mount puts it back) and why the
record size alone looked innocent.

The read-only guard in `super_glue.c` is gone and `write_clean` is true for
every geometry in `test_geometry.c`, so the suite tests the real thing rather
than the protection: 337 checks became 672, including byte-level assertions on
`$MFT` records 0-5 and on `$MFTMirr` against `$MFT`.

Verified three ways, because removing that guard exposes real hardware. At
512/512, 1024/1024, 2048/2048, 4096/4096 and 4096/64K: payload written through
the driver, unmounted, **remounted**, read back byte-for-byte identical, with
`ntfsck -n` clean each time. Then read again with ntfsprogs' own `ntfsls` and
`ntfscat`, a different implementation, which extracted the same bytes -- so this
is not our reader agreeing with our writer. Reverting both helpers to upstream's
shift produces 35 failures including the byte-level MFT assertions.

**Who this affected.** Fewer disks than it appears. NTFS records only the
logical sector size, so a 512e disk -- physically 4 KiB, logically 512, which is
most external SSDs -- is byte-identical to a 512n one and was never at risk. The
exposure was true 4Kn disks and volumes deliberately formatted with a larger
sector. `test_geometry.c` also pins a 512-sector volume on a 4 KiB-block device
as refused at mount with `-EINVAL`, which is the other half of that story.

## Finding 16 **FIXED**: an unaligned load in the LZ77 encoder

`ntfs_hash()` in `core/ntfs/compress.c` read `*(const u32 *)p` from an unaligned
pointer, with an upstream comment saying unaligned access is allowed. It is
allowed by the hardware and it is still undefined behaviour by the C standard,
so UBSan reports it -- and once `ctest under UBSan` became a CI stage on
2026-09-14 with `-fno-sanitize-recover`, it aborted `test_compress` and turned
the pipeline red.

**FIXED** the same day with `get_unaligned_le32()`, which is the memcpy form the
compiler folds back into a single load, so it costs nothing and makes the
little-endian assumption explicit rather than implied. Upstream bug, `PORT:`
comment at the divergence.

Worth noting how this was found: the agent that met it said plainly that it had
left it alone and not recorded it. It surfaced only because the whole set of
agent reports was re-read against what had actually been done.

## Finding 17 **FIXED** (one property unproven): journal replay was unreachable, and misreported a refused barrier

Found 2026-09-14 on the first real dirty volume this feature has ever met: a
Windows 10 v2.0 journal on a USB stick. Two distinct defects, one fixed.

### 17a: the one-shot request was eaten by the read-only probe **FIXED**

Disk Arbitration loads the extension **twice** for one mount: a probe instance
with a read-only handle, then a serving instance with a writable one, in a
different process. `loadResource` consumed the app's one-shot replay request in
whichever arrived first. That was always the probe, which then refused the very
work it had claimed, because `ntfs_logfile_replay_device()` returns `-EROFS` on
a read-only device. The serving instance found nothing pending.

Measured: pid 41749 `(ro)` took the request; pid 41750, the one that could have
acted, never saw it. So **the Replay Journal button had never done anything, on
any volume, ever** -- and silently, because the refusal was logged by an
instance that was about to be torn down.

Fixed in `NTFSFileSystem.loadResource`: one-shot requests are only consumed on a
writable handle. The hibernation discard request had the identical bug and the
identical fix. Verified: the writable instance now takes the request and the
core logs its result.

### 17b: a failed *barrier* was reported as a part-way failure — **FIXED**, one part still unproven

Corrected 2026-09-14, a few minutes after it was first written here. The first
account said replay "failed part-way" and "left the volume modified", implying
writes were abandoned mid-flush. **That was wrong**, and the correction matters
because the original reading was considerably more alarming than the truth.

The `-EIO` did not come from a write. It came from the final barrier:

    [bdev] flush: Error Domain=NSPOSIXErrorDomain Code=5 "Input/output error"

`ap_sync` -> `bdev_sync` -> `fskit_flush` -> `metadataFlushWithError:`. Every
redo write had already landed. `chkdsk /f` on the volume afterwards reported no
problems, which corroborates that the replay completed correctly rather than
half way.

And the flush failure is not specific to replay: **20 flush errors in 45 minutes**
on that USB stick. This is the same defect recorded during the performance work
-- `metadataFlush` fails on USB devices on this machine and succeeds on
disk-image-backed ones -- which was deliberately left alone at the time because
"whether a device that refuses to flush should fail the operation or merely warn
is a data-integrity decision for the user". It now also makes a successful
replay report itself as a possible corruption.

Both defects below were fixed on 2026-09-15; the third item is not a defect but
an unproven property, and it is the only part of this finding still open.

The real defects here were:

1. **The message was wrong and frightening.** A failed barrier means "this may
   not be durable yet", not "your volume may be inconsistent; run chkdsk".
   **FIXED**: `struct ntfs_log_replay_result` gained `flush_failed`, set only
   when every write succeeded and the barrier did not, and the repair layer now
   says the replay was written but could not be confirmed, the journal is
   unchanged, and the work will be redone. Covered by
   `test_flush_failure_is_not_a_partial_write`.
2. **The underlying flush failure was unaddressed. FIXED**, and it was ours:
   `FSResource.h` says `metadataFlushWithError:` flushes what was written with
   `delayedMetadataWriteFrom:`, which this bridge never calls -- every write goes
   through the direct `writeFrom:` path. We were flushing a buffer cache we never
   write to, and failing operations on it. The call is kept (correct for the
   delayed path, free), its failure is logged once per device and not
   propagated, which is what Apple's own FSKit FAT driver does. The cost is
   stated in `fskit/README.md`: FSKit exposes no barrier, so `fsync` cannot
   promise the data has left the device's write cache.
3. **Atomicity is still unproven — the one part of this finding still open.** Nothing here demonstrated a partial write,
   but nothing rules it out either: the flush writes records one at a time, so a
   genuine write error mid-way would leave exactly the state this finding
   originally described. The overlay protects the planning phase only.

What is now known to work, on a real Windows 10 v2.0 journal: the request
reaches the writable instance, the dry run accounts for every record, the writes
land, and Windows subsequently finds the volume clean. What is still unknown is
whether the *content* of those writes is correct -- chkdsk validates structure,
not intent -- and that remains the open part of phase 4.

## Finding 18 **FIXED**: replay refused a record Windows accepts

The first end-to-end phase 4 ground-truth run, 2026-09-15, on a real Windows 10
dirty v2.0 journal (`tools/phase4-capture/captures/run2-dirty.img`, kept).

**The good half.** Our analysis identified exactly the right work. It planned
`InitializeFileRecordSegment` on MFT records 43 and 44; before replay both were
all zeros, and after Windows replayed the same journal record 43 was an in-use
**directory** and record 44 an in-use **file** -- matching the directory and file
created on Windows before the stick was yanked. The engine's understanding of
this journal is correct.

**The bad half.** We never applied any of it. The dry run refused:

    redo: malformed client record at lsn 0x204c8d:
    redo data runs past the end of the record

and `needs_chkdsk` was set, so `--apply` correctly wrote nothing -- verified by
diffing our output against the input: **0 changed regions**. That refusal is also
the `-22` the driver reports when mounting this volume.

**The record, exactly:**

    redo_off 40  redo_len 4  undo_off 40  undo_len 0  data_len 40  lcns 1  target 0x18

`NR_HEADER_SIZE` (32) + `8 * lcns` (8) = 40, so the redo payload begins
immediately after the LCN array, which is sensible framing. But `data_len` says
the record ends at 40, so `40 + 4 > 40` and `lfs_check_client_rec()` rejects it.

**Windows replayed this record without complaint**, so the record is not
malformed -- our validator is too strict, or our `data_len` is short by at least
four bytes.

Two candidate causes, not yet distinguished:

1. **Our reassembly truncates a record that spans a page boundary.**
   `docs/LOGFILE.md` §5 already lists "v2.0 multi-page tail transfers --
   simplified vs fslog.c (page-by-page through `file_off`), inferred" as a known
   simplification. This is the shape that simplification would produce.
2. **Windows' own framing permits redo data beyond the declared length**, and
   reads it from the page buffer without checking. Then the check itself is
   wrong rather than the input.

### Fixed 2026-09-15

`data_len` turned out to be Windows' own `client_data_length`, read straight
from the record header, so the first candidate (our reassembly truncating a
page-spanning record) was **ruled out**. Windows simply writes records whose
redo payload starts at or past the length it declared, and reads that payload
from the page buffer without bounding it by the declared length.

**The first attempt at this fix was wrong, and the way it was caught is worth
keeping.** It extended the buffer and *read* the extra bytes from the page. The
record at `0x204c8d` is 88 bytes -- 48 of header plus the 40 it declares -- and
the next record begins at lsn `0x204c98`, exactly 88 bytes later, so those extra
bytes are the next record's header. Replay duly wrote `0x00204c98`, that
record's own LSN, into the user's file `d/i.txt`, where Windows had written four
zero bytes. Every summary check passed: the directory appeared, the MFT records
matched Windows on every field, `ntfsck` called the volume clean. Only reading
the file's contents showed it.

The payload is not there to be read. **The extension is zero-filled**, which
reproduces Windows byte for byte and never touches a neighbouring record.

The fix reads as far as the record actually reaches instead of as far as it says
it is: `struct lfs_record` gains `data_avail`, the read path sizes the buffer to
`max(client_data_length, redo_off + redo_len, undo_off + undo_len)` under the
same log-size bound the declared length already had, and the two payload checks
bound against `data_avail` rather than `data_len`. The check is not relaxed --
it becomes true by construction, and still refuses anything the log could not
hold.

**Result on the real journal.** The plan went from 6 operations to 8: the record
at `0x204c8d` decodes as `UpdateResidentValue` on MFT record 44, and `0x204cae`
as `UpdateFileNameRoot` on record 43. `--apply` then wrote 2 MFT records and 2
clusters and rewrote the restart pages clean, and `ntfsck -n` calls the result
clean.

**Compared against Windows' replay of the same journal**, our output matches:

| | ours | Windows |
|---|---|---|
| record 43 | seq 1, links 1, in-use **dir**, 432 bytes used | identical |
| record 44 | seq 1, links 1, in-use **file**, 296 bytes used | identical |

Record 44 is the decisive one: **only 2 bytes differ, at +0x1fe and +0x3fe**,
which are the two update-sequence fixup slots and differ on every write by
definition. Everything else is byte-identical, including the `$LogFile` LSN
stamp of `0x204c8d`. `d/i.txt` reads back `00 00 00 00` on both. Record 43 differs more because Windows mounted the volume after
replaying and wrote the directory again -- its LSN advanced to 0x20508b and its
USN bumped from 2 to 3, which is ordinary mount activity rather than a
disagreement about the journal.

**Do not simply delete the check.** It exists to stop a read running off the end
of a buffer. The fix has to either recover the missing bytes (if they are in the
page) or treat the payload as absent rather than failing the entire replay.

Also fixed here, and worth keeping regardless: `lfs_check_client_rec()` now
reports **which** of its six checks failed. It previously said only "malformed
client record", which was the sum total of the evidence for a rejection that
turned out to be wrong.

## Finding 19 **FIXED**: replay refused an index block it had written itself

Phase 4 scenario C, 2026-09-17. The first capture to exercise directory index
recovery, and it found a real limit.

**The capture.** 25 deletes and 25 creates issued against an 80-entry directory
on Windows 10, with the stick pulled mid-loop. 448 records, ten operation types,
including 28 `AddIndexEntryAllocation` and 26 `DeleteIndexEntryAllocation` --
against scenario A's three types. Saved as `scenC2-dirty.img` with its metadata.

**What we do.** Replay applies 148 records and then refuses:

    lsn 0x1806de5: index block torn

The record is `AddIndexEntryAllocation`, a 120-byte insert at offset 0x478 into
the index block at VCN 8. The block fails its update-sequence check, so its
contents cannot be trusted, and patching 120 bytes into an untrustworthy block
is refused. Nothing is written.

**What Windows does.** Applies all 50 operations. The directory comes back with
55 originals and 25 new entries, 80 total, and the journal clean. Windows
recovers a torn index block; we do not.

**The naive fix is worse than the refusal, which is the useful part.** Tolerating
the failed fixup and applying the insert anyway was tried: it produces **79
entries where Windows produces 80** -- one `zz-new-entry` missing. A silently
incomplete directory is worse than a loud refusal, so the check stays.

### Fixed 2026-09-17

**The block was not torn.** On disk, `attr 0x68 vcn 8` (lcn 190055) is **all
zeros** -- a cluster Windows had allocated and never written. The record at that
page's own `oldest_lsn`, `UpdateNonresidentValue`/2000, lays down its `INDX`
header. The *next* record reads the block back out of the replay's overlay, sees
`INDX`, checks the update sequence array, and finds no USNs -- because nothing
ever wrote them.

The cause is one line of asymmetry. `deprotected` was set only when a block
**arrived** as `INDX` from disk, and the outgoing `lfs_fixup_pre_write()` was
gated on it. A record that is the first thing ever written into a fresh cluster
therefore produced an `INDX` block with no update sequence array, and every
later read of it failed.

The fix protects on the **result** rather than the origin: if what we are about
to store is an index block, it gets its update sequence array, whatever was
there before.

**Two wrong turns on the way, both recorded because they were instructive.**
Tolerating the failed check gave 79 entries of 80 with one filename truncated.
Skipping the *inbound* deprotection for blocks already dirty in the overlay was
worse still -- names came back as `a-deliberately-long-fie-name` and
`...-to-fill-"ndex-blocks-78.txt`, which is USN bytes left sitting in the data.
Both confirmed that the asymmetry, not the check, was the fault.

**Result: byte-identical to Windows.** 80 entries, 25 of them new, `ntfsck`
clean, from the same journal Windows replayed. Scenario A still reproduces
Windows exactly, so the fix is not a trade.

**What this used to mean.** Our replay was not correct for index-heavy crash
recovery. The likely gap is dirty-page handling: a torn page has to be rebuilt by
replaying every update to it from its own `oldest_lsn` in the dirty page table,
rather than by patching whatever is on disk. We start from a single global
`redo_lsn` (0x1804af9 here) and treat the on-disk block as a base, which is only
valid when the block is intact.

Until this is understood, replay of a crash that caught a directory index
mid-update will refuse rather than half-apply. That is the right failure, and it
is why replay stays opt-in.

## Finding 20 **FIXED**: our symlinks were correct, and Windows could not follow them

Measured on Windows 10, 2026-09-17, against the phase 2 test tree.

**Hard links are fine.** `fsutil hardlink list` returns all four paths for a file
this driver hard-linked three times. Nothing to do.

**Extended attributes are fine.** `dir /r` shows them as ordinary NTFS alternate
data streams -- `hardlink-0.txt:com.apple.provenance:$DATA`, 11 bytes -- so the
xattr-to-ADS mapping round-trips to Windows correctly.

**Symlinks are structurally correct and practically unusable on Windows:**

    fsutil reparsepoint query symlink-relative
    Reparse Tag Value : 0xa000001d
    Reparse Data: 02 00 00 00 "link-target.txt"

`0xA000001D` is `IO_REPARSE_TAG_LX_SYMLINK` -- the **WSL** symlink tag, version 2,
target stored as UTF-8. The record is well formed and `chkdsk` accepts it. But:

* `type symlink-relative` fails with "The file cannot be accessed by the system".
  Native Windows does not follow WSL symlinks; only WSL does.
* plain `dir` does not list them at all. `dir /a` does. That is ours:
  `core/vfs/namei.c` sets `FILE_ATTR_SYSTEM` on every non-regular, non-directory
  inode, and `dir` hides system files by default.

**Two separable questions, and neither should be changed casually.**

*The tag.* **The first version of this paragraph was wrong, and the error was
load-bearing.** It said the WSL tag "is what Linux's ntfs3 and ntfs-3g write",
and used that to argue for keeping it as round-trip fidelity. Read from source
on 2026-09-17, neither half is true:

* **Linux ntfs3 writes the native tag**, `IO_REPARSE_TAG_SYMLINK` (0xA000000C),
  always, with no option (`fs/ntfs3/inode.c:1336`). Its `readlink` has **no
  case for the WSL tag** -- it falls to "Unknown Microsoft Tag" (`inode.c:2220`)
  and returns `-EINVAL`. So on an ntfs3 mount our symlinks stat as symlinks and
  cannot be read. That is a broken link, not fidelity.
* **ntfs-3g writes no reparse point at all by default.** It writes an *Interix*
  file: `FILE_ATTR_SYSTEM` plus a `$DATA` stream holding the magic `IntxLNK\1`
  and a UTF-16 target. It writes the WSL tag only under `-o special_files=wsl`,
  and its own man page says of both modes: *"Neither mode are interoperable with
  Windows."* Our driver cannot read Interix links at all -- a separate gap.
* ntfsprogs-plus 1.0.0 (local, `libntfs/reparse.c:1354`) and the vendored
  ntfsplus (`upstream/.../reparse.c:519`) do write the WSL tag. That is where we
  inherited it, and it is the minority position.

The corrected matrix: **`IO_REPARSE_TAG_SYMLINK` is followed by every reader
that exists** -- Windows, WSL, ntfs3, ntfs-3g, and our own native-read branch in
`core/vfs/api.c` (exercised for the first time during this research; it works).
`IO_REPARSE_TAG_LX_SYMLINK` is followed by WSL, ntfs-3g and us, refused by
Windows, and broken on ntfs3.

So the argument for the current tag does not survive contact with the source.
The real cost of switching is narrower than "fidelity": POSIX targets containing
`: * ? " < > |` cannot be expressed natively, and a Unix-absolute target like
`/usr/bin/foo` would be written the way ntfs3 writes it (`\usr\bin\foo`,
RELATIVE), which Windows resolves against the drive root -- wrong target, but a
well-formed link that was unopenable before.

**Microsoft itself chooses per link.** WSL's DrvFs writes a native symlink when
the target is relative and exists, and the WSL tag otherwise (documented, WSL
release notes build 17046). That is the strongest precedent for a per-link rule.

### Fixed 2026-09-17, and measured on Windows the same day

Symlinks are now written with `IO_REPARSE_TAG_SYMLINK` whenever the target can
be expressed in it, and with the WSL tag otherwise (commit 5355969). Verified on
the Windows 10 machine against a set written through the real FSKit path:

    dir
      <SYMLINK>   n-deep    [sub\deep.txt]
      <SYMLINK>   n-dir     [dir1]
      <SYMLINK>   n-rel     [target.txt]
      <SYMLINK>   n-unixabs [\usr\bin\foo]
      <JUNCTION>  w-bslash  [...]
      <JUNCTION>  w-colon   [...]
      <JUNCTION>  w-star    [...]

    type n-rel   -> the target
    type n-deep  -> deep target

    fsutil reparsepoint query n-rel
      Reparse Tag Value : 0xa000000c   Tag value: Symbolic Link
      00 00 14 00 14 00 14 00 01 00 00 00 t.a.r.g.e.t...t.x.t. t.a.r.g.e.t...t.x.t.

    chkdsk /f -> found no problems. 10 reparse records processed.

The three unknowns, answered:

1. **Windows follows a native symlink that also carries our WSL `$EA`.** `type`
   opens the target through it. The EA coexistence was not a blocker, so the
   EAs stay and nothing has to be dropped for `S_ISLNK`.
2. **`chkdsk` accepts the native tag with our `$Reparse` index layout.** No
   problems, and the reparse count is exactly right.
3. **A `\`-rooted RELATIVE target is a well-formed link.** `n-unixabs` lists as
   `<SYMLINK> [\usr\bin\foo]`. Nothing there to open, but it is a link, where
   before it was unopenable and hidden.

Also learned: a *file*-type symlink whose target is a directory (`n-dir`) lists
as `<SYMLINK>`, not `<SYMLINKD>`, and `dir n-dir` shows the link itself rather
than the directory contents. Windows distinguishes the two at creation and we do
not. Harmless for reading; worth a follow-up if `cd n-dir` matters to anyone.

The plain `dir` listing shows the native links unhidden -- that is the
`FILE_ATTR_SYSTEM` fix landing. The three WSL-tagged links still render as
`<JUNCTION> [...]`, which is Windows' way of saying "a reparse point I do not
follow"; those targets could not have been expressed natively, so that is the
intended outcome, not a defect.

**One procedural lesson worth more than the rest.** The first stick handed to
Windows carried WSL tags despite the new code, because the mount was served by
a stale extension instance that `fskitd` had kept resident across the reinstall.
It was diagnosed only after Windows reported the wrong tag. The tags should have
been read back from the MFT through the mount before the stick left the desk --
that takes ten seconds and needs no root -- and now they are.

What used to be here: "Three things must be measured on Windows before any
switch, none of them in the spec: whether Windows follows a native symlink whose file also carries the WSL
`$EA` we write; whether `chkdsk` accepts our `$Reparse` index layout under the new
tag; and what Explorer does with a `\`-rooted RELATIVE target. Also required
first: our `getattr` reports **size 0** for native symlinks after a remount,
because `ni->target` is only populated for the WSL tag. That must be fixed
before we write any, or every link we make will stat at zero bytes.

*The SYSTEM attribute.* Making symlinks invisible to `dir` is a smaller and more
clearly unhelpful side effect. Upstream sets it for device nodes and FIFOs, where
hiding them is reasonable; a symlink is an ordinary thing a user expects to see.
Dropping `FILE_ATTR_SYSTEM` for `S_ISLNK` specifically would cost nothing and is
worth doing on its own.

**User-visible today:** copy a tree containing symlinks from macOS to an NTFS
disk, open it on Windows, and the symlinks are invisible in Explorer and
unopenable from the command line. The data they point at is fine; the links are
not usable.

## Finding 21 **FIXED**: Windows directory symlinks and junctions read as empty folders

Inherited from upstream. `core/vfs/inode.c` decided an inode's type by
`MFT_RECORD_IS_DIRECTORY` before looking at the reparse point, and looked at the
reparse point only for file records. A `mklink /D` symlink or a `mklink /J`
junction is a directory record *with* a reparse point, so every one of them
loaded as an empty directory -- including the junctions Windows plants on every
system disk (`Documents and Settings` -> `Users`, `Application Data`, and so on).
`ntfs_reparse_tag_mode()` also had no case for `IO_REPARSE_TAG_MOUNT_POINT`, so
even a reparse-first check would have called a junction "unknown".

ntfs3, ntfs-3g and WSL all present them as symlinks. ntfsplus fixed the same bug
out of tree in June 2026 (commit 5e47664782 and two follow-ups); mainline v7.1
still has it.

**The trap the research found by trying it.** The obvious fix -- move the reparse
check ahead of the directory check -- makes every junction *unreadable*: a
junction tag mapped to mode 0, mode 0 falls into the file branch, the file
branch demands an unnamed `$DATA`, and the whole inode load fails on "$DATA
attribute is missing". The `$INDEX_ROOT` branch must stay keyed on the record
flag; only the *mode* decision moves. Done that way in `e1200b9`.

**Also fixed in the same change**, because the decoder had to be rewritten:

* An absolute native symlink (`\??\C:\target`) was returned as `/target` -- a
  host-absolute path against the Mac's root -- by the decoder written that
  morning. A volume-absolute target now drops its meaningless drive letter, and
  `readlink` prefixes one `../` per level between the link and the volume root,
  computed on demand from `$FILE_NAME` parent references. A junction one level
  deep to `\??\C:\sub` reads as `../sub`. `Volume{GUID}` and UNC targets are
  returned verbatim with slashes flipped: honest and dangling.
* The decoder read the PrintName; Windows reparses with the SubstituteName, and
  a `mountvol` mount point has an *empty* PrintName. SubstituteName first now.
* `readdir` typed a directory-record link as `DT_DIR` while `stat` said
  `S_IFLNK`. Reparse-first now, taking the tag from the index entry (which is
  what Windows' own `FindFirstFile` serves) and opening the inode only when that
  reads zero.
* `ntfs_vfs_link` refused hard links on `S_ISDIR` alone; a junction presented as
  `S_IFLNK` would have slipped past, and NTFS forbids hard-linking a directory
  record. A new `NI_DirRecord` inode flag, set at load, guards it.

**The sharp edge is safe.** `unlink` on a junction removes only the junction: the
emptiness check keys on the record flag, not the presented mode, so the target
and its contents survive. `rmdir` gets `ENOTDIR` from the kernel. Verified on a
byte-patched image with `ntfsck` clean afterwards.

**Test.** Nothing on macOS can create a junction, so
`tools/mkfixtures/patch_reparse.py` (the research agent's byte-patcher, checked
in) rewrites directories made by our driver into the exact bytes Windows leaves.
`test_windows_dir_links` covers both tags, relative and absolute, a
`Volume{GUID}`, a junction one level deep, and a cross-volume one, plus the
delete and hard-link edges. Restoring the directory-first check fails 26
assertions with "mode 40755 is not S_IFLNK".

**Verified against real Windows-made links, 2026-09-18.** The fixture above was
built from the spec and from reading other implementations; this is the same
read path against what Windows 10 actually wrote on the stick:

| Windows made | Mac sees | resolves |
|---|---|---|
| `mklink /J j-dir dir1` | link -> `../rt2/dir1` | yes |
| `mklink /D s-dir dir1` | link -> `dir1` | yes |
| `mklink /D s-abs F:\rt2\dir1` | link -> `../rt2/dir1` | yes |
| `mklink s-file target.txt` | link -> `target.txt` | yes |

The two absolute ones exercise the drive-letter drop and the `../`-per-level
prefix on real bytes. `cd` through the junction lands in `/rt2/dir1`; `find
-type l` lists all of them, so `stat` and `readdir` agree; `st_size` equals the
`readlink` length. `rm` on the real junction and the real absolute directory
symlink removed the link and left `dir1/file.txt` intact; `rmdir` refused with
ENOTDIR. All through the FSKit mount.

## Finding 22 **FIXED**: a symlink to a directory was written as a file-type link

Windows has two kinds of symbolic link and demands the choice at creation
(`mklink` vs `mklink /D`); POSIX has one. We always wrote file-type. Windows
followed those to a directory, but listed them as `<SYMLINK>` rather than
`<SYMLINKD>`, and `cd` through one from cmd failed (measured 2026-09-17).

Now, as WSL's DrvFs does (release notes, build 17046: the target "must be
relative, must not cross any mount points or symlinks, and must exist"), the
target is resolved at `symlink(2)` time and a directory record is written --
`MFT_RECORD_IS_DIRECTORY`, empty `$I30`, no unnamed `$DATA`, the index-present
bit in every `$FILE_NAME` -- when it names an existing directory here by a
relative path through no other link. Everything else stays file-type, which is
what every open implementation writes. Verified on Windows 10: `<SYMLINKD>`,
`dir d-link` lists the target's contents, `cd d-link` works, `chkdsk /f` clean.

Limits stated up front: a link created before its target (tar, git checkout,
cp -R ordering) is file-type, and a later rename of the target is not tracked.
Windows does not track it either. `88afa50`.

## Finding 23 **FIXED**: `$FILE_NAME`'s union held the EA size where the reparse tag belongs

`$FILE_NAME` has a four-byte union: packed EA size for an ordinary file, the
reparse tag for a reparse point. Upstream wrote the EA size unconditionally, so
the MFT-resident copy of every symlink we created claimed a reparse tag of
`0x2d` (45, the packed size of `$LXUID`/`$LXGID`/`$LXMOD`). The index copy was
corrected later by `ntfs_inode_sync_filename()`, which reads the real tag from
the attribute. So the two on-disk copies of one name disagreed. `ntfsinfo`
prints the MFT copy as `Reparse point tag: 0x0000002d`; `chkdsk` never counted
it as a problem.

**How it surfaced.** After a Windows round trip on 2026-09-17 -- `chkdsk /f`,
then a read-write session on the Mac -- symlinks on the test stick that had
listed as `<SYMLINK> [target.txt]` listed as zero-byte plain files, while links
created minutes earlier by the new build listed correctly. Windows reads the tag
from the directory index; the disagreement had been resolved in favour of the
wrong copy. Not data loss -- the reparse attribute was intact -- but Explorer and
`dir` stopped recognising every link on the volume.

**What is proven and what is not.** The MFT copy being wrong is measured. The
index copy could *not* be made to degrade through our own API: stat, readlink,
create, unlink, mkdir, a second symlink and setxattr all leave it correct. That
points at the Windows side of the trip reconciling from the MFT, most likely
`chkdsk`; the exact step is unproven and stays so. The root defect is not in
doubt, and writing the tag at create time removes the inconsistency that
anything could resolve the wrong way.

`test_symlink_filename_union_is_the_tag` reads the MFT copy back with `ntfsinfo`;
restoring the unconditional EA-size write fails it with "reads 0x0000002d".
`d95d728`.

### Confirmed, and the real cause found (2026-09-18)

The union fix above was necessary and **not sufficient**: links written by
`d95d728` -- both copies of the union correct -- degraded on the very next
round trip. Reading the damaged index block off the stick gave the answer:

    index entry union: 2d 00 0f 00   (packed_ea_size 0x2d, reserved 0x0f)
    MFT copies:        intact, tag 0xa000000c throughout

Nothing in this driver writes the EA form into an index entry. **`chkdsk`
does.** [MS-FSA] states that reparse points and extended attributes are
mutually exclusive; `chkdsk` recomputes each index entry's duplicated
`$FILE_NAME` from the MFT record, sees `$EA_INFORMATION`, and writes the EA
form of the union over the tag. By its rules the file cannot be a reparse
point, so it reports no problems. Windows then reads a reparse flag with an
unrecognisable tag and lists a zero-byte file.

Every symlink we ever wrote carried `$LXUID`/`$LXGID`/`$LXMOD`, because
upstream writes them on every inode. The research had flagged the coexistence
as outside the spec; it was set aside because Windows *follows* such links.
It does. `chkdsk` is what objects.

Fix, `e045c2e`: no `$EA` on symlinks, as Linux ntfs3 does. A symlink's mode is
0777 by definition and uid/gid are mount-wide under noowners, so nothing is
lost. Verified on Windows 10 with the exact sequence that had broken three
previous sets -- `dir`, `chkdsk /f`, a Mac read-write session in the same
directory, `dir` again: `<SYMLINKD>`, `<SYMLINK>`, `<SYMLINK>` all intact.

This also explains the very first observation in this thread: the 500 GB
disk's links listing as zero-byte files on 2026-09-17, after its `chkdsk /f`
three days earlier.

**Migration.** Links created by builds before `e045c2e` still carry `$EA` and
the next `chkdsk` will still rewrite their index entries. The reparse
attribute survives, so they still work on the Mac; only Windows' listing is
affected. A repair pass that strips `$EA` from existing symlinks would fix
them; not written.

