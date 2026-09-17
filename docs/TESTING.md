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
| `ctest` (15 targets, ~2,700 checks) | the platform layer, mount decisions, `$LogFile`, compressed and sparse writes, links and names, large directories, crash consistency, fault injection, geometry, the enable script | `ctest --test-dir build` |
| `tools/run-tests.sh` (265 checks) | read and write against 7 NTFS fixtures, compared with ntfsprogs as ground truth, then structurally checked | `tools/run-tests.sh all` |
| `NTFSTests` (51 tests) | the app's Swift logic: option parsing, status decoding, note expiry | `xcodebuild test -scheme NTFSTests` |
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
* **`mount`** (111) — the `$LogFile` clean rule, the read-only reason, a
  read-write mount with a non-empty journal, the fast probe against the full
  one, and the device-flush count.
* **`links`** (279) — hard links, symlinks through reparse points, the 100 ns
  timestamp conversion, and permissions from the mount masks.
* **`names`** (448) — the three name and xattr decisions in PORTING.md §6:
  Windows-illegal names refused, case-insensitive and case-preserving lookup
  through a real `$UpCase` table, and xattrs as alternate data streams.
* **`compress`** (327) — compressed writes, including appending across
  compression-block boundaries, and the sparse read, seek and `fallocate` paths.
* **`bigdir`** (480) — the index B-tree: growth to four levels, deletion in
  creation, reverse and shuffled order, and six collation shapes.
* **`crash`** (217) — the write ordering, asserted by mapping a sync's device
  writes back to `$MFT`/`$Bitmap`, then 55 interrupted operations checked with
  `ntfsck`.
* **`faults`** (148) — injected read, write and flush failures at fourteen
  points, plus a volume filled to `ENOSPC`.
* **`geometry`** (672) — 4Kn sectors, cluster sizes either side of the sector
  size, and a 16 TiB sparse volume. Runs a full read-write cycle per geometry
  and compares `$MFT` records 0-5 and `$MFTMirr` byte for byte afterwards; this
  is what found finding 15 and what pins it fixed.
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
* **Two journal-replay fixes have no unit test**, only verification against real
  Windows crash captures. Finding 19 (an index block written into a fresh
  cluster must still get its update sequence array) and the two-page journal
  retirement were both proved by replaying a real capture and diffing the result
  against what Windows produced from the same journal -- stronger evidence than
  a synthetic test, but it does not run in CI, and the captures are 3.2 GB of
  gitignored images.

  Writing the synthetic equivalent was attempted on 2026-09-17 and abandoned:
  `core/logfile/tests/test_logfile.c` builds its volume from a handful of
  hand-made MFT records, and a record carrying a real `$INDEX_ALLOCATION` whose
  runlist the replay engine will resolve turned out to need more fixture than the
  harness currently offers. Worth doing; not worth faking. Until then a
  regression in either would be caught only by re-running a capture by hand.
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
  worth retrying after an Xcode update.
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
  at it. Keep the in-test assertions as well as the structural check.
* **A cached read proves nothing.** Several bugs this week passed a read-back and
  failed only after a remount: a truncated symlink target served from
  `ni->target`, and a compressed extension served from the page cache. Every
  assertion about what reached the disk must remount first.
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

There is no git remote yet, so nothing runs automatically. Until there is one,
`tools/install-hooks.sh` installs a pre-push hook that runs `tools/ci.sh
--quick`.
