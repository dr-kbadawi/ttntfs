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

## Finding 15: any sector size but 512 destroys the volume on the first write

The most serious defect found on 2026-09-14, by `core/tests/test_geometry.c`.
**Mitigated, not fixed.**

A volume whose logical sector size is 1024, 2048 or 4096 bytes mounts, reads
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

**Mitigation, 2026-09-14:** `super_glue.c` now refuses a read-write mount of any
volume whose sector size is not 512, reporting `NTFS_RO_UNSUPPORTED`. Reading
stays available, which is the point of the read-only fallback. The volume is
left untouched, verified by `ntfsck` after a refused mount. Removing the guard
makes `test_geometry` fail in 4 places, so it is load-bearing rather than
decorative.

That is a safety net over a write path that is still wrong. Fixing it properly
means finding why a non-512 `sector_size` corrupts the MFT during allocation;
when that is done, drop the guard and flip `write_clean` in `test_geometry.c`,
which fails until both are done.

**Who this affects.** Not as many disks as it first appears. NTFS records only
the logical sector size, so a 512e disk -- physically 4 KiB, logically 512, which
is most external SSDs -- is byte-identical to a 512n one and is unaffected. The
exposure is true 4Kn disks, and volumes deliberately formatted with a larger
sector size. `test_geometry.c` pins a 512-sector volume on a 4 KiB-block device
as refused at mount with `-EINVAL`, which is the other half of that story.

## Finding 16: an unaligned load in the LZ77 encoder

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

