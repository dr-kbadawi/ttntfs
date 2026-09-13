# `$LogFile` — format, replay algorithm, verification status

Module: `core/logfile/` (libc only). API: `core/logfile/ntfs_logfile.h`.
Analyzer: `ntfslog`. Tests: `core/logfile/tests/`. Status: `docs/progress/logfile.md`.

This document describes the on-disk format **as this module implements it** and
the replay algorithm **as coded**, marks every item as verified or inferred, and
ends with the capture list the integrator runs on the physical Windows PC. Until
those captures exist, replay is a dry-run-only facility: nothing in this module
has yet touched a `$LogFile` written by Windows.

Reference used as a map (never as truth): Paragon's `fs/ntfs3/fslog.c` (Linux
v7.1, GPL-2.0). Where this module deliberately differs from fslog.c it is said so.

## 1. Build, run, test

```
cmake -S core/logfile -B build-logfile && cmake --build build-logfile
ctest --test-dir build-logfile            # logfile_unit (ASan+UBSan), logfile_images
./build-logfile/ntfslog [-v] [-x] [-n MAX] [--no-plan] [--apply] [-c CLUSTER] [-r RECSIZE] <image|dump>
```

`ntfslog` accepts an NTFS **volume image** (partition, boot sector at offset 0) or a
**raw `$LogFile` dump** (first bytes are a restart page or all 0xff; geometry then
comes from `-c`/`-r`, default 4096/1024). Without `--apply` it never writes. With
`--apply` it applies the plan to the image and writes clean restart pages.

`logfile_images` runs `ntfslog` over `${NTFS_IMAGES}` (default
`tools/images`, override with `-DNTFS_IMAGES=<dir>`) and requires every image to
report `Clean for read-write mount: yes`. All mkntfs fixtures have an all-0xff
`$LogFile` (state EMPTY); the initialized cases live in `test_logfile`.

## 2. Format as implemented

All integers little-endian. Offsets in hex. Every structure below is declared in
`logfile_layout.h`.

### 2.1 Geometry and LSNs

* `$LogFile` is MFT record 2, unnamed `$DATA`, non-resident. Only the first 4 GiB
  are addressable (`orig_size = min(size, 4G)`); the usable size `l_size` is the
  restart area's `file_size` rounded down to a page.
* `file_data_bits = 64 - seq_number_bits`; the restart area's `seq_number_bits`
  must equal `67 - bits(file_size)` (fslog.c check, enforced).
* `lsn -> file offset`: `(lsn << seq_number_bits) >> (seq_number_bits - 3)` (an lsn
  is an 8-byte-granular offset plus a sequence number in the high bits).
* `offset -> lsn`: `(vbo >> 3) | (seq << file_data_bits)`.
* Page size: `log_page_size` must equal `system_page_size` (refused otherwise,
  `-ENOTSUP`); ≤ 64 KiB. Every page and MFT record uses 512-byte update sequence
  fixups regardless of the physical sector size.
* First record page: `4 * page_size` for v1.x, `0x22 * page_size` for v2.0.
  Pages before it: 0 and 1 are the two restart pages; 2 and 3 (v1) or 0x02–0x11
  and 0x12–0x21 (v2) are tail-copy pages.

### 2.2 Restart page (`RSTR`, or `CHKD` when written by chkdsk)

| off | field |
|---|---|
| 00 | magic `RSTR` / `CHKD` |
| 04/06 | usa offset / count (count must be `1 + page/512`; a `CHKD` page may have none) |
| 08 | `chkdsk_lsn` (must be 0 on `RSTR`) |
| 10 | `system_page_size` |
| 14 | `log_page_size` |
| 18 | `restart_area_offset` (8-aligned, after the usa) |
| 1a/1c | `minor_ver` / `major_ver` |

Selection between the two pages: both are parsed; the one with the higher
`current_lsn` (or `chkdsk_lsn`) wins; a `CHKD` page in slot 0 with an `RSTR` in
slot 1 means an interrupted chkdsk → state `CHKDSK`, replay refused.

### 2.3 Restart area (at `restart_area_offset`)

| off | field |
|---|---|
| 00 | `current_lsn` — last lsn the LFS flushed the area for |
| 08 | `log_clients` (we require the NTFS client at index 0) |
| 0a/0c | `client_free_list` / `client_in_use_list` (`0xffff` = none). In-use list empty ⇒ log closed |
| 0e | `flags`: bit 1 `VOLUME_IS_CLEAN`, bit 0 `SINGLE_PAGE_IO` |
| 10 | `seq_number_bits` |
| 14 | `restart_area_length`; 16 `client_array_offset` (0x30 on Win2k, 0x40 on XP+) |
| 18 | `file_size` |
| 20 | `last_lsn_data_length` (client data length of the record at `current_lsn`) |
| 24 | `log_record_header_length` (0x30) |
| 26 | `log_page_data_offset` (0x40 for 4 KiB pages) |
| 28 | `restart_log_open_count` |

Client record (0xa0 bytes, one per client): `oldest_lsn`@00, `client_restart_lsn`@08
(the NTFS checkpoint), prev/next client @10/12, `seq_number`@14, name length @1c,
UTF-16 name @20 (`"NTFS"`).

**State decision** (`ntfs_logfile_is_clean`, same as the Linux drivers): EMPTY (both
pages 0xff), CLOSED (no client in use) or `VOLUME_IS_CLEAN` ⇒ clean, mount rw
without replay. Otherwise DIRTY. CORRUPT (no usable page) and UNSUPPORTED
(version) are never clean. A dirty volume must not be mounted rw without replay
and must never be "fixed" by resetting the log (PORTING.md §6).

### 2.4 Record page (`RCRD`)

| off | field |
|---|---|
| 08 | `last_lsn`: lsn of the last record **header** on the page. **v1 tail copy: the file offset of the copied page instead** |
| 10 | `flags`: bit 0 `LOG_RECORD_END` (a record ends on this page) |
| 14/16 | `page_count` / `page_position` of a multi-page I/O transfer |
| 18 | `next_record_offset` (== page size when the page is full / the record continues) |
| 20 | `last_end_lsn`: lsn of the last record that **ends** on the page |
| 28 | usa (v1.1) |
| 3c | `file_off` (v2.0 only): 0 on in-place pages, copied page's offset on tail copies |

Records start at `log_page_data_offset` (0x40) and are 8-aligned. A record whose
client data does not fit continues at `data_off` of the next page (flag
`MULTI_PAGE` in the LFS header), wrapping from `l_size` to the first record page
with the sequence number incremented.

### 2.5 LFS record header (0x30 bytes)

`this_lsn`@00, `client_prev_lsn`@08, `client_undo_next_lsn`@10, `client_data_len`@18,
client seq/idx @1c/1e, `record_type`@20 (1 client record, 2 client restart =
checkpoint), `transaction_id`@24 (byte offset of the entry in the transaction
table), `flags`@28.

Reader rules (`logfile_records.c`): the record's page must be `RCRD`, its
`last_lsn >= this_lsn`, a continuation page must carry `LOG_RECORD_END` with
`last_end_lsn >= this_lsn`; `client_data_len` is bounded by what fits between the
record and `last_lsn`'s end. Next record = `this_lsn`'s end, skipping to the next
page's `data_off` when fewer than a header's bytes remain.

### 2.6 NTFS client record (0x20 bytes + page LCNs + redo/undo data)

| off | field |
|---|---|
| 00/02 | `redo_op` / `undo_op` (§2.9) |
| 04/06 | `redo_offset` / `redo_length` (from the start of this header) |
| 08/0a | `undo_offset` / `undo_length` |
| 0c | `target_attr`: byte offset of the open attribute entry |
| 0e | `lcns_to_follow` |
| 10 | `record_offset`: byte offset inside the MFT record / index block |
| 12 | `attribute_offset`: offset inside the attribute / entry |
| 14 | `cluster_block_offset` in 512-byte units |
| 18 | `target_vcn` |
| 20 | `page_lcns[lcns_to_follow]` (u64 each; 0 = cluster since freed) |

Byte address of the target inside its attribute: `vbo = target_vcn * cluster_size +
cluster_block_offset * 512`; for MFT ops the record number is `vbo / record_size`.

### 2.7 Checkpoint (`NTFS_RESTART`, record type 2, at `client_restart_lsn`)

`major_ver`@00 (0 or 1), `minor_ver`@04, `start_of_checkpoint`@08, then lsn/length
pairs of the four table dumps: open attribute table @10/@30, attribute names
@18/@34, dirty page table @20/@38, transaction table @28/@3c. Each dump is a
client record with redo op `*TableDump` whose redo data is the table.

`major_ver` picks the open-attribute entry layout: v0 = 0x2c bytes
(`file_ref`@08, `open_record_lsn`@10, `dirty_pages`@18, `type`@1c, `name_len`@20,
`bytes_per_index`@28) and a v0 dirty-page entry with an extra `target_attr`
pointer field; v1 = 0x28 bytes (`bytes_per_index`@04, `type`@08,
`dirty_pages`@0c, `file_ref`@10, `open_record_lsn`@18). v0 dirty-page entries are
converted to the v1 layout in place (fslog.c does the same).

### 2.8 Restart tables (0x18-byte header + `entry_size × total` entries)

`entry_size`@00, `used`@02, `total`@04, `free_goal`@0c, `first_free`@10,
`last_free`@14. An entry whose first u32 is `0xffffffff` is allocated; otherwise it
holds the next free index. Entries are addressed by byte offset (that is what
`transaction_id`/`target_attr` hold).

* Transaction entry (0x28): `state`@04 (1 active, 2 prepared, 3 committed),
  `first_lsn`@08, `prev_lsn`@10, `undo_next_lsn`@18, undo records/bytes @20/@24.
* Dirty page entry (0x20 + 8 × `lcns_follow`): `target_attr`@04, `transfer_len`@08,
  `lcns_follow`@0c, `vcn`@10, `oldest_lsn`@18, `page_lcns[]`@20.
* Attribute names blob: `(entry_offset u16, name_bytes u16, UTF-16 name)*`,
  terminated by a zero entry.

### 2.9 Operations (redo/undo op codes)

| code | op | handler |
|---|---|---|
| 00 | Noop | skip |
| 01 | CompensationLogRecord | skip (adjusts `undo_next` in analysis) |
| 02 | InitializeFileRecordSegment | MFT: copy `dlen` bytes at `record_offset` |
| 03 | DeallocateFileRecordSegment | MFT: clear IN_USE, bump sequence |
| 04 | WriteEndOfFileRecordSegment | MFT: replace from an attribute boundary, set `bytes_in_use` |
| 05 / 06 | CreateAttribute / DeleteAttribute | MFT: insert / remove at an attribute boundary, link count for indexed resident attrs |
| 07 | UpdateResidentValue | MFT: in place when redo/undo lengths match, else resize the attribute |
| 08 | UpdateNonresidentValue | cluster: copy `dlen` at `record_offset`; INDX blocks are re-protected |
| 09 | UpdateMappingPairs | MFT: splice mapping pairs, recompute `highest_vcn` |
| 0a | DeleteDirtyClusters | analysis: zero matching `page_lcns` |
| 0b | SetNewAttributeSizes | MFT: alloc/data/valid (+ total when the attribute has the field) |
| 0c / 0d / 11 / 13 / 21 | Add/DeleteIndexEntryRoot, SetIndexEntryVcnRoot, UpdateFileNameRoot, UpdateRecordDataRoot | MFT: `$INDEX_ROOT` entry ops |
| 0e / 0f / 10 / 12 / 14 / 22 | Add/DeleteIndexEntryAllocation, WriteEndOfIndexBuffer, SetIndexEntryVcnAllocation, UpdateFileNameAllocation, UpdateRecordDataAllocation | cluster: INDX block ops (deprotect, validate, apply, stamp lsn, re-protect) |
| 15 / 16 | Set/ClearBitsInNonresidentBitMap | cluster: `{bitmap_off u32, bits u32}` |
| 17 | HotFix | analysis: replace a dirty page LCN |
| 18 | EndTopLevelAction | analysis: `undo_next` |
| 19 / 1a / 1b | Prepare/Commit/ForgetTransaction | analysis: transaction state / free |
| 1c | OpenNonresidentAttribute | analysis: allocate open-attr entry; name in the undo data |
| 1d / 1e / 1f / 20 | table dumps | checkpoint load |
| 23 / 24 | UpdateRelativeDataInIndex(2) | **no handler: replay refused** |
| 25 | ZeroEndOfFileRecord | MFT: zero `dlen` bytes at `record_offset` |

Every handler validates the target first (`check_file_record`, attribute boundary
checks, index header/entry walks, INDX fixups) and refuses (`-EINVAL`,
`needs_chkdsk`) on any inconsistency. fslog.c skips such records and marks the
volume dirty; we abort so the volume is left exactly as found for chkdsk.

## 3. Algorithm as implemented

1. **Open** (`logfile_open.c`): read both restart pages, validate header and
   area, pick the current one, derive geometry, classify the state (§2.3).
2. **Tail scan** (`lfs_tail_scan`, fslog.c `last_log_lsn()`): load the tail-copy
   pages; walk forward from the page after `current_lsn`'s record, requiring each
   page to carry the expected sequence number and to fit the multi-page transfer
   bookkeeping (`page_count`/`page_position`). A tail copy of the page under
   examination replaces it when the on-disk page is unreadable or the copy ends
   with a later lsn ("override", written back by `mark_clean`). The scan extends
   `last_lsn` to the last record end seen and fixes `next_page`.
   **H4**: after the scan stops at page S, the page following S's transfer must
   not carry a sequence number that could only have been written after S; if it
   does the tail is corrupt and the log is refused.
3. **Checkpoint** (`lfs_load_checkpoint`): read the NTFS_RESTART record at
   `client_restart_lsn`, then the three tables and the names blob from their dump
   records; validate table headers; convert v0 layouts.
4. **Analysis pass**: walk every record after `start_of_checkpoint`. Maintain
   the transaction table (`prev_lsn`/`undo_next_lsn`, states, Forget frees), the
   dirty page table (one entry per `(target_attr, page-aligned vcn)`, `page_lcns`
   filled from each record, `oldest_lsn` = first record touching it), the open
   attribute table (`OpenNonresidentAttribute`) and names; apply
   `DeleteDirtyClusters` and `HotFix`. `redo_lsn` = min over dirty page
   `oldest_lsn` and transaction `first_lsn`.
5. **Dirty-page preparation**: for every dirty page LCN, override the attribute's
   run list for that vcn (this is what the logged LCNs are for: the owning record
   may not have been flushed yet). **Never for `$MFT:$DATA`** (see H1) and never
   for the first four records of `$MFTMirr`.
6. **Redo pass** from `redo_lsn`: for each client record with `lcns_to_follow`
   whose dirty page exists and whose `oldest_lsn <= lsn`: resolve the attribute
   (base record + `$ATTRIBUTE_LIST` extents, through the overlay), shorten the
   redo data by clusters deleted since, then `do_action(redo)`. Targets already
   carrying `lsn >= record lsn` (MFT record `lsn`@08, INDX block `lsn`@08) are
   **skipped**, not re-applied; skips are reported in the plan and not counted
   as redone.
7. **Undo pass**: for every transaction still ACTIVE, follow `undo_next_lsn`
   links and `do_action(undo)` each record whose undo op is not a no-op.
8. **H3** aliasing check: no dirty overlay cluster may lie inside `$MFT:$DATA`'s
   runs (NTFS logs MFT changes as record ops, never as cluster writes).
9. **Flush** (real run only, and only if every step succeeded): MFT records
   (protected, `$MFTMirr` updated for records 0–3), then clusters coalesced into
   runs, then `sync()`. A dry run stops here and returns the plan.
10. **`mark_clean`**: write superseding tail copies back in place, then both
    restart pages as version **1.1**, log closed, `VOLUME_IS_CLEAN`,
    `open_count + 1`, `current_lsn = last_lsn`. This is what fslog.c writes
    (`major_ver = 1`) and, we believe, what Windows 8+ itself does at clean
    dismount (it upgrades a log to 2.0 at mount and downgrades to 1.1 at
    dismount unless `NtfsDisableLfsDowngrade` is set). Unverified, see §7.

Nothing is written before step 9. Any `-EINVAL` sets `needs_chkdsk`; the mount
path must then mount read-only and say so.

## 4. Coherence rules and the refusal policy

The module refuses (rather than skips) whenever the log and the volume disagree.
The rules beyond per-op validation:

* **H1** — an MFT op must target the open attribute for `$MFT:$DATA` (record 0,
  type 0x80, unnamed), and every non-zero `page_lcns[i]` must equal what
  `$MFT`'s own mapping pairs (record 0 and its extents, as already modified by
  earlier records of this replay) give for `target_vcn + i`. Rationale: MFT
  records are written **by number** through the apply vtable (fslog.c likewise
  reads them via `mi_get(rno)` and ignores the logged LCN), so the volume's
  mapping is what will be used; a log that places `$MFT` elsewhere would write
  through the wrong mapping. Also: an MFT op whose `target_vcn` is outside
  `$MFT`'s run list is refused (`$MFT` never shrinks; in a coherent log the
  `UpdateMappingPairs` that grew it precedes the first use of the new page and is
  applied first, invalidating and reloading the run list).
* **H2** — `InitializeFileRecordSegment` is skipped only when a *valid* `FILE`
  record already carries a later lsn; an unreadable, BAAD or zeroed target is
  initialized.
* **H3** — no cluster written by replay may lie inside `$MFT`.
* **H4** — torn-transfer check in the tail scan.

**Why the six original test failures happened** (fixed this session; details in the
commit `f5ba6ba`):

1. H1 was a tautology. Dirty-page preparation overrode `$MFT`'s run list with the
   logged LCN, then H1 compared the logged LCN with the (overridden) run list. A
   record claiming `$MFT` vcn 10 lives at cluster 27 when record 0 says 26 was
   accepted and applied (`test_logfile.c` "wrong logged lcn" case). Fix: `$MFT:$DATA`
   never takes overrides; H1 checks against the mapping pairs, all `lcns_to_follow`
   entries.
2. Skipped records were counted as redone. `do_action` returned 0 both for
   "applied" and "already at or past lsn", so the idempotence test saw
   `records_redone == 2` on a second replay although the plan said SKIPPED. Fix:
   `LFS_SKIPPED` result, counts only applied records.
3. The "unreadable logged lcn" test was wrong. It corrupted **record 6's run
   list**, not the logged LCN; the logged LCN (cluster 40) was valid, and the
   dirty page table is *defined* to take precedence over the run list (the file's
   record may not have been flushed when power was lost — the very case replay
   exists for; fslog.c `run_add_entry` does the same for every attribute). Refusing
   there would break file extension recovery. The test now corrupts the logged LCN
   (read fails, `-EIO`, nothing written) and a second case asserts that a stale
   run list is overridden by the logged LCN.

## 5. Verified vs inferred

| item | status |
|---|---|
| Restart page / area / client record layout, state decision | layout matches the in-tree Linux 7.1 `core/ntfs/logfile.h` and ntfsprogs (v1.1 definitions, three independent sources); run on real data only for the EMPTY case (`tools/images`). An initialized restart page from Windows (capture E1) is still needed |
| Record page header, LFS record header, multi-page/wrap rules | **inferred** from fslog.c + ntfs docs; exercised only by synthetic logs |
| v1 tail copies (pages 2/3, file offset in `last_lsn`) | inferred (fslog.c); synthetic |
| v2.0: first page 0x22, 16+16 tail slots, `file_off`@3c, `data_off` 0x40 | **inferred from fslog.c only**; `test_v2_log` pins the guess, proves nothing about Windows |
| v2.0 multi-page tail transfers | simplified vs fslog.c (page-by-page through `file_off`) — inferred |
| NTFS client record header, op codes | inferred (fslog.c, Linux `logfile.h`) |
| Checkpoint v0/v1 layouts, table formats | inferred (fslog.c) |
| Per-op redo/undo payload layouts (`SetNewAttributeSizes`, bitmap range, index entry ops) | inferred (fslog.c) |
| `lcns_to_follow > 1` per record (512-byte clusters, 4 KiB pages) | inferred; the 512-byte-cluster capture is the test |
| `mark_clean` writing 1.1 pages accepted by Windows/chkdsk | inferred (fslog.c does it; Windows downgrade behaviour from public knowledge) |
| H1–H4 never firing on a coherent Windows log | **unknown** until captures |
| `UpdateRelativeDataInIndex(2)` | not implemented: refused |
| `$ATTRIBUTE_LIST` extents for open attributes | implemented, untested on real data |

## 6. Supported versions

| `$LogFile` version | open | analyze | replay | `mark_clean` |
|---|---|---|---|---|
| 1.0 (Win2k) | yes | yes | yes (same as 1.1) | writes 1.1 |
| 1.1 (XP … 7, and 8+ after clean dismount) | yes | yes | yes | writes 1.1 |
| 2.0 (8/10/11 while mounted, i.e. every dirty volume from a modern PC) | yes | yes | yes, layout inferred | writes 1.1 |
| ≥ 2.1, ≥ 3 | `NTFS_LOG_UNSUPPORTED`, never clean | — | refused | — |
| `CHKD` restart page | reported `CHKDSK` | — | refused ("let Windows finish") | — |

## 6.1 Read-only analysis on a real volume

`ntfs_logfile_analyse(struct ntfs_bdev *, struct ntfs_logfile_analysis *)`
(core/logfile/logfile_analyse.c) runs the analysis, redo and undo passes with
`dry_run`, so every write goes to the in-memory overlay and nothing reaches the
disk. It works on a device rather than a mounted volume, reusing the same
geometry parser the tools use for images (`ntfs_image_open_io`), and needs only
read access. The extension runs it whenever a volume mounts read-only because
its journal is unclean, and publishes the one-sentence result through
`mount-status.json`, so the app can say what a replay *would* do while replay
itself stays refused.

This is the honest answer to "let the user replay at their own risk": the risk
is not that replay fails, it is that an inferred v2.0 layout silently
restructures a filesystem. Reading cannot do that, and what it reports is the
evidence section 7 asks for.

First real-volume result (2026-09-13, a Windows recovery partition on a USB
disk): `state = no usable restart page, version 0.0`. Not a v2.0 log at all —
the restart pages are unusable, so there is nothing to replay and nothing to
learn from it. The same code reports `empty (never written)` on the clean
fixtures, matching `ntfslog`, so the bridge itself is sound.

## 7. Capture list for the Windows PC

Goal: real dirty logs (1.1 and 2.0), plus Windows' own replay result as ground
truth, so the inferred rows of §5 become verified and the phase-4 criterion
("volumes left dirty mount rw and `chkdsk` agrees") can be measured.

### 7.1 Ground rules

* Use small USB sticks (2–8 GB) so whole-partition images are practical. Format
  on the PC: `format E: /fs:ntfs /q /v:CAP1` (4 KiB clusters) and a second stick
  `format F: /fs:ntfs /q /a:512 /v:CAP512` (512-byte clusters → several LCNs per
  log record). Note the Windows build: `ver` and `winver`.
* Windows keeps removable drives on the "Quick removal" policy (write-through, log
  usually clean within a second). For every dirty scenario first switch the stick
  to **Better performance**: Device Manager → Disk drives → the stick → Policies →
  "Better performance", untick "Turn off Windows write-cache buffer flushing" only
  if it is ticked. Re-plug after changing.
* **A dirty capture must be imaged on the Mac before Windows sees the stick
  again.** Windows replays (and cleans) the log at mount; that includes autoplay
  after a reboot. Pull the stick, walk to the Mac, image, *then* return it to
  Windows for the ground-truth image.
* Name captures `<scenario>-<stick>-<winbuild>-{dirty,replayed}.img` and keep the
  console transcript with each.

### 7.2 Imaging on the Mac (all captures)

```
diskutil list                              # find the stick, e.g. disk4, partition disk4s1
diskutil unmountDisk /dev/disk4            # macOS auto-mounts NTFS read-only; unmount anyway
sudo dd if=/dev/rdisk4s1 of=A-cap1-dirty.img bs=1m status=progress
tools/.local/bin/ntfscat  -i 2 -a 0x80 A-cap1-dirty.img > A-cap1-dirty.logfile   # raw $LogFile
tools/.local/bin/ntfsdump_logfile A-cap1-dirty.img > A-cap1-dirty.ntfsprogs.txt  # v1.1 interpretation (may reject 2.0)
./build-logfile/ntfslog -v A-cap1-dirty.img > A-cap1-dirty.ntfslog.txt           # ours: state, tail scan, records, dry-run plan
```

`ntfscat -i 2 -a 0x80` works on the device node too (`/dev/rdisk4s1`) if the
image is not wanted. Never run `ntfslog --apply` on the original image; copy it.

### 7.3 Ground truth on Windows (after the Mac image exists)

Plug the same stick back into the PC, wait 10 s for the mount (Windows replays),
then in an elevated prompt:

```
chkdsk E:                      >> A-cap1-replayed.chkdsk.txt   (read-only check; must be clean)
fsutil dirty query E:          >> A-cap1-replayed.txt
fsutil fsinfo ntfsinfo E:      >> A-cap1-replayed.txt
dir /s E:\                     >> A-cap1-replayed.txt
```

Safely remove, image again on the Mac as `A-cap1-replayed.img`. This image is
the reference our replay must reproduce. Dumping `$LogFile` on Windows itself
is only meaningful *after* replay; for that, an elevated PowerShell can read the
raw partition (`[IO.File]::Open('\\.\E:', 'Open', 'Read', 'ReadWrite')`) or simply
take the Mac image — the pre-replay state can only be captured off-Windows.

### 7.4 Scenarios

**A — file created, then power cut** (transactions: MFT record init, `$I30`
index entry, `$Bitmap`/`$MFT` bitmap bits)
1. Better-performance policy on; stick empty.
2. `cmd`: `echo hello > E:\A.txt & mkdir E:\Adir & echo x > E:\Adir\inner.txt`
3. Within 1–2 s: pull the stick (for an internal SATA disk: pull the PC's power
   plug — the truest version; a stick is the practical one).
4. Image on the Mac (§7.2). Expect `State: DIRTY`, version 2.0.
5. Ground truth (§7.3).

**B — hibernation / Fast Startup with pending writes**
* B1 (hibernate): start `copy C:\path\200MB.bin E:\B1.bin` in one window and
  within a second run `shutdown /h` in another (or press the Hibernate menu
  item). Once the PC is off, pull the stick; image on the Mac. Hibernation does
  not dismount volumes: the log should be open with the copy's transactions.
* B2 (Fast Startup): enable Fast Startup (Control Panel → Power Options →
  "Choose what the power buttons do"), write `echo fs > E:\B2.txt`, then
  `shutdown /s /hybrid /t 0`. Pull the stick when the PC is off; image. This
  tells us whether Windows leaves removable volumes dirty on a hybrid shutdown
  (internal volumes are known to be left dirty; removable ones may be flushed).
  Either result is useful; record which.
* Ground truth for both: boot the PC **without** the stick first (so the
  hibernated session does not resume with it attached), then §7.3.

**C — pending directory index update** (`$INDEX_ALLOCATION` ops, index block
split, `WriteEndOfIndexBuffer`)
1. `mkdir E:\idx` and `for /L %i in (1,1,80) do @echo x > E:\idx\a-deliberately-long-file-name-to-fill-index-blocks-%i.txt`
   (the index leaves `$INDEX_ROOT` and gets several INDX blocks). Safely remove
   and re-plug so this part is clean.
2. `del E:\idx\a-deliberately-long-file-name-to-fill-index-blocks-7.txt & echo y > E:\idx\zz-new-entry.txt & ren E:\idx\a-deliberately-long-file-name-to-fill-index-blocks-40.txt renamed.txt`
3. Pull immediately; image; ground truth.

**D — large file extension** (`UpdateMappingPairs`, `SetNewAttributeSizes`,
`SetBitsInNonresidentBitMap` on `$Bitmap`; possibly `$MFT` growth)
1. `fsutil file createnew E:\D.bin 1048576`, safely remove, re-plug.
2. `type C:\path\300MB.bin >> E:\D.bin` (appends, i.e. extends the existing
   attribute) — pull the stick 2–3 s into the copy.
3. Image; ground truth. Also run D on the 512-byte-cluster stick (`lcns_to_follow > 1`).

**E — baselines**
* E1: same operations as A, then "Safely remove" → image → `ntfslog` must say
  CLEAN and show version 1.1 (confirms the downgrade-at-dismount belief and gives
  a real, initialized 1.1 restart page to compare `mark_clean`'s output against).
* E2: the replayed images from §7.3 are the ground truth for every scenario.
* E3 (chkdsk verdict on *our* replay): `cp A-cap1-dirty.img work.img &&
  ./build-logfile/ntfslog --apply work.img`, write `work.img` to a stick
  (`sudo dd if=work.img of=/dev/rdisk4s1 bs=1m`), plug into Windows, `chkdsk E: /f`
  must find nothing, and `dir /s` must match E2's listing. Then compare the
  metadata: `tools/.local/bin/ntfscmp work.img A-cap1-replayed.img` and
  `cmp` of the `$MFT` runs (`ntfscat -i 0 -a 0x80` of both).

### 7.5 What each capture settles

| capture | settles |
|---|---|
| any dirty image | v2.0 restart page/area, first record page, `data_off`, real record headers, real op mix, whether H1–H4 fire on a coherent log |
| A | MFT init + `$I30` root/allocation ops + bitmap ops end to end |
| B1/B2 | whether replay is even needed for the Fast Startup story on removable media |
| C | INDX block ops, fixups, block validation |
| D, D-512 | mapping-pairs/size ops, `DeleteDirtyClusters`, multiple LCNs per record |
| E1 | 1.1 initialized restart pages from a modern Windows; `mark_clean` parity |
| E3 | the phase-4 criterion: chkdsk agrees with our replay |

## 8. Integration notes for `core/vfs/`

At mount: `ntfs_logfile_open` with `io` over MFT record 2's `$DATA` runs and the
volume geometry → `is_clean` → if not, `ntfs_logfile_replay(dry_run=true)`; on
`0` and `!needs_chkdsk`, `replay(dry_run=false)` then `mark_clean`; on any error
mount read-only and surface `ntfs_logfile_last_error`. The `apply` vtable must
read/write MFT records **by number through the volume's own `$MFT` mapping** and
clusters by LCN; buffers are raw on-disk bytes (fixups are handled inside the
module). Until §7's captures pass, keep replay behind a flag and default to the
read-only-with-explanation behaviour from PORTING.md §6.
