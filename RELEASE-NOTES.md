# TT NTFS Native — release notes

## Build of 2026-09-17, second (commit 0023620)

### Symlinks now work on Windows

Every symlink this driver created was invisible in Explorer and unopenable from
the Windows command line -- `type` reported "The file cannot be accessed by the
system". The data behind the link was fine; the link itself was unusable.

Two causes, both fixed. The link was marked as a hidden system file, so `dir`
and Explorer skipped it. And it carried the WSL reparse tag, which native Windows
refuses to follow; Linux's ntfs3 driver cannot read it either. That tag was
inherited from the Linux source this driver is ported from, and the justification
in this project's own notes for keeping it -- "it is what Linux writes" -- turned
out on inspection to be false.

Symlinks are now written with the native Windows tag whenever the target can be
expressed in it, which is every ordinary link a copied tree contains. Verified
on Windows 10: `dir` lists them as `<SYMLINK>`, `type` follows them, and
`chkdsk /f` finds no problems. A target Windows cannot name -- one containing
`: * ? " < > |` or a literal backslash -- keeps the WSL tag so it survives
unchanged, which is the same per-link rule Microsoft's own WSL applies. A new
`wsl_symlinks` mount option forces the old behaviour for every link.

Also fixed on the way: a native symlink written by Windows or Linux reported a
size of 0 bytes after a remount, because only WSL-tagged links were decoded at
load. Both kinds now stat correctly.

One thing to know: a symlink to a *directory* is written as a file-type link,
which Windows lists as `<SYMLINK>` rather than `<SYMLINKD>`. Following it works;
`cd` through it from cmd may not. Recorded as a follow-up.

---

## Build of 2026-09-17, first (commit fb7c1bb)

### Journal replay is now verified against Windows, not inferred

Four scenarios were captured from a real Windows 10 machine -- each one a genuine
interrupted write, with the disk pulled mid-operation -- then replayed both by
this driver and by Windows, and the results compared byte for byte.

| what was interrupted | journal records | result |
|---|---|---|
| creating a file and a directory | 5 | matches Windows |
| 25 deletes and 25 creates in one directory | 448 | matches Windows |
| appending 300 MB to a file | 1113 | matches Windows |
| `chkdsk` on a volume **only this driver** recovered | -- | **no problems found** |

That last row is the one that matters most: Windows never saw the journal, and
still found nothing to correct in a directory index this driver had rebuilt from
50 pending operations.

Two real bugs were found and fixed along the way, both of which would have
mattered on ordinary crashes: an index block written into a freshly allocated
cluster came out without its integrity data, which made every later read of it
fail; and a replay payload could be truncated.

### "Close Journal" when nothing needs replaying

Fast Startup is on by default in Windows, and a normal shutdown with a disk
attached does **not** close that disk's journal. The volume then looks dirty
while being completely intact -- nothing to replay, no unfinished writes.

Until now the app offered "Replay Journal" and warned about possible file system
damage. For this case every word of that was wrong. It now reads **Close
Journal**, explains that Windows simply left the journal open, and says plainly
that it changes two pages of bookkeeping and touches none of your files. The
original warning is kept, with a count, for journals that really do carry
unfinished work.

---

## Build of 2026-09-15, third (commit c8f9e94)

### A volume whose journal has no restart page is now usable

Some volumes carry a `$LogFile` whose two restart pages have been erased, with
record pages still behind them. We called that corrupt and mounted the volume
read-only, permanently, with no way forward except Windows.

**Windows does not agree, and it is right.** Given such a volume -- a Windows
Recovery partition in this state since 2022 -- Windows wrote two fresh restart
pages, left every record page alone, and mounted it read-write. It did not erase
the journal and did not repair anything.

This build does the same: **two writes, and the volume mounts**. The restart
pages we produce match Windows' own in version, state and geometry.

Worth stating what this is not. It does not repair a volume. The recovery
partition had a real structural inconsistency, and Windows did not fix that
either -- `chkdsk` is still the tool for that, and a journal cannot substitute.
What changed is only that a missing journal header is no longer mistaken for a
damaged volume.

A journal that genuinely says "replay me" is still refused read-write, as before.

---

## Build of 2026-09-15, second (commit 60710f0)

One change over the earlier build today, and it is worth having.

### Mounting a disk read-write no longer rewrites its whole journal

Every read-write mount of a volume Windows has used rewrote the entire
`$LogFile`. Measured at the device on a real Windows volume: **1,420 writes,
5.5 MiB**, for a mount that changed nothing else. Your 500 GB disk carries a
64 MiB journal, so the same mount was about **16,384 writes**.

Worse than the cost was the order: the erase destroyed the two restart pages
*first* and then spent thousands of writes on the rest, so a crash anywhere in
that window left a journal full of recoverable work with nothing left to reach
it by. That is exactly what this driver did to a disk on 2026-09-13.

It now writes **two restart pages, last** -- the same thing Windows leaves on a
clean dismount, and what the Linux `ntfs3` driver and `ntfsrecover` both write
after replaying. The full erase remains as a fallback for a journal too damaged
to rewrite.

**Measured: 1,420 writes -> 2. Verified across two Windows round trips:**
`chkdsk /f` reported no problems both times, files opened normally on Windows,
and payloads hashed identically on return. Windows rewrote the two restart pages
itself on mount, changing 38 of 8,192 bytes of bookkeeping and keeping our
format rather than correcting it.

A journal that is genuinely corrupt is still refused a read-write mount before
any of this runs, so that case is untouched -- and no longer erased, which is
safer than before.

---

## Build of 2026-09-15, first (commit a47cd6e)

53 commits since the previous build. Read the first two sections before
installing over an older copy; the rest is detail.

### Read this first

**Two defects in the previous build could destroy data.** Both are fixed here.

- **Any volume whose logical sector size is not 512 bytes was destroyed by the
  first metadata write.** One `mkdir` was enough: the first six MFT records came
  back freshly formatted and the volume would not mount again. This affects true
  4Kn disks. Most external SSDs are 512e, which NTFS records as 512 and which was
  never at risk.
- **Appending to a compressed file silently did not work.** Three separate bugs,
  all of them present in the Linux source this driver is ported from. Each
  reported success to the caller and each left the volume passing a structural
  check, so nothing looked wrong.

If you wrote to a 4Kn disk or appended to a compressed file with an earlier
build, check those volumes with `chkdsk /f` on Windows.

### Journal replay now works, and has been checked against Windows

The Replay Journal button **had never done anything, on any volume.** Disk
Arbitration loads the extension twice for one mount -- a probe with a read-only
handle, then the instance that actually serves the volume -- and the probe was
consuming the request, then refusing the work it had just claimed because the
device was read-only. Silently, because the refusal was logged by an instance
about to be torn down.

With that fixed, replay was run against a real Windows 10 dirty journal
(version 2.0, the format modern Windows writes) and **reproduced Windows' own
result**: the same pending directory and file recovered, file contents identical
byte for byte, and the MFT record differing from Windows' in exactly the two
update-sequence fixup slots, which differ on every write by definition. `ntfsck`
calls the result clean.

That is one scenario out of five in the test plan, so replay remains **opt-in per
volume** and is not offered automatically. It is no longer unverified, but it is
not yet proven across the range of operations a real crash produces.

### Windows has now checked our writes

`chkdsk /f` on Windows 10 reported no problems on a volume this driver had
written 626 files and 608 MiB to -- covering the resident/non-resident boundary,
fragmented 200 MiB files, a 500-entry directory index, 255-character and CJK and
NFC-versus-NFD names, alternate data streams, hard links, symlinks, and a
delete/rename churn -- and all 626 files then verified byte for byte on the way
back. Both halves matter: `chkdsk` repairs what it finds, so a clean verdict
alone would not have been enough.

Its own counts corroborate the write: +2 reparse records for our two symlinks,
+618 index entries, +16 data files for the alternate streams.

This closes phase 2 of the port. One Windows build, one disk, one pass -- it is
evidence, not a warranty.

### Fixed

- 4Kn and other non-512-sector volumes are writable (two unit errors in the
  vendored Linux source: bio sector numbers divided by the filesystem block size
  instead of 512, and the MFT mirror dropping the page index)
- three compressed-write data-loss bugs, all upstream's: a write into the
  allocation rounding was lost, `initialized_size` was never advanced, and
  zeroing a compression block that held data left the data
- journal replay reaching the instance that can perform it
- a replay payload that points past its own record is zero-filled rather than
  read from the neighbouring log record
- `fsync` no longer returns success when the device flush failed
- unmount no longer fails on USB devices, and a failed flush no longer claims
  the volume may be inconsistent
- an I/O error at mount is reported as an I/O error, not as "not an NTFS volume"
- a volume that drops to read-only after errors now says why, and tells you to
  copy your files off
- pre-1970 timestamps no longer cross the API denormalised
- the hard-link cap is enforced at 1024, matching what Windows itself allows
- writes to a symlink are refused rather than writing a stream nothing reads
- an unaligned load in the compressor
- the app's status file could not be read after an upgrade, so no volume showed
  any status until every one was remounted
- an eject-failure message could outlive its disk and reattach to the next one

### Performance

Streaming writes are ahead of Apple's own exFAT module (1.15x); overwrite and
append are level; every metadata operation is within the "2x of exFAT" bar.
Deleting from a large directory is 3.5x faster than the previous build. Reads on
a USB SSD are within about 11% of Apple's built-in driver, which is close to the
enclosure's ceiling.

Small random writes remain slower than exFAT. The cause is measured rather than
guessed: macOS works in 16 KiB pages, so a 4 KiB write becomes a 16 KiB device
write. Apple's own module pays the same cost.

### Known limitations

- **`fsync` cannot promise durability.** FSKit exposes no device-cache barrier
  of any kind, so a successful `fsync` does not guarantee the data has left the
  disk's own write cache. Ordering is preserved and writes are direct. This is a
  platform limitation, documented rather than hidden behind a success return.
- **Journal replay is verified against one scenario**, and the v2.0 record format
  it acts on is reverse-engineered -- Microsoft has never documented it. Replay
  stays opt-in.
- **Compressed files cannot be created**, only written to if Windows made them.
  That path is also the one part of the write path Windows has not checked.
- **arm64 only.** No Intel build.
- No format or repair tools in the app yet.

### Under the hood

`ctest` went from 4 targets to 15, roughly 2,700 checks, written mostly against
code that had already shipped without tests. They found 14 defects, all fixed.
Everything runs twice, with and without UndefinedBehaviorSanitizer, plus 265
fixture checks against ntfsprogs as an independent oracle with a real structural
`fsck` on every write, and 51 unit tests for the app.

`LICENSE` is present for the first time. The driver is GPL-2.0, inherited from
the Linux source it derives from rather than chosen.
