# platform review — adversarial correctness review of the compat layer

Scope: `platform/pagecache/`, `platform/src/`, `platform/tests/`, `platform/include/`
(additive only). Reviewed against how `core/ntfs/` uses the APIs (`mft.c`
map/write of MFT records, `bitmap.c`, `lcnalloc.c`, `compress.c`, `attrib.c`)
and against `core/vfs/` (aops backend, sync/unmount sequence).

Build/test: `cmake -S . -B build-review && cmake --build build-review && ctest --test-dir build-review`,
the same with `-DNTFS_SANITIZE=ON`, and the standalone page cache
(`cmake -S platform/pagecache -B b [-DNTFS_TSAN=ON -DNTFS_SANITIZE=OFF]`).

## Findings

| # | Finding | Severity | Test that demonstrates it | Fix |
|---|---------|----------|---------------------------|-----|
| 1 | `pagecache.c` `writeback_one`: PG_dirty was cleared *before* PG_writeback was set. A concurrent `filemap_write_and_wait_range()` (fsync, `cache_sync_range`, `truncate` paths) collects dirty folios, then waits for `PG_writeback`; in the window it saw neither and returned while the data was still in flight. | High (fsync/sync returned with data not on the device) | `test_pagecache.c: test_sync_visibility` — 3 flushers hammer one folio while the main thread dirties + write_and_wait + checks the fake disk; failed every run before the fix (`iter N, disk has N-1`). | PG_writeback set before the dirty bit is cleared; documented state machine. a8a860f |
| 2 | `filemap_write_and_wait*()` returned 0 when the backend re-dirtied the folio (`ntfs_write_folio_resident` does so whenever `mrec_lock` is busy). `cache_sync_range()` in `core/vfs/api.c` then calls `invalidate_inode_pages2_range()`, which drops the still-dirty folio: the write is lost. | High | `test_pagecache.c: test_sync_retries_redirty` (backend redirties 3×; folio must end clean and on disk). | `write_range_sync`: bounded retry (16 × 1 ms) on redirty for synchronous flushes; background writer unchanged. a8a860f |
| 3 | `folio_free()` decremented the global live-folio counter for standalone pages from `alloc_page()` (`mem.c`) that never incremented it. Every `compress.c` block allocation/free cycle drove the counter further negative, so `NTFS_PAGECACHE_MAX_FOLIOS` reclaim eventually never fires (unbounded metadata cache). | High (memory growth on compressed volumes) | `test_pagecache.c: test_standalone_accounting` (counter must return to baseline), `test_reclaim` (300 alloc/free cycles must not disable the cap; reclaim is now exercised in every run via the runtime cap `pagecache_set_max_folios`). | `alloc_page()` built on new `folio_alloc_standalone()`; additive API `pagecache_nr_folios()`, `pagecache_set_max_folios()`. a8a860f |
| 4 | `invalidate_mapping_pages()` decided "unreferenced" under the folio lock, but lookups take their reference under `tree_lock`. With the kernel-correct unlocked `read_mapping_folio()` (vfs stream change, adopted: mft.c maps the locked `$MFT` folio again during extent allocation) a reader could obtain a folio that was being evicted; its subsequent `folio_mark_dirty()` saw `mapping == NULL` and silently dropped the write. | Critical (lost metadata writes) | `stress_pagecache` under ASan/TSan (4/8 runs failed with `check_page` mismatch before the fix; 0/8 after). Deterministic companion: `test_read_while_locked`. | Eviction check moved under `tree_lock` (`__remove_locked_cond`), the analogue of the kernel's `folio_ref_freeze()` under the xarray lock. a8a860f |
| 5 | `pagecache_sync_sb()` flushed in registry (creation) order — `$MFT` first. | Medium (crash ordering) | `test_pagecache.c: test_sync_sb_order` | Two passes: all mappings except inode 0, then inode-0 mappings newest first (`$MFT/$BITMAP` before `$MFT/$DATA`). See "Ordering" below. a8a860f |
| 6 | `inode.c`: `iput()`'s last-reference decision was not atomic with `igrab()` (`i_count` dropped under the sb lock only, `I_WILL_FREE` set later; `igrab()` checks under `i_lock`). An `igrab()` in the window revived an inode that was then evicted and freed underneath it; the second `iput()` evicted/freed it again. `igrab()` also manipulated the LRU without the sb lock. | Critical (use-after-free, double free) | `test_inode.c: test_igrab_race` — evict asserts `i_count == 0` and single eviction; before the fix: `i_count == 1` at evict, double evict, double free. | Decrement + drop decision + `I_WILL_FREE` under `i_lock`; LRU under its own innermost `lru_lock`; LRU/unmount eviction claims victims under `i_lock` (`claim_for_eviction`) so a concurrent revival wins. a8a860f |
| 7 | `sync_inodes_sb()` wrote inodes in list order (newest first): `$MFT` and user records before `$Bitmap`. | Medium (crash ordering) | `test_inode.c: test_sync_order` | Stable partition: inodes 1..15, then ≥16, then inode 0 (newest first). a8a860f |
| 8 | `work.c`: the worker wrote `work->state` after `work->func()` returned. The kernel allows a work function to free its own `work_struct`. | Medium (UAF; `precalc_work` is embedded in `ntfs_volume` today, so latent) | `test_bdev.c: test_work` self-freeing items — ASan `heap-use-after-free in worker_main work.c:59` before the fix. | Running state kept in `wq->running`; the item is never touched after its function returns. a8a860f |
| 9 | `bdev_file.c`: sub-block writes on a raw device are a read-modify-write of the covering block with no serialisation: two threads updating different bytes of one block lost each other's bytes (4Kn disks: `i_blkbits`-sized folio tails are sub-block). | High on 4Kn devices | `test_bdev.c: test_rmw_race` (8 lanes in one 4 KiB block; every lane lost updates before the fix). Uses the additive test hook `ntfs_bdev_file_set_alignment()`. | Per-device `rmw_lock` around the RMW. a8a860f |
| 10 | `bdev_file.c` `file_flush`: an `EIO` from `F_FULLFSYNC` was masked by a succeeding `fsync()`. Also: `pread/pwrite` counts > `INT_MAX` fail on macOS; `/dev` nodes whose `DKIOCGETBLOCKCOUNT` fails got size 0 (every read `-EIO`). | Medium | Reviewed (no portable test for a failing `F_FULLFSYNC`). | Fall back to `fsync` only for `ENOTSUP/ENOTTY/EINVAL`; I/O chunked at 1 GiB; `lseek(SEEK_END)` size fallback. a8a860f |
| 11 | `asm/byteorder.h` `le*_to_cpup()` took typed pointers; UBSan flags misaligned pointers into packed on-disk structs (LZNT1). | Low (UBSan noise, UB by the letter) | Integrator report. | `const void *` parameters, memcpy-based. a8a860f |
| 12 | Every `folio_unlock()`/`folio_end_writeback()` took a global mutex and broadcast a global condvar. | Perf (rule 4: no global lock on the hot path) | `stress_pagecache` throughput. | Broadcast only when `state_waiters > 0` (Dekker-style seq_cst pair; no lost wake-up, see comment). a8a860f |
| 14 | `bd_mapping` (the page cache over the raw device) was created only by `bdev_file.c`, though `bdev.h` declares it for every `struct ntfs_bdev` and kernel code dereferences it unchecked. Any other bdev got NULL: the FSKit bridge segfaulted in `idx_of()` via `filemap_write_and_wait_range()` from `ntfs_empty_logfile()` on the first read-write mount of a volume with a non-empty journal. | High (crash on a normal mount; invisible to every fixture, which all have empty journals) | Real Windows-formatted 1 TB disk; `EXC_BAD_ACCESS ... at 0x168` with that stack. No unit test: the FSKit bdev is not reachable from ctest. | Folio ops moved verbatim to `platform/src/bdev_mapping.c` (they only call `ntfs_bdev_read/write`, so they suit any device) behind `ntfs_bdev_attach_mapping()`/`detach`, called from both constructors. 6835a53 |
| 15 | `pagecache_sync_sb()` walked **every** mapping on the volume, twice, calling `filemap_write_and_wait()` on each. FSKit asks for a sync roughly every second file operation, so with a large directory cached each unlink paid O(cached mappings) and deleting a directory was quadratic: the same 1000 deletes cost 1.61 s in a 1000-file directory and 7.67 s in an 8000-file one, with the device only 10-34% busy. | High (delete is the one metadata op outside 2x of Apple's exFAT module, and it degraded with directory size) | Measured on a 1 GiB image on internal NVMe: `delete/s` 854 -> 224 across 500 -> 8000 files before; extension CPU per 1000 unlinks 1.17 s -> 7.57 s; `sample` and the bdev's own `io window` counters both put the time in `sync_filesystem`, not I/O. | A maybe-dirty list (`g_dirty`) that sync walks instead of the registry. One-sided invariant: everything dirty is listed, listed things may be clean. Added on the 0 -> 1 `nrdirty` transition from `folio_mark_dirty()` *after* `tree_lock` is dropped, since `registry_lock` is the outer lock; removed only by a sync pass that has just waited for that mapping's writeback, which is when "clean" also means "nothing in flight". Result: delete/s at 8000 files 224 -> ~440. 08c6f10 |
| 16 | `sync_inodes_sb()` -> `snapshot_inodes()` walked `sb->s_inodes` on every sync, the same O(cached) problem as finding 15 on the mapping side. | High (the other half of the delete degradation) | Same workload: with finding 15 alone, delete/s across 500 -> 8000 files was 900 -> 445. | `sb->s_dirty_inodes` + `inode->i_dirty_list`, same one-sided invariant. Added from `__mark_inode_dirty()` on the clean -> dirty edge after `i_lock` is dropped, and from the page cache when a mapping first dirties (`sync_inodes_sb` selects on mapping dirtiness too). Pruned only after `write_inode_now(sync=1)`, which has waited for that inode's writeback. Result, from a same-session A/B against the pre-fix platform: delete/s 800/443/210 -> 1024/986/735 at 500/2000/8000 files, i.e. close to flat; creates unchanged (669/655/677 -> 675/674/701). NB this is what Linux's VFS does with `wb->b_dirty`; only the driver is ported here, not the VFS, so the naive version was ours. b58b5e8 |
| 17 | The write-back order "newest first within a rank" -- which puts $MFT's attribute inodes, and so the bitmaps, before $MFT itself -- was an accident of `s_inodes` being built with `list_add()`. Walking a dirty list instead silently reversed it. | High (crash consistency; `test_sync_order` failed the moment the list changed) | `platform_inode: test_sync_order` -- `write_order[2] == mftbmp` failed. | An explicit `inode->i_seq` assigned at allocation, and `snapshot_inodes()` sorts by (rank, i_seq descending) rather than relying on insertion order. The rule is ours, not upstream's: Linux has no such ordering and leaves it to the filesystem. b58b5e8 |
| 13 | `folio_put()` reaching zero for a folio still in the tree was silent. | Debug aid | — | `WARN_ON(folio->mapping)` on free; the lost-insert-race path clears `mapping` first. a8a860f |

## Ordering decided for `pagecache_sync_sb()` / `sync_inodes_sb()`

The kernel gives NTFS no ordering (and this port does not drive `$LogFile`),
so the choice is about which crash leaves the volume in the more benign
state:

- bitmap bit set, MFT record not written → leaked space, chkdsk reclaims;
- MFT record written, bitmap bit clear → the next allocation reuses live
  clusters / MFT slots → corruption.

Hence: everything else first, then the mappings of inode 0. Among the
inode-0 mappings, newest first: `$MFT/$BITMAP` is an attribute inode created
after `$MFT/$DATA` (both carry `i_ino 0`), so the MFT bitmap precedes the
MFT data. `sync_inodes_sb()` uses the same idea per inode: system inodes
1..15 (`$Bitmap`, `$MFTMirr`, …), user inodes, then inode 0 newest first;
within one inode `write_inode_now()` writes its folios (index blocks, data)
before its MFT record. Note that `mft.c` writes MFT records synchronously
through bios from `write_inode`, so this ordering governs the folios flushed
by the cache (bitmaps, index allocations, resident-attribute views, MFT
folios dirtied in place). `sync_filesystem()` then runs `sync_fs` and
flushes the device.

## Reviewed, judged risky, not changed

- `invalidate_inode_pages2_range()` drops *dirty* folios (kernel: refuses
  with `-EBUSY`) and reports `-EBUSY` for referenced ones (kernel: drops
  them). `core/vfs/api.c` relies on the drop-dirty behaviour after a
  resident write ("cached view is stale"); with finding 2 fixed, the
  `write_and_wait` + `invalidate2` pairs no longer lose data in the
  transient-lock case, but a caller that itself holds `mrec_lock` across
  `cache_sync_range()` would still discard its dirty folio. Left as is;
  vfs should use `truncate_inode_pages_range()` where it means "discard".
- `read_mapping_folio()` returns an uptodate folio unlocked (kernel
  semantics). `ntfs_write_mft_block()` clears uptodate and applies MST
  fixups in place under the folio lock; a reader that already holds the
  folio can copy a record mid-fixup (the upstream driver has the same
  window; `post_read_mst_fixup` rejects a torn copy with `-EIO`).
- A write error leaves the folio clean with `PG_error` and the mapping's
  `wb_err` set (kernel semantics): a transient `-EIO` loses that update.
  Re-dirtying would risk an endless retry loop against a dead device.
- LRU eviction may recurse: `write_inode_now(victim)` →
  `ntfs_inode_sync_filename()` → `iput(parent)` → `shrink_lru()` → next
  victim. Depth is bounded by the LRU overage and no locks are held across
  the write, but the stack can grow after a burst of `iput()`s.
- `truncate_inode_pages_range()` handles only `lend == -1` fully (all
  callers); a partial *last* folio would be dropped rather than zeroed.
- `krealloc(p, 0)` frees and returns NULL (kernel returns `ZERO_SIZE_PTR`);
  the only caller passes a non-zero size.
- `kmem_cache_alloc()` runs the constructor on every allocation (kernel:
  once per slab object, objects reused constructed). This is a superset of
  what `inode_init_once` needs; `__GFP_ZERO` is ignored when a constructor
  exists (no caller uses `kmem_cache_zalloc` on such a cache).
- `wait_on_freeing()` polls at 50 ms as a safety net besides the generation
  broadcast; `wait_event()` polls at 100 ms. Both are correctness-neutral.
- Every inode registers its mapping with the writer thread; a pass is
  O(#inodes) per 200 ms tick. Fine at 4096 cached inodes; revisit if the
  inode cache cap grows.

## Status

Done: findings 1–13 fixed and tested; `ctest` green in `build-review`,
`build-review` with `-DNTFS_SANITIZE=ON`, standalone page cache under ASan
and TSan (8× / 4× stress loops).

## Semantics: what Linux guarantees, what we do (2026-09-14)

The table the section below asked for. `platform/` reimplements ~174 Linux
functions from their signatures rather than porting them, and that is where this
project's bugs come from -- so this records, per function, what Linux promises,
what we actually do, and whether the difference is deliberate. Rows are ordered
by how much damage the difference can cause.

`docs/LOGFILE.md` §5 does the same job for the journal (verified vs inferred).
**Add a row here before changing any shim**, and treat an undocumented
divergence as a bug rather than a design choice.

### Divergences that have caused bugs

| function | Linux | us | status |
|---|---|---|---|
| `sync_blockdev()` | `filemap_write_and_wait(bdev->bd_mapping)`; **no barrier** | was a full device flush, so every `ntfs_fsync()` issued two and every `sync_filesystem()` three | **fixed** a0e6310 |
| `sync_inodes_sb()` | walks `wb->b_dirty`, the per-bdi dirty list | walked all of `sb->s_inodes`, making every unlink O(cached inodes) | **fixed** b58b5e8 (finding 16) |
| our `pagecache_sync_sb()` | (Linux has per-bdi lists) | walked the whole mapping registry twice per sync | **fixed** 08c6f10 (finding 15) |
| write-back ordering | none; Linux leaves ordering to the filesystem | ours: rank, then newest-first, so `$MFT`'s attribute inodes precede `$MFT` | **ours by design**, now explicit via `inode->i_seq` (finding 17) |
| `bd_mapping` | always present on a block device | only `bdev_file.c` created one; the FSKit bdev had NULL and crashed | **fixed** 6835a53 (finding 14), contract now tested |
| `filemap_write_and_wait()` | returns when writeback completes | same, plus a bounded retry when the backend re-dirties the folio | **ours by design** (finding 2); without it `ntfs_write_folio_resident` loses writes |

### Divergences that are live

| function | Linux | us | why it matters |
|---|---|---|---|
| folio dirty granularity | buffer heads track sub-page dirtiness | whole folio only | a partial write dirties the entire folio. Harmless today because the macOS UBC hands us 16 KiB requests anyway (see vfs.md), but it is why sub-page tracking is not available if that changes |
| `page_cache_sync_readahead()` | issues readahead | **no-op** (`pagemap.h`) | we never read ahead. Measured 2026-09-13: the device is 97% busy during a sequential read, so there is nothing to win here now; revisit only on faster media |
| `errseq_check()` / `errseq_sample()` | per-fd once-only writeback error reporting | **no-op**; we keep a single `mapping->wb_err` | a writeback error is reported to whoever syncs next, not once per fd. Two syncing threads can both miss or both see it. Note this carries *writeback* errors only: a refused device barrier is not recorded here in Linux either, which is why finding 12 was fixed by keeping `blkdev_issue_flush()`'s return at the call sites rather than by implementing `errseq_t` |
| `PAGE_SIZE` | the host MMU page | fixed 4 KiB (`NTFS_PAGE_SHIFT` 12) regardless of host | deliberate (PORTING.md §6) so the cache is independent of a 16 KiB host page. Do not confuse it with `SC_PAGE_SIZE`; doing so produced a wrong diagnosis on 2026-09-13 |
| `bdev_freeze()` / `bdev_thaw()` | quiesce the filesystem | **no-op** | nothing freezes volumes here; a future snapshot feature would need real ones |

### No-ops that are correct

Userspace makes these vacuous, and they are safe: `kunmap_local`, `kunmap`,
`kunmap_atomic` (no highmem), `flush_dcache_page`/`flush_dcache_folio` (no
aliasing VIPT caches to worry about), `invalidate_kernel_vmap_range`,
`memalloc_nofs_save`/`restore` (no reclaim recursion into the filesystem),
`dput` (no dentry cache), `register_filesystem`/`unregister_filesystem`/
`kill_block_super` (no VFS to register with), `BUILD_BUG`.

### Not implemented at all

`blkdev_issue_discard` is wired but the FSKit resource exposes no TRIM on
macOS 26, so `discard_granularity` is 0 and the core never asks.

## The gap this layer keeps falling into (2026-09-13)

`platform/` is ~5,200 lines implementing ~174 Linux functions. Linux implements
every one of them and has hardened them for two decades. We did not port them;
we reimplemented them from their signatures and from what the driver appeared to
need. That was a deliberate choice (PORTING.md §4: Tier 1 is kept line-for-line
and "compiled against a compatibility layer"), and the reason not to vendor is
sound: `fs/fs-writeback.c` pulls in backing-device info, workqueues, cgroup
writeback and RCU, with no clean cut.

But count where the defects come from. Of everything found on 2026-09-13, one
bug was in ported NTFS code (the `$LogFile` clean rule, and that was a
misreading of a rule `logfile.h` states plainly). The rest were ours:

- sync walking every inode and every mapping (findings 15, 16)
- the write-back order surviving only as an accident of list insertion (17)
- a block device with no `bd_mapping` (14)
- and, earlier, the writeback-visibility, lookup-vs-invalidate, work UAF and
  RMW races in findings 1-13.

The vendored driver has been comparatively quiet. The layer we invented is where
the bugs live, and every fix converged on what Linux already does: the dirty
lists are `wb->b_dirty`; the remaining random-write gap (a 4 KiB write dirties a
16 KiB folio, so writes amplify 4x on Apple Silicon) is what buffer heads exist
to solve. Those were reached with a profiler rather than from the reference,
which is slower and less reliable than reading.

**What is missing is a semantics table.** `docs/LOGFILE.md` §5 does exactly this
for the journal: verified versus inferred, row by row. `platform/` has no
equivalent -- only an ownership contract and this findings list. The proposal,
not yet done:

> For each Linux function `platform/` provides, ranked by the call counts
> already in PORTING.md §"Linux API the core actually uses": what Linux
> guarantees, what we do, and whether the difference is deliberate. That turns a
> class of bug currently found by profiling into one findable by reading.

Known divergences to seed it with, all from this session:

| function | Linux | us |
|---|---|---|
| `sync_inodes_sb` | walks `wb->b_dirty` | walked all of `sb->s_inodes` until finding 16 |
| `pagecache_sync_sb` (our name) | per-bdi dirty lists | walked the whole registry until finding 15 |
| write-back ordering | no such rule; left to the filesystem | ours, newest-first within a rank, now explicit via `i_seq` |
| folio dirty granularity | buffer heads track sub-page dirtiness | whole folio; 4x write amplification at 16 KiB pages |
| `bd_mapping` | always present on a block device | only `bdev_file.c` created one until finding 14 |

## Tests written 2026-09-14, and what each one would have caught

Every case below is a bug that reached a real disk before anything tested it.
Each was verified by reintroducing the original defect and confirming the test
fails, which is the only evidence that a regression test is worth its lines.

| test | the bug it locks down | proven by |
|---|---|---|
| `core/tests/test_mount.c: journal_clean_rule` | `ntfs_glue_logfile_clean()` requiring closed **and** flagged clean, which held nearly every cleanly dismounted Windows volume read-only (6835a53) | reverting `\|\|` to `&&` -> 5 failures |
| ...its differential half | the mount path and `core/logfile` disagreeing about one decision on the same bytes | same revert -> "the two implementations DISAGREE" |
| `...: nonempty_journal_rw` | every fixture having an EMPTY `$LogFile`, so `ntfs_empty_logfile()` returned at its first line and its real path had never run under any test | same revert -> read-only where read-write was required |
| `...: probe_light_matches` | `ntfs_probe_light()` checked against `ntfs_probe()` once by hand with a throwaway program (eea4f90), then deleted | kept as a test over all 7 fixtures |
| `...: one_flush_per_sync` | `sync_blockdev()` and `blkdev_issue_flush()` both meaning a device flush, so an fsync issued two and a volume sync three (a0e6310) | counts flushes through a bdev shim; asserts exactly 1 |
| `platform/tests/test_bdev.c: bd_mapping_contract` | only `bdev_file.c` attaching a `bd_mapping`; the FSKit bridge built its own bdev, got NULL, and killed the extension (6835a53). `test_rw` asserted it for the one implementation that always had it -- this tests the *contract* on a hand-built device | deleting the attach from a constructor -> FAIL |
| `fskit/scripts/tests/test-enable-module.sh` | the prune loop aborting the script silently, and the guard piping `lsof` into `grep -q` so it never fired (506ba4a, c4ec674) | reintroducing both -> 5 of 7 fail |

The restart-page builder in `test_mount.c` is the reusable part: it writes a
valid v1.1 restart page pair into a fixture's `$LogFile`, which is what the
project lacked. Note `check_ra()` requires `seq_number_bits == 67 -
bit_length(file_size)` exactly; a hand-built page that gets this wrong is
rejected as CORRUPT with no hint as to why.

Still untested, and honestly so: 3,070 lines of Swift with no test target, and
the FSKit extension's own request path, which is not reachable from `ctest`.

## The sanitizer that found half of these no longer runs (2026-09-14)

Findings 1-13 above lean on AddressSanitizer: finding 8 is a use-after-free
caught by ASan, and the races in 1, 2 and 5 were found the same way. **ASan
deadlocks in its own initialiser on macOS 26 / Apple silicon**, before `main()`,
so an instrumented binary produces no output and never exits. Verified on every
test target here, not just the logfile suite where it was first noticed.

That safety net has therefore been unavailable on this machine for some time,
on a project whose hardest bugs are memory-lifetime bugs in a hand-written page
cache. Nothing detected it because the only suite that enabled sanitizers by
default was also the only suite nothing ran.

Now: UBSan is on by default for the tests (it works, and it is the right tool
for the unaligned loads this port makes out of packed on-disk structures), ASan
is `-DNTFS_ASAN=ON` with the hang documented at the option. Retry ASan after an
Xcode update; until it works, a memory-lifetime bug of the kind in findings 1-13
would have to be found by reading.

