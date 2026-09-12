# tools stream — status

Updated: 2026-09-12 (milestone a)

## Done
- `tools/build-ntfsprogs.sh`: builds ntfs-3g **2026.7.7** (Tuxera release tarball,
  sha256 pinned) into `tools/.local/` for arm64, `--disable-ntfs-3g` (no FUSE),
  static `libntfs-3g.a` + headers + mkntfs/ntfsls/ntfscat/ntfsinfo/ntfscp/
  ntfsfix/ntfscluster/ntfsclone (+ ntfslabel, ntfscmp, ntfstruncate, ...).
  Fix over the previous attempt: `--exec-prefix` (otherwise `make install`
  tries to move `libntfs-3g.so*` into `/lib`). 23 s clean build. No brew
  packages needed (pkg-config shim for configure).
- `tools/common/{json,sha256}.{c,h}`: JSON writer/parser and SHA-256 shared
  by mkfixtures and ntfscli.

## In progress
- `tools/mkfixtures/`: generates 7 images + manifests; 6 of 7 verified with
  ntfsinfo/ntfscat/ntfsfix. `attrlist.img`'s fragmented-file case does not
  yet fragment (libntfs-3g allocator prefers the largest free range); being
  fixed by filling the volume so the only free space is the reserved region.

## Next
- `tools/ntfscli/` (+ CMake, stub fallback), `tools/run-tests.sh`,
  `tools/README.md`.

## Known problems / notes
- libntfs-3g: `ntfs_set_ntfs_attrib()` cannot set COMPRESSED on files (dirs
  only); set `ni->flags` before first `ntfs_attr_open()` instead.
  `NVolSetCompression(vol)` required after `ntfs_mount()`.
- libntfs-3g: `ntfs_attr_open()` takes ownership of a non-constant name;
  `ntfs_mbstoucs()` needs `*out == NULL`.
- No inode cache outside the FUSE driver: close children with
  `ntfs_inode_close_in_dir()` while the parent is open.
- mkntfs derives the serial from the clock: images made in the same second
  collide; mkfixtures sets a deterministic one via `ntfslabel --new-serial`.
- `ntfsck` in 2026.7.7 is a quarantined stub ("Unsupported: check_volume()").
