# logfile stream — status

Owner: logfile agent. Paths: `core/logfile/`, `docs/LOGFILE.md`.

## Done
- All module sources compile with `-Wall -Wextra` (`logfile_open.c`, `logfile_pages.c`,
  `logfile_records.c`, `logfile_tables.c`, `logfile_replay.c`); the truncated
  `logfile_replay.c` from the previous session is repaired.
- Replay engine completed: analysis / redo / undo passes over an in-memory overlay,
  dry-run plan, coherence heuristics (H1 MFT ops must target `$MFT:$DATA` with a matching
  logged LCN, H2 lsn check on InitializeFileRecordSegment, H3 no cluster writes inside
  `$MFT`, H4 tail-scan torn-transfer check), open-attribute invalidation after MFT edits,
  INDX re-protection after UpdateNonresidentValue.
- Tail scan rewritten around fslog.c `last_log_lsn()` semantics (v1 tail copies keep the
  file offset in the lsn field; v2 copies use `file_off` at 0x3c).
- Public API additions (additive): `ntfs_logfile_load_checkpoint`, `ntfs_logfile_tables`,
  extra `ntfs_logfile_info` / `ntfs_log_record` fields.
- Written, not yet built: `cli/ntfslog.c` + `cli/ntfs_image.c`, `tests/test_logfile.c`,
  `CMakeLists.txt`.

## In progress
- Building and running the unit tests (ASan/UBSan), fixing what they find.

## Next
- `docs/LOGFILE.md` (format, algorithm, verified vs inferred, capture list).
- Run `ntfslog` on `tools/images/*.img` when the tools stream generates them.

## Known problems / open questions
- No real dirty `$LogFile` (1.1 or 2.0) has been seen by this code yet; everything about
  2.0 is inferred from fslog.c. Replay must be validated on captures from the Windows PC
  before it is enabled by default.
