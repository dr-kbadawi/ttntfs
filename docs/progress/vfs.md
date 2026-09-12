# vfs stream — status

Owner: vfs agent. Paths: `core/vfs/`, `core/include/`.

## Done
- `core/vfs/` compiles into `build/libntfscore.a` (`cmake -S . -B build && cmake --build build`;
  `ctest` 4/4). Harness: `cmake -S tools/ntfscli -B build-cli -DNTFSCLI_USE_STUB=OFF`.
- `inode.c`: upstream kept; PORT edits: `ntfs_drop_big_inode` -> `generic_drop_inode`,
  `ntfs_iget` drops (instead of caching) an inode whose read failed, `ntfs_set_vfs_operations`
  without inode_operations tables, `ntfs_show_options` stub, `iomap_zero_range` ->
  `ntfs_vfs_zero_range`, compressed named `$DATA` attribute inodes allowed.
- `namei.c`: `__ntfs_create`/`ntfs_delete`/`__ntfs_link` kept; dentry wrappers replaced by
  `ntfs_vfs_lookup/create/unlink/link/rename` (+ case-only rename via a temp name), `mknod`
  and NFS export ops dropped, `ntfs_check_bad_windows_name` exported.
- `file.c`: fsync, setattr/truncate, fallocate, punch-hole kept (inode based); iomap/dio/mmap/
  llseek/splice/ioctl/fiemap dropped.
- `aops.c`: `ntfs_aops` (resident copy from/to the mft record, run list -> `ntfs_bdev_*`,
  compressed via `ntfs_read_compressed_block`, holes/initialized_size zeroing) and
  `ntfs_mft_aops` (`ntfs_write_mft_block` from mft.c); direct read/write and zero-range helpers.
- `super_glue.c`: mount through `ntfs_fs_type`'s fs_context ops: always read-only first, then
  `reconfigure` to rw only if clean; `ro_reason` = requested/device/hibernated/dirty/
  unsupported/logfile/errors; probe, unmount, sync, statfs -> `ntfs_volume_get_info`, label.
- `api.c`: whole `ntfscore.h`. Read/write: resident from the mft record, compressed through the
  cache, otherwise whole pages direct from the run list to the device with cached edges
  (flush+invalidate around direct spans); write allocates clusters up front, zeroes the gap
  to `initialized_size`, extends it afterwards. xattrs = named `$DATA` streams. `noowners`
  mode/uid/gid policy. `preallocated_size=0` so `allocated_size` never carries slack.
- `compat_extra.c`: `dir_emit_dotdot`, `writeback_iter`.
- Read path: `ntfscli verify` passes 0 mismatches on basic-4k (234), cluster-512 (231),
  cluster-64k (231), attrlist (11), compressed-sparse (23), empty (0), names (5058, see below).

## PORT edits outside core/vfs (minimal, all marked `/* PORT: */`)
- `core/ntfs/mft.c`, `mft.h`: `ntfs_write_mft_block` exported ($MFT write_folio backend).
- `core/ntfs/super.c`: `ntfs_fs_type` no longer static (mount glue uses its fs_context ops).
- `core/ntfs/ea.h`: `#define ntfs_listxattr ntfs_ea_listxattr` (public ABI owns the name).
- `core/ntfs/debug.c`: messages formatted with vsnprintf instead of `%pV` (was printing a
  pointer; every `ntfs_error` was unreadable).
- `core/ntfs/dir.c`: several POSIX names differing only in case no longer abort the lookup
  ("Found already allocated name"); the first candidate is kept while scanning for an exact match.
- `core/ntfs/compress.c`: compressed *named* `$DATA` streams accepted by
  `ntfs_read_compressed_block` (fixture compressed-sparse has one).

## Known problems / open questions
- `tools/common/json.c` `parse_string_raw()` double-encodes raw UTF-8 bytes (each byte >= 0x80 goes
  through `put_utf8()` as a code point). Every non-ASCII manifest path is looked up mangled, so
  `verify names.img` reports 17 missing + 17 extra. With that one line fixed (append the byte),
  names.img verifies 0/5058. Tools stream: please fix.
- Writes to compressed files go through `ntfs_compress_write` (upstream); truncate of compressed/
  encrypted files is refused (upstream limitation). Encrypted data is refused (EOPNOTSUPP).
- No `->release()`: pre-allocation is disabled instead of trimmed on close.
- Windows symlinks (IO_REPARSE_TAG_SYMLINK) are readable through `ntfs_readlink` (print name);
  created symlinks are WSL-style (upstream behaviour).

## Next
- Phase 2: `tools/run-tests.sh write` on every fixture; ntfsfix/ntfsls agreement; sanitizer run.
