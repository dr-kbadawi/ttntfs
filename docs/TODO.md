# TT NTFS Native — open work, 2026-09-15

## Needs the Windows machine
- [x] Phase 4 scenarios A, C, D and E3 -- all reproduce Windows. E3 is the
      strongest: chkdsk found no problems on a volume only our replay recovered.
- [x] E1 done: a clean Windows safe-removal leaves v1.1 CLEAN, confirming the
      documented downgrade, and every structural field of its restart area matches
      what mark_clean writes. Only current_lsn differs, which is a position.
- [x] B2 (Fast Startup) done 2026-09-17, and it is the most user-facing result of
      the lot: a hybrid shutdown does NOT dismount a removable volume, so the log
      is left open and reads dirty with nothing at all to replay. The app now
      distinguishes that from real pending work (Close Journal vs Replay Journal).
- [~] B1 attempted 2026-09-17: hibernation leaves the log open like Fast Startup,
      with one transaction to roll back. But the 300 MB copy finished before the
      machine hibernated, so writes-in-flight were never stranded and the dangerous
      case is still unmeasured. Needs the hibernate to interrupt a copy rather than
      follow it -- bigger file, slower device, or hibernate first.
- [ ] Scenario D on a 512-byte-cluster stick, for multi-cluster records
      (lcns_to_follow > 1), a path none of the three captures exercised.
- [x] Answered by B2 and E1: a *safely removed* stick comes back v1.1 CLEAN (E1),
      and an open v2.0 log is what a shutdown that does not dismount leaves (B2).
      The removal policy was never the cause. Ordinary users DO hit the read-only
      path, via Fast Startup, which is why the Close Journal wording exists.
- [ ] Does Windows *open* what we wrote? (255-char/CJK names, our symlinks,
      xattrs as ADS, hard-link count.) chkdsk validated structure, never opened a file.
- [ ] Compressed round trip: Windows makes a compressed folder, we write into it,
      chkdsk. Closes phase 2's one hole; we cannot create compressed files.

## Blocked on a decision
- [ ] Git remote. CI exists (tools/ci.sh, 6 stages) and runs only when typed.
- [ ] Author email on 29 commits. Safest now, no remote. Needs a fresh backup branch.

## Unblocked, not started
- [ ] Give core/logfile/tests a volume fixture with a real $INDEX_ALLOCATION, so
      finding 19 and the two-page retirement can have unit tests instead of only
      capture evidence. Attempted 2026-09-17 and abandoned; see docs/TESTING.md.
- [ ] Port ntfsprogs-plus's v2.0 acceptance into core/ntfs/logfile.c so the log
      stops saying "LogFile version 2.0 is not supported" on every mount.
- [ ] Apple Feedback: System Settings toggle, 5 s mount gap, and possibly
      metadataFlush EIO on USB (test FSSupportsKernelOffloadedIO first).
- [ ] x86_64 build. arm64 only; nothing has ever compiled for a second arch.
- [ ] mkfs/fsck in the app, Homebrew cask.
- [x] Rebuild the DMG. Done 2026-09-15 from a47cd6e: notarized, stapled,
      `source=Notarized Developer ID`, installed from the DMG itself and verified
      serving a real volume. Release notes in `RELEASE-NOTES.md`.
