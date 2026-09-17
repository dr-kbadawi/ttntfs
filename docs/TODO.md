# TT NTFS Native — open work, 2026-09-15

## Needs the Windows machine
- [x] Phase 4 scenarios A, C, D and E3 -- all reproduce Windows. E3 is the
      strongest: chkdsk found no problems on a volume only our replay recovered.
- [ ] Phase 4 scenario B (hibernation / Fast Startup) and E1 (a real Windows-written
      v1.1 restart page to compare mark_clean against). B is the case ntfsrecover
      refuses by default as dangerous; refusing may be the correct answer.
- [ ] Scenario D on a 512-byte-cluster stick, for multi-cluster records
      (lcns_to_follow > 1), a path none of the three captures exercised.
- [ ] Quick-removal policy test: is a normally-removed Win10 stick clean?
      Two minutes. Decides whether ordinary users hit our read-only path.
- [ ] Does Windows *open* what we wrote? (255-char/CJK names, our symlinks,
      xattrs as ADS, hard-link count.) chkdsk validated structure, never opened a file.
- [ ] Compressed round trip: Windows makes a compressed folder, we write into it,
      chkdsk. Closes phase 2's one hole; we cannot create compressed files.

## Blocked on a decision
- [ ] Git remote. CI exists (tools/ci.sh, 6 stages) and runs only when typed.
- [ ] Author email on 29 commits. Safest now, no remote. Needs a fresh backup branch.

## Unblocked, not started
- [ ] Port ntfsprogs-plus's v2.0 acceptance into core/ntfs/logfile.c so the log
      stops saying "LogFile version 2.0 is not supported" on every mount.
- [ ] Apple Feedback: System Settings toggle, 5 s mount gap, and possibly
      metadataFlush EIO on USB (test FSSupportsKernelOffloadedIO first).
- [ ] x86_64 build. arm64 only; nothing has ever compiled for a second arch.
- [ ] mkfs/fsck in the app, Homebrew cask.
- [x] Rebuild the DMG. Done 2026-09-15 from a47cd6e: notarized, stapled,
      `source=Notarized Developer ID`, installed from the DMG itself and verified
      serving a real volume. Release notes in `RELEASE-NOTES.md`.
