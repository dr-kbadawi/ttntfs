# Testing

What is checked, what is not, and why. A filesystem driver that is wrong loses
data silently, so the question this document tries to answer is not "do the
tests pass" but "what would still get through".

Every suite here was written on 2026-09-14 against code that had shipped
without it, and between them they found twelve defects, four of which lose data
silently. That is the argument for the rest of this document.

Run everything that does not need hardware or a signing certificate:

```
tools/ci.sh              # all stages
tools/ci.sh --quick      # skip the fixture suite (the slow one)
tools/ci.sh --list       # what it will run
```

## 1. The suites

| suite | what it covers | run by |
|---|---|---|
| `ctest` (16 targets, ~4,800 checks) | the platform layer, mount decisions, `$LogFile`, compressed and sparse writes, links and names, large directories, crash consistency, fault injection, geometry, the cluster allocator, the enable script | `ctest --test-dir build` |
| `tools/run-tests.sh` (265 checks) | read and write against 7 NTFS fixtures, compared with ntfsprogs as ground truth, then structurally checked | `tools/run-tests.sh all` |
| `NTFSTests` (128 tests) | the app's Swift logic: option parsing, status decoding, note expiry, name alternatives, turning the extension on (the module list, when System Settings takes over), how the menu reads its state, when to ask for a star, the update check | `xcodebuild test -scheme NTFSTests` |
| `fskit/scripts/mount-test.sh` | a real FSKit mount, read-write, end to end, then `fsck` on the result | by hand |

### ctest, in detail

* **`platform_inode`** — the inode table: `igrab`/`iput` races, eviction, the
  write-back ordering (`test_sync_order`, which is the one that caught a real
  regression the day it was written).
* **`platform_bdev`** — the block device: read/write, read-only handling, the
  sub-block read-modify-write race, the work queue, and the `bd_mapping`
  contract that every bdev must satisfy.
* **`pagecache`, `pagecache_stress`** — folio lifecycle, writeback visibility,
  the lookup-versus-invalidate race, reclaim under a cap.
* **`mount`** (313) — the `$LogFile` clean rule, the read-only reason, a
  read-write mount with a non-empty journal, the fast probe against the full
  one, the device-flush count, and the unmount of a read-only volume. That
  unmount must write nothing, except the dirty flag after an error
  (finding 33), and neither may a refused mount or `ntfs_probe()`. A volume
  with errors that stays writable must stay marked dirty too (U23), and a
  failed discard of the hibernation image must leave the volume read-only
  (finding 34). Mount Read-Write Anyway (`NTFS_MOUNT_CLEAR_DIRTY`) must clear
  the dirty flag in both copies and mount read-write when that flag is the one
  reason, write nothing when anything else keeps the volume read-only or the
  switch to read-write is refused, and leave the volume dirty when the cleared
  flag's own write fails.
* **`links`** (559) — hard links, symlinks through reparse points, the 100 ns
  timestamp conversion, and permissions from the mount masks.
* **`names`** (448) — the three name and xattr decisions in PORTING.md §6:
  Windows-illegal names refused, case-insensitive and case-preserving lookup
  through a real `$UpCase` table, and xattrs as alternate data streams.
* **`compress`** (675) — compressed writes, including appending across
  compression-block boundaries, a block rewritten where no free run can hold
  it (finding 30), and rewrites that fail and must leave the old block: for
  want of space, on a write error (finding 32), or because the new mapping
  pairs cannot be written (U22). Also the sparse read, seek and `fallocate`
  paths.
* **`bigdir`** (480) — the index B-tree: growth to four levels, deletion in
  creation, reverse and shuffled order, and six collation shapes.
* **`crash`** (253) — the write ordering, asserted by mapping a sync's device
  writes back to `$MFT`/`$Bitmap`, then 55 interrupted operations checked with
  `ntfsck`.
* **`faults`** (417) — injected read, write and flush failures at fourteen
  points, plus volumes filled to `ENOSPC`. One is filled with data. Two are
  filled to their last cluster, then hit by new names, and must stay writable
  afterwards and pass a check after a remount (finding 26). One more, also
  full, checks that a create which fails on a full folder leaves its MFT
  record safe to give to the next file (finding 27), and that a record freed
  on a full volume can be given out again (finding 28). A write into a hole
  whose data the device refuses must leave the hole a hole on the disk and in
  the page cache, without losing an earlier write in the same page (finding 29),
  and an allocation whose mapping pairs cannot be written must not stay in the
  run list (U20).
* **`geometry`** (1271) — 4Kn sectors, cluster sizes either side of the sector
  size, a 16 TiB sparse volume, and writes into holes, which must read back
  zeroes around them after a remount on every cluster size, and give their
  clusters back when they fail with `ENOSPC` (finding 29). Runs a
  full read-write cycle per geometry and compares `$MFT` records 0-5 and
  `$MFTMirr` byte for byte afterwards; this
  is what found finding 15 and what pins it fixed.
* **`alloc`** (104) — the cluster allocator and upstream's fixes to it (U13-U16).
  A 2 MB root folder filled with names, which must not stop while clusters the
  allocator could use are free. Four requests made straight to
  `ntfs_cluster_alloc()` over a `$Bitmap` crafted on the image, because a
  contiguous request's second run cannot be seen through the ABI. A
  `fallocate()` over a hole whose clusters hold a deleted file's bytes, read
  back after a remount: a guard that passes on 0.4.2 too (U14 explains why).
  Freeing two runs while `$Bitmap` cannot be read, which must leave the free
  count alone. And two crafted images: a file whose run points past the end
  of the volume, which a write must not reach through, and a `$Bitmap` longer
  than the volume, whose out-of-bounds read a guard page catches. That case
  runs last, because a fault ends the run.
* **`logfile_unit`** (243 checks) — the replay engine against synthetic logs.
* **`logfile_images`** — `ntfslog` over every fixture.
* **`enable_module_script`** — the enable script's two silent-failure modes.

### The fixture suite

Seven images from `mkntfs` covering 4 KiB / 512 B / 64 KiB clusters, long and
Unicode names, attribute lists, and compressed plus sparse files. For each:
read it and compare against `ntfsls`/`ntfscat`; verify a manifest of every file
with its size, times and hash; run a write cycle; then check the result with
`ntfsfix -n` and with ntfsprogs-plus's `fsck.ntfs`, and confirm the volume is
byte-identical to the pristine image once the written files are removed.

`NTFS_REQUIRE_FSCK=1` turns a missing structural checker into a failure instead
of a skip. CI sets it.

## 2. What is deliberately not covered

Being explicit about this is the point of the document.

* ~~**`chkdsk` has never seen anything we wrote.**~~ **Closed 2026-09-14.**
  `chkdsk /f` on Windows 10 (19045) found no problems on a volume this driver
  wrote 626 files to, and all 626 verified byte-for-byte afterwards. Its own
  counts corroborate what we wrote: +2 reparse records for our two symlinks,
  +618 index entries, +16 data files for the alternate data streams. Procedure
  and scripts: `tools/phase2-gate/`.

  What that does **not** cover, and is worth stating: one Windows build, one
  disk, one pass. Compressed writes were not exercised, because this driver
  cannot create a compressed file (`ntfs_create` never sets the flag), so only
  a volume Windows compressed would test that path end to end.
* **Journal replay has only ever run against synthetic logs.** The v2.0 record
  layout is inferred from a single source (`docs/LOGFILE.md` §5). Replay is
  therefore off unless a user asks for it per volume, at their own risk.
* **Compressed *writes*.** `core/ntfs/compress.c` is 1,550 lines and no test has
  ever written to a compressed file; the fixture is only read. The largest piece
  of implemented-but-unverified code in the project.
* **Sparse files, `fallocate`, hard links, symlinks, timestamps, permissions.**
  All implemented, none exercised by the write suite.
* **Three recorded decisions that nothing enforces** (`docs/PORTING.md` §6):
  Windows-illegal filenames must be rejected, names are case-insensitive and
  case-preserving, and xattrs map to alternate data streams.
* **Large directories**, so the index B-tree split and node-removal paths are
  untested. That is where a deletion slowdown was first (wrongly) suspected.
* **The FSKit request path** is only reachable from `mount-test.sh`, which needs
  an enabled module and a real mount, so CI cannot run it. What it does cover,
  as of 2026-09-14: an 8 MiB write and read back compared by sha256; 64
  scattered 4 KiB writes, which is where the 16 KiB UBC page turns every
  sub-page write into a read-modify-write; xattrs through the kernel including
  the assertion that no `._` file appears; a 300-entry directory listed through
  the kernel's readdir without duplicates; and **`ntfsck` on the image
  afterwards**, which until then nothing did -- the test verified that reads
  came back correct and stopped there, and a read-back cannot see a corrupt
  index, a wrong link count or a leaked cluster.
* **Everything in the app that touches the system**: IOKit and Disk Arbitration
  enumeration, `SMAppService`, the uninstaller, the packager, and the SwiftUI
  views. `fskit/NTFSTests` covers the logic that could be separated from them.
* **x86_64.** arm64 only, so nothing has ever been compiled for a second
  architecture.
* **Specific gaps the new suites named**, each because the code is unreachable
  from the public ABI or the input cannot be built here:
  hole punching (`FALLOC_FL_PUNCH_HOLE` is never passed by `ntfs_fallocate`);
  compressed and sparse together (the port reports `sparse=false` for compressed
  inodes); compression at cluster sizes other than 4 KiB (the driver refuses it
  above that, and no fixture exists below); a real EFS file (forged from
  attribute flags, with no `$EFS` stream);
  hard links into a different directory; Windows-authored
  `IO_REPARSE_TAG_SYMLINK` reparse points, which is the branch that does string
  surgery and so the likelier of the two to be wrong;
  concurrent anything -- link/unlink, readdir against a splitting index, two
  case variants racing -- because the ABI promises thread safety and a racing
  test without a scheduling hook passes by luck;
  torn writes and write reordering (the crash device is whole-write granular, so
  it models "everything after write N is lost" and not a half-written sector,
  which is what the update-sequence array exists for);
  LCNs above 2^32, which turn out to be unreachable rather than untested because
  NTFS caps a volume at 2^32 clusters, and above 2^31, which needs more than
  half of a 16 TiB volume filled.

## 3. How to write a test here

**Verify it fails.** Reintroduce the bug it is meant to catch, watch the test
fail, then restore. Every case in `core/tests/test_mount.c`,
`platform/tests/test_bdev.c: test_bd_mapping_contract` and
`fskit/NTFSTests/NoteExpiryTests.swift` was checked this way, and the commit
messages record the result. A regression test nobody has seen fail is a guess.

**Say which bug.** Each test carries a comment naming the defect it locks down
and, where useful, the commit that fixed it. That is what makes it safe for
someone to change the code later: they can tell what the test is protecting.

**Prefer a differential.** Two implementations of one decision are a bug
waiting to happen -- the mount path and `core/logfile` disagreed about whether a
journal was clean for a day. `test_mount.c` now asserts they agree, and
`probe_light_matches` does the same for the two probe paths. If you add a second
implementation of anything, add the comparison with it.

**Making something testable is allowed; changing behaviour is not.** The note
expiry rule was extracted into a pure function so it could be tested. The
extraction moved the comment with it and changed nothing else.

## 4. Traps that have cost real time

* **A test that does not run is not coverage.** `logfile_unit` hung inside
  ASan's own initialiser on macOS 26 -- before `main()`, so its 243 checks never
  executed -- and nothing noticed for a day because that suite lived in its own
  CMake project and `ctest --test-dir build` never touched it. The suite is in
  the main build now, and the sanitizers are split (below).
* **ASan does not work on macOS 26 / Apple silicon.** It deadlocks in
  `__asan::AsanInitInternal()` before `main()`, producing no output and never
  exiting. Confirmed on every test in this project, not just one. That matters
  because ASan is what originally found the work-queue use-after-free and the
  lookup-versus-invalidate race. It is `-DNTFS_ASAN=ON`, off by default, and
  worth retrying after an Xcode update. For one known out-of-bounds access, a
  guard page will do instead: `test_bitmap_longer_than_volume` copies the array
  to the end of a page, makes the next page inaccessible, and turns the read
  into a fault.
  **UBSan works and is ON by default**, with `-fno-sanitize-recover=undefined`
  so undefined behaviour fails the test rather than printing a warning nobody
  reads. This port takes a lot of unaligned loads out of packed on-disk
  structures, which is exactly what UBSan catches; all 8 targets pass clean.
* **Never compare performance across sessions.** A "12% regression" chased for
  an hour was an artefact of a baseline measured hours earlier with different
  disks attached. Build both versions and measure them back to back.
* **`F_NOCACHE` does not reach through an FSKit volume**, so each mount yields
  exactly one valid cold read per file. Re-reading reports 7-14 GB/s.
* **`io window` log lines aggregate over time**, so one covering a mixed phase
  describes whichever phase moved the most bytes. Isolate the phase before
  reading a cause out of them; a wrong diagnosis was published from one.
* **`platform_bdev` is timing-sensitive.** It has `usleep`-based waits and a
  lost-wake-up check with a 100 ms fallback, so it can fail under heavy load.
  Observed once at load average 64 on an 8-core machine, passing immediately
  afterwards three times in a row. If it fails, check the load before believing
  it; on a shared CI runner this is the test most likely to flake.
* **Device numbers are recycled.** `disk6` was three different physical disks in
  one session. Check `diskutil list` before trusting a path.
* **A hand-built restart page must satisfy `check_ra()`**, which requires
  `seq_number_bits == 67 - bit_length(file_size)` exactly and otherwise rejects
  the page as CORRUPT with no hint why. `test_mount.c` has a working builder.

## 5. Things that look like coverage and are not

* **`ntfsck -n` does not catch everything.** Measured, not assumed: it reports a
  dangling index entry and a `$Bitmap` that calls allocated clusters free, both
  exit 4. It does **not** catch a zeroed MFT record with no index entry pointing
  at it, or an `$EA_INFORMATION` that disagrees with its `$EA` (finding 27).
  Keep the in-test assertions as well as the structural check.
* **A cached read proves nothing.** Several bugs this week passed a read-back and
  failed only after a remount: a truncated symlink target served from
  `ni->target`, and a compressed extension served from the page cache. Every
  assertion about what reached the disk must remount first.
* **A fresh mount is a special case for the allocator.** Its data zone position
  starts on a cluster in use (the root folder's security descriptor, on mkntfs
  images), and a cluster in use ends a contiguous run cleanly. The first
  version of `test_fallocate_hole_zeroes` passed against the defect it was
  written for: the rest of the request came from a second call, which reports
  its clusters as new. It fails as it should once a write earlier in the same
  mount has moved the zone position onto free space.
* **A volume that an error took read-only comes back dirty.** Since finding 33,
  its unmount writes nothing but `VOLUME_IS_DIRTY`. A test that injects an
  error and then remounts finds what reached the disk before the unmount, not
  what was in memory, and that includes what the failing operation wrote after
  its error. A read-write remount is refused (`-EROFS`) unless the test mounts
  with `NTFS_MOUNT_RDONLY_FALLBACK`.
* **The logfile fixture has a real `$INDEX_ALLOCATION`** (record 31, cluster 44,
  left zeroed). That is what lets `test_first_write_into_fresh_cluster_is_protected`
  exist: it is the state finding 19 was about, a cluster allocated but never
  written. Two traps when using it: the open-attribute table's entry 0 is `$MFT`'s
  own `$DATA`, so a record aimed at `target_attr = 0x18` writes *inside the MFT*
  (entry 1 is at `RT_HEADER_SIZE + OA1_SIZE`); and an all-zero payload makes an
  update-sequence test vacuous, because the sector tails then trivially match a
  zero USN. Both cost an afternoon.
* **`ntfsck -n` cannot tell whose bytes a file holds.** Two cases were clean:
  a file reading back a deleted file's data around a write into a hole
  (finding 29), and a compressed block written over the clusters of the file
  next to it (finding 30). A stale-data test must also show that the clusters
  it lands on held something, because a fill that never reached them passes
  against the bug. `test_hole_write_zeroes` finds each write on the image and
  checks what its cluster held before. `test_fragmented_rewrite` finds the
  rewritten block on the image and requires it in two runs at least, because a
  free count cannot rule out 16 free clusters in a row. A write that fails
  leaves nothing of its own on the image to find. `test_failed_hole_write`
  requires a cluster that held `D` before to hold zeroes after, which only the
  failed write's edge zeroing does. `test_write_fail_into_hole` checks the
  cluster its refused data was aimed at.
* **`ntfsck -n` does not check a compressed size against the runs.** A failed
  mapping-pairs update left `random-1m.bin` on the disk with a compressed size
  64 KiB short of its runs, and `ntfsck -n` was clean (U22). Compare the
  compressed size before and after a remount; `ntfs_getattr()` reports it in
  `alloc_size` for a compressed file.
* **A sandboxed extension reads the app-group container, not `~/Library/Preferences`.**
  `defaults write group.ch.techtag.ntfs …` from a shell lands in
  `~/Library/Preferences/group.ch.techtag.ntfs.plist`; the extension reads
  `~/Library/Group Containers/group.ch.techtag.ntfs/Library/Preferences/…`.
  Probing the Settings switches that way made three of them look like mocks.
  Second time this trap has cost an hour (the first was the replay request).
  Write to the full group-container path, or flip the switch in the real app.
* **The kernel sends `flags = 0` on every create, and 0 is not a request.**
  `applyCreateAttributes` mapped it through the BSD-flags converter, which
  clears `FILE_ATTR_HIDDEN` when `UF_HIDDEN` is absent -- so `hide_dot_files`
  set the bit and the next call cleared it, leaving `$SI` plain and `$FN`
  hidden. The two-copies-disagree shape again. When a create-time attribute
  carries a default value, ask whether the caller meant it before applying it.
* **When a symptom will not reproduce locally, read the damaged bytes.** Three
  round trips were spent attributing broken symlinks to two different things
  in our own code -- each fit the sequence, neither reproduced through the raw
  API or FSKit, on 4 KiB or 512-byte clusters. Reading the damaged index block
  off the stick took one look: the bytes were a form nothing in this driver
  writes, and the only other writer was `chkdsk`. A sequence that fits a theory
  is not evidence for it; bytes are.
* **`FSShortName: ntfs` is unavailable, and it costs the Disk Utility label.**
  Measured 2026-09-23, because the original choice (`ttntfs`) was recorded only
  as a commit subject -- "Apple's ntfs.fs owns NTFS" -- with no evidence.
  Disk Arbitration's VolumeKind is the module's `FSShortName`, and `diskutil`
  resolves that name against the filesystem bundles in
  `/System/Library/Filesystems`. `ttntfs` matches nothing, so it falls back to
  guessing from the partition type: **"MS-DOS (FAT)"** in the CLI, **Unknown**
  in Disk Utility.app, for every Microsoft Basic Data volume we mount. Apple's
  driver reports `ntfs`, which resolves to "Windows NT File System (NTFS)".
  Note `f_fstypename` is `ntfs` for both -- the kernel name is not what
  `diskutil` reads.

  Setting `FSShortName: ntfs` fixes the label and breaks mounting. The module
  registers, our side mounts successfully (`core: ntfs: mounted read-write`),
  and then `fskitd` refuses: `ReallyMountVolume: returning Error
  Domain=NSPOSIXErrorDomain Code=61` (ECONNREFUSED), preceded by `Could not get
  file provider connection`, followed by an immediate unload. So the name is
  genuinely taken. The wrong label is the price of a working mount.

  **Reverting the plist is not enough to recover.** `fskitd` keeps per-volume
  state that survives a replug, killing the extension, killing the fskit agent
  and killing Apple's UserFS daemons. Only `sudo pkill -x fskitd` (or a reboot)
  clears it, after which the volume mounts normally. A volume whose partition
  type only we claim -- GPT Windows Recovery -- is unaffected throughout, which
  is the quickest way to tell this state apart from a real driver fault.

  **Run this class of experiment on a test image or the stick, never on a live
  disk.** It was run on a 995 GB working volume because the stick was not
  attached, and cost an hour of recovery.
* **Verify what is on the disk before it leaves the desk.** A stick handed to
  Windows to test the new native symlinks turned out to carry the old WSL tags:
  `fskitd` had kept a stale extension instance resident across the reinstall and
  it served the mount. The tags could have been read back from `$MFT` through
  the mount in ten seconds, no root needed. Instead the mistake surfaced from
  `fsutil` on the other machine, costing a full round trip. Kill the extension
  process after installing (`pkill -f Extensions/NTFSExtension`), and check the
  bytes locally first.
* **An all-`0xff` device is not necessarily a dead one.** A test stick yanked
  mid-write came back enumerating with no partition table and reading `0xff` at
  every offset across all 1.7 GB. It looked unambiguously erased. Windows then
  read every file from it normally, and it returned to macOS intact: the
  controller had failed to initialise and was reporting erased flash, and a power
  cycle fixed it. Power-cycle a suspect device before concluding anything, and
  never let an all-`0xff` read justify writing to a disk.
* **Read the tool's stage name before believing its counter.** `chkdsk`'s
  "N reparse records processed" is printed by a stage called *"Reparse point and
  Object ID verification"* and counts both. A climbing count on a volume with
  zero reparse points cost an investigation; it was Windows assigning object IDs.
* **Structural correctness is not correct data, and this one nearly shipped.**
  A journal-replay fix passed every check available: the pending directory
  appeared, the MFT records matched Windows on sequence, link count, flags and
  size, and `ntfsck` called the volume clean. It was still writing the *next log
  record's header* into a user's file. The only thing that caught it was reading
  the file's contents and diffing them against the same file on Windows. When
  replaying or repairing, compare payloads, not just metadata.
* **A structural check is not a byte comparison.** A one-cluster-short length in
  the compressed writer left `ntfsck` perfectly happy and the data unreadable.
  Both are needed.
* **Deleting from either end of a directory misses the interesting path.** It
  rarely removes a key that lives in an internal B-tree node, so the shuffled
  and middle-third cases are the ones that catch node removal.

## 6. What CI runs

`.github/workflows/ci.yml` calls `tools/ci.sh` on `macos-26` and caches the two
NTFS toolchains. It cannot run `mount-test.sh` (needs an enabled module and a
real mount) or `package.sh` (needs the Developer ID certificate), so a release
still needs a person.

It runs on every push to `main` via `.github/workflows/ci.yml` on GitHub's
`macos-26` runner, and has been green since 2026-09-19. Two things that runner
taught us, both fixed at the source: Homebrew installs GNU libtool as
`glibtoolize`, so `ensure_autotools` must accept that name; and the runner's
Xcode may be newer than the developer's -- its 26.6 SDK added
`FSVolume.MountOptions`, which shadowed our own type inside a subclass of
`FSVolume`, so ours is `NTFSMountOptions`. `tools/install-hooks.sh` still
installs a pre-push hook running `tools/ci.sh --quick`, which catches most
things before the runner does.
