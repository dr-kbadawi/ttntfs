# compat stream — status

Owner: compat agent (integrator verified 2026-09-12 21:30). Paths: `platform/include`, `platform/src`, `platform/tests`, `core/ntfs`, `CMakeLists.txt`, `core/CMakeLists.txt`.

## Done
- All 20 Tier 0/1 files in `core/ntfs/` compile; `diff -ru upstream/linux-v7.1/fs/ntfs core/ntfs` is small (PORT-commented).
- `platform/src/`: log, mem, bitmap, rbtree, unicode, work, wait, misc, fs_parser, bio, inode, bdev_file.
- Top-level CMake: `cmake -S . -B build && cmake --build build` produces `build/libntfscore.a` and `build/libntfsplatform.a`; ctest runs platform + pagecache tests.
- Tests passing: platform_bdev, pagecache, pagecache_stress.
- Integrator fixes after cut-off: log.c comment terminator, `s_inode_lru`/`s_nr_inode_lru` added to `super_block` (additive), `ENOPARAM`, `find_inode_nowait` definition vs `_Generic` macro, `PAGE_*` `#undef` before define, `asm/byteorder.h` in unicode.c.

## Known problems (resolved 21:50: inode LRU fixed, 4/4 tests pass)
- `platform_inode` test: 9 failures, all in unreferenced-inode LRU/cache accounting (`inode_cache_count()`, eviction on `iput`, `NTFS_INODE_CACHE_MAX`). See `platform/tests/test_inode.c:185,219,290,307`.
- `core/ntfs` builds with `-Wsign-compare`/`-Wpointer-sign`/`-Waddress-of-packed-member` warnings inherited from kernel code; consider silencing those three for `core/ntfs` only.

## Next
Nothing outstanding for this stream. The LRU accounting was fixed in 81dda8f and
the vfs stream it was blocking landed in fdbbb1c; the layer is now covered by
`platform_inode`, `platform_bdev`, `pagecache` and `pagecache_stress`, and its
divergences from Linux are tabulated in `platform-review.md`.
