# logfile stream — status

Owner: logfile agent. Paths: `core/logfile/`, `docs/LOGFILE.md`.

## Done
- Module builds with `-Wall -Wextra`; `cmake -S core/logfile -B build-logfile &&
  cmake --build build-logfile && ctest --test-dir build-logfile` builds `libntfslogfile.a`,
  `ntfslog`, `test_logfile` (ASan+UBSan) and runs two tests: `logfile_unit` (257 checks) and
  `logfile_images` (analyzer must say clean on every `tools/images/*.img`; skipped if absent).
- Parser: restart pages (v1.0, 1.1, 2.0), restart area, client record, record pages, LFS record
  headers, NTFS client records, checkpoint (NTFS_RESTART v0/v1) and the three restart tables.
- Tail scan around fslog.c `last_log_lsn()` semantics (v1 tail copies at pages 2/3 with the
  file offset in the lsn field; v2 copies at 0x02../0x12.. with `file_off` at 0x3c), torn
  transfer check (H4).
- Replay: analysis / redo / undo over an in-memory overlay, dry-run plan, flush in order,
  `$MFTMirr` kept in sync, `mark_clean` writes v1.1 closed+clean restart pages (what Windows
  writes at clean dismount, and what fslog.c writes).
- Coherence rules: H1 MFT ops must target `$MFT:$DATA` and every logged page LCN must equal
  what record 0's run list (through the overlay) gives for that vcn; H2 lsn check on
  InitializeFileRecordSegment; H3 no cluster writes inside `$MFT`; H4 torn-transfer check;
  MFT op outside `$MFT`'s run list is refused (never skipped).
- Fixed this session: (1) H1 compared against the dirty-page override seeded from the very
  LCN under test (tautology) — now `$MFT:$DATA` never takes dirty-page overrides and the
  comparison is against the mapping pairs; (2) skipped records were counted as redone/undone;
  (3) the "unreadable lcn" test corrupted the MFT run list instead of the logged LCN — the
  logged LCN legitimately wins for non-MFT attributes (dirty page table semantics, fslog.c
  parity), test rewritten and a positive stale-run-list case added; (4) `ASAN_OPTIONS=
  detect_leaks=1` aborts on macOS arm64, removed.
- Synthetic v2.0 case (restart page 2.0, records from page 0x22, v2 tail copy, stray copy in
  the record area) in `test_v2_log`.
- `docs/LOGFILE.md`: format, algorithm, verified-vs-inferred, supported versions, capture list.

## In progress
- Nothing; waiting for Windows captures.

## Next
- Validate on captures from the Windows PC (see `docs/LOGFILE.md` §7): run `ntfslog` on each,
  compare our replay against the image Windows produced after its own replay, `chkdsk /f` on
  our result. Only then enable replay by default in the mount path.
- Wire `ntfs_logfile_*` into `core/vfs/` mount (integrator): open → is_clean → dry run →
  apply → mark_clean; on any refusal mount read-only with the message.

## Known problems / open questions
- No real dirty `$LogFile` (1.1 or 2.0) has been seen by this code. Everything about 2.0 and
  about the op payload layouts beyond fslog.c is inferred. See the verified/inferred table
  in `docs/LOGFILE.md` §5.
- `mark_clean` always writes version 1.1 pages (fslog.c does too). Believed to match the
  Windows 8+ downgrade-at-dismount behaviour; needs a capture round-trip to confirm chkdsk
  accepts it.
- v2.0 multi-page tail transfers are matched page by page through `file_off`, not
  reassembled as one transfer as fslog.c does.
