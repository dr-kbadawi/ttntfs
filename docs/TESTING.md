# Testing

What is checked, what is not, and why. A filesystem driver that is wrong loses
data silently, so the question this document tries to answer is not "do the
tests pass" but "what would still get through".

Run everything that does not need hardware or a signing certificate:

```
tools/ci.sh              # all stages
tools/ci.sh --quick      # skip the fixture suite (the slow one)
tools/ci.sh --list       # what it will run
```

## 1. The suites

| suite | what it covers | run by |
|---|---|---|
| `ctest` (8 targets) | the platform layer, mount-time decisions, `$LogFile`, the enable script | `ctest --test-dir build` |
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
* **`mount`** — the mount-time decisions: the `$LogFile` clean rule, the
  read-only reason, a read-write mount with a non-empty journal, the fast probe
  against the full one, and the device-flush count.
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

* **`chkdsk` has never seen anything we wrote.** This is the phase 2 gate.
  ntfsprogs-plus is a good structural check and it runs on every commit, but
  Windows defines NTFS: agreement with a third-party checker is evidence, not
  proof. Needs a physical Windows PC; the procedure is `docs/LOGFILE.md` §7.
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
  CMake project and `ctest --test-dir build` never touched it. Sanitizers are
  now opt-in (`-DNTFS_LOGFILE_SANITIZE=ON`) and the suite is in the main build.
* **Never compare performance across sessions.** A "12% regression" chased for
  an hour was an artefact of a baseline measured hours earlier with different
  disks attached. Build both versions and measure them back to back.
* **`F_NOCACHE` does not reach through an FSKit volume**, so each mount yields
  exactly one valid cold read per file. Re-reading reports 7-14 GB/s.
* **`io window` log lines aggregate over time**, so one covering a mixed phase
  describes whichever phase moved the most bytes. Isolate the phase before
  reading a cause out of them; a wrong diagnosis was published from one.
* **Device numbers are recycled.** `disk6` was three different physical disks in
  one session. Check `diskutil list` before trusting a path.
* **A hand-built restart page must satisfy `check_ra()`**, which requires
  `seq_number_bits == 67 - bit_length(file_size)` exactly and otherwise rejects
  the page as CORRUPT with no hint why. `test_mount.c` has a working builder.

## 5. What CI runs

`.github/workflows/ci.yml` calls `tools/ci.sh` on `macos-26` and caches the two
NTFS toolchains. It cannot run `mount-test.sh` (needs an enabled module and a
real mount) or `package.sh` (needs the Developer ID certificate), so a release
still needs a person.

There is no git remote yet, so nothing runs automatically. Until there is one,
`tools/install-hooks.sh` installs a pre-push hook that runs `tools/ci.sh
--quick`.
