# tools stream — status

Updated: 2026-09-12 (milestone b: runner, README; stream complete pending a linkable core)

## Done
- `tools/build-ntfsprogs.sh`: ntfs-3g **2026.7.7** (Tuxera tarball, sha256 pinned)
  into `tools/.local/` for arm64, `--disable-ntfs-3g`; static `libntfs-3g.a`
  + headers + mkntfs/ntfsls/ntfscat/ntfsinfo/ntfscp/ntfsfix/ntfsck/ntfscluster/
  ntfsclone/ntfslabel/... 23 s clean build, no brew packages.
- `tools/common/{json,sha256}.{c,h}`: shared by mkfixtures and ntfscli.
- `tools/mkfixtures/`: 7 images + manifests in 6.8 s (`make -C tools/mkfixtures
  fixtures`). **`attrlist.img` is finished**: `ntfsinfo -f -i 72` shows
  `$ATTRIBUTE_LIST` with `$DATA` spread over MFT records 72/77/79
  (`fragmented.bin`, 600 one-cluster extents); inodes 65 (`many-ads.bin`, 40
  streams) and 73 (`interleaver.bin`) too. The generator dies unless the
  fragmented file has >= 300 runs and an attribute list; regeneration
  reproduces it (0.12 s).
- `tools/ntfscli/` (1,435 lines): probe/info/ls/stat/cat/cp-out/cp-in/mkdir/
  rmdir/rm/mv/ln/truncate/xattr/sync/verify/bench over `ntfscore.h`.
  `CMakeLists.txt` now: links `libntfsplatform.a` after the core when it sits
  next to `NTFSCORE_LIB`, then pthread; detects an archive built for another
  architecture (`lipo -archs`) and builds ntfscli for it with a warning; the
  stray CMake syntax warning is fixed. Stub build verified
  (`ntfscli probe` -> ENXIO, exit 1).
- `tools/run-tests.sh`: builds ntfsprogs/fixtures/ntfscli when missing; modes
  `verify`, `ground-truth`, `write`, `all`, `build`, `fixtures`; `--image`,
  `--stub`, `--ntfscli PATH`, `--core-lib`, `--sanitize`, `--quick`,
  `--max-files`, `--no-data`, `--no-times`, `--keep`, `-v`. PASS/FAIL/SKIP per
  check, summary, exit 1 on any failure, logs in `tools/build/test-logs/`.
- `tools/ntfscli-shim.sh`: ntfs-3g answering `ls -R -l`/`cat`/`xattr get|list`
  in ntfscli's syntax; `run-tests.sh --ntfscli tools/ntfscli-shim.sh
  ground-truth` passes (19/19 checks; 31 s with `--quick`, 1 m 46 s full) — proves the
  runner's normalisers/diff/hash plumbing. `--stub all` fails every check
  with ENOSYS and exits 1, as it must.
- `tools/README.md`: build/run for everything, fixture catalogue, ntfscli
  reference, runner modes, and the Lima + self-built 7.1 kernel recipe for a
  kernel-mount ground truth (documentation only, nothing installed).

## In progress
- Nothing. Waiting on `core/vfs/` to make `build/libntfscore.a` linkable; the
  first real run is `tools/run-tests.sh all`.

## Next
- When the vfs stream lands: run `all`, triage mismatches, then add the
  kernel-mount ingestion (`find ... sha256sum` listing) as a fourth mode.
- Consider adding the runner to `ctest` from the top level (`add_subdirectory(
  tools/ntfscli)` already links the `ntfscore` target directly).

## Known problems / notes
- **`build/libntfscore.a` and `build/libntfsplatform.a` are x86_64** (the
  top-level tree was configured with Homebrew's Intel `/usr/local/bin/cmake`;
  `CMakeCache.txt: CMAKE_COMMAND=/usr/local/bin/cmake`). ntfscli's CMake now
  copes (builds x86_64, runs under Rosetta), but the integrator should
  reconfigure `build/` with a native cmake (`. tools/env.sh && ensure_cmake`).
- With the arch matched, `NTFSCLI_USE_STUB=OFF` links against `build/
  libntfscore.a` cleanly except for the expected undefined symbols: the
  `ntfs_*` API of `ntfscore.h` plus Tier-2 internals (`ntfs_iget`,
  `ntfs_aops`, `ntfs_read_inode_mount`, `ntfs_get_locked_folio`, ...) and the
  two platform shims `dir_emit_dotdot`, `writeback_iter`. No platform symbol
  (bdev/pagecache) is missing. `run-tests.sh` prints this list and fails the
  `build` check until core/vfs lands.
- ntfscli argument order is `COMMAND IMAGE [ARGS]` (`ls IMG -R -l`, `xattr IMG
  get PATH NAME`); the runner and shim follow that.
- macOS bash 3.2: empty arrays under `set -u` need `${a[@]+"${a[@]}"}`.
- ntfsls prints `./` and `../` in sub-directories with `-l -F` and `-F`
  headers carry a trailing slash (`/dir/:`); the normaliser strips both.
- `ntfsck` 2026.7.7 is a quarantined stub (`Unsupported: check_volume()`,
  exit 1): reported as SKIP; `ntfsfix -n` is the consistency gate.
- libntfs-3g: `ntfs_set_ntfs_attrib()` cannot set COMPRESSED on files (dirs
  only); set `ni->flags` before first `ntfs_attr_open()` instead.
  `NVolSetCompression(vol)` required after `ntfs_mount()`.
- libntfs-3g: `ntfs_attr_open()` takes ownership of a non-constant name;
  `ntfs_mbstoucs()` needs `*out == NULL`.
- No inode cache outside the FUSE driver: close children with
  `ntfs_inode_close_in_dir()` while the parent is open.
- mkntfs derives the serial from the clock: images made in the same second
  collide; mkfixtures sets a deterministic one via `ntfslabel --new-serial`.
- libntfs-3g allocator: exact-LCN hint only honoured for appends; see the
  README for how `attrlist.img` forces 600 extents.
