# Porting Linux 7.1 `fs/ntfs` to macOS FSKit

Working title: a free, open-source, kext-less NTFS read/write driver for macOS,
built by porting the Linux kernel's new `ntfs` driver (ntfsplus) to run as a
user-space FSKit file system extension.

Status (2026-09-14): phases 0–3 done. Real NTFS disks mount read/write in
Finder on macOS 26.3 and 26.6.2 from a notarized, Developer ID-signed DMG that
enables its own extension. Phase 4 (`$LogFile`) ships analysis always and replay
only on an explicit per-volume instruction; its apply path has still only run
against synthetic logs. Phase 5 in progress: metadata and write throughput now
meet the "within 2× of Apple's exFAT" bar except random 4 KiB writes, and
`fsck.ntfs` runs on every write test. **The two open gates are both `chkdsk`:**
the write path has never been checked by Windows, and neither has replay. No
x86_64 build, no `mkfs`/`fsck` in the app, no Homebrew cask. See
`docs/progress/*.md` for the detail and the current numbers.

## 1. Why this source

Linux 7.1 (June 2026) merged a rewritten NTFS driver by Namjae Jeon (author of
the Linux exFAT driver). It passes 326 xfstests vs 273 for Paragon's `ntfs3`,
which it is slated to replace. It derives from Anton Altaparmakov's original
`fs/ntfs` (the same lineage as `libntfs-3g`) with write support, iomap-based
I/O and folio-based caching added. It is the most modern, best-tested,
GPL-licensed NTFS write implementation that exists.

Nothing else was suitable:

- `ntfs3` — being retired upstream; concurrent-delete directory bugs; its
  `$LogFile` replay is reported non-functional by the ntfsplus author.
- `libntfs-3g` — 20 years of mileage but a single global lock, no journal
  replay, and a design from 2006. (Still useful as ground truth for tests.)
- Rust `ntfs` crate — read-only.

Vendored pristine copy: `upstream/linux-v7.1/fs/ntfs/` (see `upstream/UPSTREAM.md`).

## 2. Dependency survey

27 C files, 31,591 lines of `.c` (36,270 with headers). Counts are grep hits
per category, from the survey run on 2026-09-12; they size the porting effort
per file, not its difficulty.

| File | Lines | VFS | folio/page cache | iomap | bio/bdev | locks | alloc | Tier |
|---|---:|---:|---:|---:|---:|---:|---:|:-:|
| attrib.c | 5551 | 38 | 39 | 3 | 0 | 57 | 24 | 1 |
| inode.c | 3787 | 71 | 74 | 4 | 9 | 52 | 24 | 2 |
| mft.c | 2962 | 21 | 182 | 2 | 41 | 69 | 8 | 1 |
| super.c | 2786 | 29 | 74 | 1 | 12 | 38 | 55 | 1/2 |
| index.c | 2133 | 11 | 0 | 0 | 0 | 0 | 23 | 0 |
| runlist.c | 2085 | 0 | 3 | 0 | 0 | 0 | 14 | 0 |
| namei.c | 1693 | 76 | 0 | 0 | 0 | 33 | 41 | 2 |
| compress.c | 1578 | 3 | 58 | 0 | 11 | 19 | 18 | 1 |
| dir.c | 1245 | 14 | 25 | 0 | 1 | 10 | 32 | 1 |
| file.c | 1160 | 64 | 20 | 25 | 6 | 25 | 2 | 2 |
| lcnalloc.c | 1049 | 3 | 20 | 0 | 3 | 7 | 2 | 1 |
| ea.c | 957 | 23 | 0 | 0 | 0 | 15 | 16 | 1 |
| iomap.c | 882 | 22 | 31 | 172 | 1 | 39 | 4 | 2 |
| logfile.c | 781 | 9 | 27 | 0 | 2 | 3 | 13 | 1 |
| reparse.c | 574 | 10 | 0 | 0 | 0 | 7 | 8 | 0 |
| unistr.c | 477 | 0 | 0 | 0 | 0 | 0 | 13 | 0 |
| aops.c | 305 | 5 | 25 | 28 | 7 | 2 | 0 | 2 |
| bitmap.c | 290 | 1 | 34 | 0 | 3 | 0 | 2 | 1 |
| attrlist.c | 288 | 3 | 0 | 0 | 0 | 0 | 3 | 0 |
| mst.c | 194 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| debug.c | 171 | 2 | 0 | 0 | 0 | 0 | 0 | 0 |
| object_id.c | 158 | 5 | 0 | 0 | 0 | 4 | 3 | 0 |
| collate.c | 146 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| bdev-io.c | 120 | 1 | 14 | 0 | 18 | 0 | 3 | 2 |
| quota.c | 95 | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| upcase.c | 70 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| sysctl.c | 54 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

**Tier 0 — portable as-is (~6,400 lines).** Pure on-disk logic: run lists,
index B+-trees, Unicode, MST fixups, collation, attribute lists, reparse
points. Need only byte-order macros, allocators and `list.h`.

**Tier 1 — core that reads metadata through the page cache (~14,800 lines).**
The driver's central design (inherited from Altaparmakov): `$MFT`, `$Bitmap`,
`$LogFile`, `$MFTMirr` and every attribute's data are *inodes*, and the core
reads them with `read_mapping_folio(inode->i_mapping, index)` then
`kmap_local_folio`. `mft.c` alone does this 182 times. This tier is kept
line-for-line where possible and compiled against a compatibility layer that
provides a user-space page cache.

**Tier 2 — Linux VFS glue to be replaced (~8,000 lines).** `inode.c` (inode
ops/`iget`), `namei.c` (lookup/create/rename → dentries), `file.c`
(read/write via iomap + dio), `iomap.c`/`aops.c` (address-space ops),
`bdev-io.c` (bio submission), `sysctl.c`. These are rewritten as the FSKit
side, using the Tier 0/1 core as the engine. `inode.c` and `super.c` are
mixed: their on-disk parsing (`ntfs_read_locked_inode`, boot-sector and
system-file loading) stays; their VFS plumbing goes.

### Linux API the core actually uses

`struct inode` fields: `i_sb i_mode i_mapping i_size i_blocks i_ino i_lock
i_uid i_gid i_generation i_nlink i_flags i_blkbits` (plus `i_op/i_fop`, which
are Tier 2 only).

Page cache / I/O family (hit counts): `folio_put(53) kunmap_local(45)
folio_unlock(38) read_mapping_folio(25) kmap_local_folio(21) folio_lock(14)
folio_mark_uptodate(13) folio_size(12) folio_mark_dirty(9) iomap_zero_range(8)
filemap_write_and_wait_range(6) truncate_setsize(4) __filemap_get_folio(3)
truncate_pagecache(3)` and a long tail; `bio_*`/`submit_bio` appear only in
Tier 2 files and `mft.c`'s MFT-mirror sync.

Headers: `linux/fs.h blkdev.h writeback.h iomap.h pagemap.h slab.h vmalloc.h
highmem.h bitops.h list.h rwsem.h posix_acl*.h xattr.h nls.h uidgid.h
iversion.h fs_context.h fs_parser.h` + `asm/byteorder.h asm/div64.h`.

## 3. Architecture

```
┌──────────────────────────────────────────────────────────┐
│ fskit/            Swift FSKit extension + host app       │
│   FSUnaryFileSystem / FSVolume ops → C bridge            │
├──────────────────────────────────────────────────────────┤
│ core/vfs/         Tier 2 rewritten: inode table, lookup, │
│                   create/rename/unlink, read/write paths │
├──────────────────────────────────────────────────────────┤
│ core/ntfs/        Tier 0 + 1, tracked against upstream   │
│                   (attrib, mft, index, runlist, lcnalloc,│
│                    bitmap, compress, dir, ea, logfile…)   │
├──────────────────────────────────────────────────────────┤
│ platform/         "linux-compat": the subset of the      │
│                   kernel API the core calls, in userland │
│   pagecache/      address_space + folio cache, dirty     │
│                   tracking, writeback                     │
│   inode.h         struct inode / super_block (data only) │
│   lock.h          mutex/rwsem/spinlock → pthread         │
│   alloc.h         kmalloc/kvmalloc/kmem_cache → malloc   │
│   byteorder.h     le16/32/64 helpers, bitops, list.h     │
│   bdev.h          block device vtable (pread/pwrite/sync)│
└──────────────────────────────────────────────────────────┘
        ▲ tools/ntfscli: mount image, ls, cat, cp, rm — the
          same core, no FSKit, for tests and fuzzing
```

Design rules:

1. **Tier 0/1 files stay diffable against `upstream/`.** Fixes land in the
   kernel driver for years to come; we want to merge them. Changes to these
   files are limited to `#include` swaps and the removal of Tier 2 entry
   points. Everything else is done in `platform/`.
2. **The compat layer emulates semantics, not just signatures.** `folio_lock`
   must lock; `folio_mark_dirty` must queue writeback; `read_mapping_folio`
   must read through the owning inode's run list. The MFT is read and written
   only through this cache, so correctness of the cache *is* correctness of
   metadata.
3. **File data bypasses our cache.** The kernel's UBC caches file contents
   above FSKit. FSKit `read`/`write` calls map offset → run list → block device
   directly with large aligned I/O; our page cache is for metadata inodes and
   for read-modify-write of unaligned edges. This is where the throughput
   comes from.
4. **Locking is real from day one.** FSKit dispatches operations concurrently.
   The driver already has per-inode `mrec_lock`, `runlist.lock`, per-volume
   `mftbmp_lock`/`lcnbmp_lock` and so on; the compat layer maps them to
   pthread primitives one-to-one. No global lock.
5. **The core must be testable without FSKit.** `tools/ntfscli` links the core
   and operates on image files; it is how the read and write paths are
   validated against images formatted and checked by real Windows.

## 4. Phases

| # | Milestone | Done when |
|---|---|---|
| 0 | Skeleton: `platform/` headers, Tier 0/1 files compile with clang on macOS, stubs for Tier 2 | `make` produces `libntfscore.a` for arm64 |
| 1 | **Read path**: mount image, `ls`, `stat`, `cat`, compressed/sparse/ADS files, Unicode names | `ntfscli` output matches `ntfsls`/`ntfscat` and a Linux 7.1 mount on a corpus of Windows-formatted images |
| 2 | **Write path**: create, write, truncate, mkdir, rename, unlink, xattr, timestamps, permissions | `chkdsk /f` reports clean after every test on a Windows VM; `fsck.ntfs` (ntfsprogs-plus) clean **[done 2026-09-13: all 7 fixtures, every run, see tools/README.md]**; xfstests-style cases pass in `ntfscli` |
| 3 | **FSKit extension + host app**: probe, auto-mount, unmount, read-only fallback for dirty volumes, menu-bar status | An NTFS USB drive mounts in Finder on plug-in with no terminal, no Recovery, no reboot; survives unplug |
| 4 | **`$LogFile` replay** — our own module; no open implementation currently works | Volumes left dirty by Windows Fast Startup mount rw and `chkdsk` agrees with the result |
| 5 | Performance, x86_64 build, `mkfs`/`fsck` in the app, notarized DMG + Homebrew cask | Throughput at device speed on USB 3 / Thunderbolt SSDs; metadata ops within 2× of Apple's exFAT FSKit module |

Journal replay (phase 4) is the flagship: neither Paragon's open code nor
ntfs-3g nor ntfsplus does it correctly today. References when we get there:
`fs/ntfs3/fslog.c` (Paragon, incomplete), the Linux-NTFS project's NTFS
documentation (Russon & Fledel), and behavior observed from Windows.

## 5. Platform facts that constrain the design

- FSKit: macOS 15.4+. Extension entry point must be Swift (ExtensionKit);
  block-device file systems only (`FSUnaryFileSystem` over
  `FSBlockDeviceResource`); volumes mount under `/Volumes`.
- Entitlement `com.apple.developer.fskit.fsmodule` — a managed capability
  enabled from a paid developer account, not a request-and-wait like kext
  signing.
- Users were expected to enable the module in System Settings → General →
  Login Items & Extensions → File System Extensions. **That switch does not work
  for any third-party FSKit module on macOS 26.3 or 26.6.2**: `fskitd` refuses
  the enable call from every caller lacking a private Apple entitlement, which
  `LoginItems.appex` does not hold. The host app therefore enables itself, which
  is why it ships unsandboxed with Developer ID. Once enabled, recognized volumes
  do auto-mount like Apple's own FSKit exFAT. See fskit/README.md "Known issues".
- Measured on this machine (M-series, macOS 26.3): Apple's FSKit exFAT does
  1.0–1.5 GB/s sequential write and 0.8–1.6 GB/s cold read on an internal
  NVMe-backed image — same as in-kernel APFS. Metadata ops cost ~0.3–0.6 ms
  each (XPC round-trip), 10–20× slower than kernel `stat`. Design rule 3 and
  a warm inode cache address this.
- License: the core is GPL-2.0 (kernel code). The product is GPL-2.0,
  distributed as a notarized DMG and Homebrew cask. Not App Store.

## 6. Decisions (2026-09-12)

| Decision | Choice |
|---|---|
| Minimum macOS | **26**. One FSKit API generation. 15.4 support only if demand appears. |
| Metadata write policy | **Write-back, ordered, flushed within 1 s** and on sync/unmount. Full `$LogFile` journaling (writing records, not just replay) is the later item that makes yanks safe. |
| Filenames illegal on Windows (`: ? * < > \| "` and trailing dot/space) | **Rejected** with `EINVAL`. Everything the Mac writes must open on Windows. |
| Test ground truth | A physical Windows PC for `format` and `chkdsk /f` round-trips on USB media; plus ntfsprogs as a read oracle and ntfsprogs-plus's `fsck.ntfs` as a structural checker on every run. What each suite covers, and what none of them cover, is `docs/TESTING.md`. |
| Running the tests | One entry point, `tools/ci.sh`, covering everything that needs no hardware and no signing certificate. Added 2026-09-14 after two existing suites were found to have been silently not running for a day each. A test nobody runs is not coverage. |
| Page cache logical page | Fixed 4 KiB regardless of host page size. |
| Languages | C core + C compat layer behind one C ABI; Swift only for the FSKit layer. |
| Build | CMake for core/platform/`ntfscli`; Xcode links `libntfscore.a`. |
| Names | Case-insensitive, case-preserving. |
| Ownership | `noowners` semantics; existing security descriptors preserved; new files inherit the parent's security ID. No ACL mapping in v1. |
| macOS xattrs | Stored as alternate data streams. No `._` files. |
| Dirty volume | Read-only mount with the reason shown. Since v0.2 the user may instruct a replay per volume, at their own risk, for journal versions 1.0/1.1/2.0; it is never automatic and never a default. A hibernated volume can have its saved session discarded on the same terms. The journal is still never silently reset. |
