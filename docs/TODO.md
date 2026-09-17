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
- [x] B1 attempted twice 2026-09-17 and not pursued further. Hibernation leaves the
      log open like Fast Startup, with one transaction to roll back. Both attempts
      failed to strand a write: Windows seems to finish a pending copy before
      hibernating. Expected finding is already known from ntfsrecover's refusal.
- [x] Done 2026-09-17 after four attempts: 26,543 records, 6,634 UpdateMappingPairs,
      8 clusters per dirty page. Our replay matches Windows -- D.bin identical, its
      MFT record differing in 5 per-write bytes. The lcns_to_follow > 1 path works.
- [x] Answered by B2 and E1: a *safely removed* stick comes back v1.1 CLEAN (E1),
      and an open v2.0 log is what a shutdown that does not dismount leaves (B2).
      The removal policy was never the cause. Ordinary users DO hit the read-only
      path, via Fast Startup, which is why the Close Journal wording exists.
- [x] Confirmed 2026-09-17: Windows opens what this driver wrote, hard links report
      all four paths, and our symlinks are structurally valid. But Windows cannot
      follow them (WSL tag) and plain `dir` hides them (we set FILE_ATTR_SYSTEM).
      Recorded as finding 20.
- [x] FILE_ATTR_SYSTEM dropped for symlinks (50a558c). Confirmed on Windows: native
      links now appear in a plain `dir`.
- [x] Symlink tag: done and verified on Windows 2026-09-17. Native tag by default,
      per link, WSL fallback for targets Windows cannot name; wsl_symlinks mount
      flag forces the old behaviour. Windows follows them, chkdsk is clean, and the
      EA coexistence turned out not to matter. Finding 20.
- [x] Fixed 2026-09-17 (finding 22): a symlink to an existing directory is written
      as a directory record. Windows shows <SYMLINKD> and cd works.
- [ ] Our driver cannot read ntfs-3g's default Interix symlinks (IntxLNK $DATA files).
      Stock ntfs-3g is the most deployed third-party NTFS driver; its symlinks show
      to us as 1-cluster system files with binary contents.
- [x] Fixed 2026-09-17 (finding 21): Windows directory symlinks and junctions read
      as links and resolve, deleting one leaves the target alone.
- [x] Confirmed 2026-09-18: links from e045c2e survive chkdsk + Mac write + Windows.
      The real cause was chkdsk rewriting the index entry of any file carrying both
      $EA and a reparse point (finding 23). Symlinks no longer carry $EA.
- [ ] Migration: symlinks from builds before e045c2e still carry $EA and chkdsk will
      still rewrite their index entries. A pass that strips $EA from existing links
      would repair them. Low priority -- they still work on the Mac.
- [x] Xattrs confirmed 2026-09-17: `dir /r` shows them as ordinary alternate data
      streams on Windows. Not separately checked: the 8 KB `user.big` xattr in
      F-xattr, which exceeds the inline limit and takes a different storage path.
- [ ] Compressed round trip: Windows makes a compressed folder, we write into it,
      chkdsk. Closes phase 2's one hole; we cannot create compressed files.

## Blocked on a decision
- [ ] Git remote. CI exists (tools/ci.sh, 6 stages) and runs only when typed.
- [ ] Author email on 29 commits. Safest now, no remote. Needs a fresh backup branch.

## Unblocked, not started
- [x] Done 2026-09-17: the logfile fixture now carries a real $INDEX_ALLOCATION,
      and both gaps have load-bearing unit tests --
      test_first_write_into_fresh_cluster_is_protected (finding 19) and
      test_rw_mount_retires_the_journal_in_two_writes (327 writes vs 2).
- [ ] Port ntfsprogs-plus's v2.0 acceptance into core/ntfs/logfile.c so the log
      stops saying "LogFile version 2.0 is not supported" on every mount.

- [ ] x86_64 build. arm64 only; nothing has ever compiled for a second arch.
- [ ] mkfs/fsck in the app, Homebrew cask.
- [x] Rebuild the DMG. Done 2026-09-15 from a47cd6e: notarized, stapled,
      `source=Notarized Developer ID`, installed from the DMG itself and verified
      serving a real volume. Release notes in `RELEASE-NOTES.md`.
