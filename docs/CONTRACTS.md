# Contracts and ownership

Work on this project is split across parallel work streams. Each stream owns
directories exclusively; nobody writes outside their area. Shared interfaces
are the **contract headers**; they are changed only additively (new
functions/fields), never by altering existing signatures or struct layouts,
and every change is noted in the stream's report.

## Contract headers (frozen unless noted in a report)

| Header | Defines |
|---|---|
| `platform/include/ntfsport/config.h` | build constants (`NTFS_PAGE_SHIFT`, write-back interval) |
| `platform/include/linux/types.h` | kernel scalar types, list/hlist/rb nodes, `timespec64`, `PAGE_*` |
| `platform/include/linux/{mutex,rwsem,spinlock,atomic}.h` | locking primitives over pthread |
| `platform/include/linux/mm_types.h` | `struct folio`, `struct address_space`, `address_space_operations` |
| `platform/include/linux/pagemap.h` | page cache API (implemented by `platform/pagecache/`) |
| `platform/include/linux/fs.h` | `struct inode`, `struct super_block`, inode table API (implemented by `platform/src/inode.c`) |
| `platform/include/ntfsport/bdev.h` | block device vtable |
| `core/include/ntfscore.h` | public C ABI used by FSKit and `ntfscli` (implemented by `core/vfs/`) |

Foundational shims written by the integrator, shared by everyone:
`linux/{kernel,err,list,bitops,highmem,errno,time64,uidgid,stat,slab,string,bug,log2,math64,compiler}.h`,
`asm/{byteorder,div64}.h`.

## Streams

| Stream | Owns | Delivers |
|---|---|---|
| **compat** | `platform/include/linux/*` (all remaining shims), `platform/include/asm/*`, `platform/src/` (inode table, bdev_file, misc), `core/ntfs/` (Tier 0/1 sources), `CMakeLists.txt`, `core/CMakeLists.txt` | Tier 0/1 compile to `libntfscore.a` on macOS arm64 with `-Wall -Wextra`; `platform/src/inode.c` implements `fs.h`'s inode table; `bdev_file.c` implements `bdev.h` |
| **pagecache** | `platform/pagecache/` (+ additive changes to `pagemap.h`/`mm_types.h`) | Every function in `pagemap.h`, write-back thread, unit tests with a fake backend |
| **vfs** (starts after compat) | `core/vfs/`, `core/include/` | `ntfscore.h` implemented on top of Tier 0/1: inode table glue, lookup/create/rename/unlink, read/write fast path, aops backend, xattr↔ADS, name policy |
| **fskit** | `fskit/` | Xcode project: host app + FSKit extension + Swift/C bridge + `FSBlockDeviceResource` bdev; builds and mounts against `core/stub/` |
| **tools** | `tools/` | ntfsprogs build script (mkntfs/ntfsls/ntfscat/ntfsinfo/ntfscp), fixture image generator with manifests, `ntfscli` harness against `ntfscore.h`, differential test runner |
| **logfile** | `core/logfile/`, `docs/LOGFILE.md` | `$LogFile` parser, analyzer CLI, replay engine with a callback interface for applying redo records |

`core/stub/` is a `-ENOSYS` implementation of `ntfscore.h` maintained by the
integrator so `fskit/` and `tools/` link before `core/vfs/` exists.

## Tests

Tests are owned by the stream that owns the code under test, with two shared
pieces that belong to nobody in particular and may be edited by anyone:

| path | covers | stream |
|---|---|---|
| `platform/tests/` | inode table, bdev, page cache | compat / pagecache |
| `core/tests/` | mount decisions, probe, flush count | vfs |
| `core/logfile/tests/` | replay engine, `ntfslog` over fixtures | logfile |
| `tools/run-tests.sh` | the differential fixture suite | tools |
| `fskit/NTFSTests/` | the app's Swift logic | fskit |
| `fskit/scripts/tests/` | the shell scripts | fskit |
| `tools/ci.sh` | runs all of the above | shared |

`docs/TESTING.md` is the reference: what each suite covers, what is not covered,
and the rule that a new test must be seen to fail before it is trusted.

## Build

Top-level CMake builds `platform` + `core/ntfs` + `core/vfs` + `core/logfile`
into `libntfscore.a`, plus `tools/ntfscli` and tests. `fskit/` is an Xcode
project that links `libntfscore.a` (`xcodebuild` from the command line; no
Xcode GUI steps may be required).

Flags for all C: `-std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration`.
Test builds add `-fsanitize=address,undefined`.

## Reports

Every stream ends with a report containing: what was built, how to build and
run it, what is stubbed or incomplete, contract changes made (additive only),
and open questions for the integrator.

## Progress notes and commits (every stream)

- Keep `docs/progress/<stream>.md` current at every milestone: done / in
  progress / next / known problems. It is a status file, overwritten, not a log.
- Commit at each milestone, **only your own paths** (`git add <your dirs>
  docs/progress/<stream>.md && git commit`). Never `git add -A`: other streams
  write concurrently. If `index.lock` exists, another stream is committing —
  sleep 2 s and retry (up to 5 times).
- Commit message: `<stream>: <what landed>`, ending with the session's
  attribution lines.
- Never commit build trees, `tools/.local`, `tools/images`.
