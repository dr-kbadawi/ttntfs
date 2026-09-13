# fskit stream — status

Owner: fskit agent. Paths: `fskit/`, `tools/windows/` (round 2).

## Done (round 2, 2026-09-12)
- **Lifecycle fixed to FSKit's order** (activate → mount → … → unmount → reclaims → deactivate):
  the core is mounted in `activate` (it was in `mount`, so `activate` failed with ENXIO before
  any mount could happen), `unmount` = `ntfs_volume_sync`, `deactivate` = `ntfs_unmount` after
  every reclaim, `unloadResource` unmounts late if deactivate never came, then closes the bdev.
- **Directory enumeration**: the core's readdir emits `.`/`..` itself (kernel `dir_emit_dots`,
  cookies 0/1); FSKit cookies are now the core's cookies verbatim (the old `+2` scheme doubled
  the dot entries). Dots are dropped when attributes are requested (FSKit rule). Lag-by-one packing
  honours "an entry for which the callback returned nonzero is not consumed". Verifier is a
  per-directory version bumped on create/remove/rename/link (nonzero as required). Missing
  `has_attr` falls back to `ntfs_inode_get`+`ntfs_getattr`.
- **xattr flags**: the core is built with `linux/xattr.h` (`XATTR_CREATE=1, XATTR_REPLACE=2`);
  Swift passed Darwin's 2/4, so "must create" was read as "replace". Fixed via
  `NTFS_XATTR_*_FLAG` in the bridging header. ENODATA → ENOATTR for xattr calls.
- **Errors**: every core return goes through `platform_errno_to_host()` (declared in the bridging
  header by including `linux/errno.h`), then a Darwin range check (anything > ELAST → EIO), then
  `fs_errorForPOSIXError`. `FSError(.invalidDirectoryCookie)` for a bad resume cookie.
- **Read/write**: FSKit's buffer pointer goes straight into `ntfs_read`/`ntfs_write`; short reads at
  EOF pass through; short writes are retried for the remainder; EROFS before the core on read-only
  volumes. Truncate/extend via `ntfs_truncate` from `setAttributes(size)` (files only, per FSKit).
- **bdev bridge**: one `readInto:`/`writeFrom:` call per request (loop only after a genuinely
  partial result), block-size alignment and device-end checks (`-EINVAL` + os_log fault on a
  misaligned request), `flush` = `metadataFlushWithError:`, `ntfs_bdev_fskit_set_read_only` for
  FSKit's `--rdonly`. No TRIM (the resource has no discard primitive).
- **Read-only fallback surfaced**: `NTFSVolume.isReadOnly`/`roReason` from `ntfs_volume_get_info`;
  all mutating ops return EROFS; `Shared/MountStatus.swift` (compiled into both targets) writes
  `<app group>/Library/Application Support/mount-status.json` on activate/deactivate/unload; the
  menu-bar app polls it and shows the reason under the volume row. Settings toggles map onto
  `ntfs_mount_options.flags` (`Options.swift`); `-o ro,rw,hidehidden,showsystem,allowillegal,
  discard,casesensitive`, `--rdonly`, `-f` parsed from load/activate/mount task options.
- `probe` answers `usableButLimited` for dirty/hibernated/unclean-log/write-protected media.
- Stub still linked in `NTFS_CORE_MODE = stub`; `NTFS_CORE_MODE=real` on the xcodebuild command
  line links `build/libntfscore.a` (verified: stub excluded, 481 core symbols, no undefined
  platform symbols, arm64).
- `fskit/scripts/mount-test.sh`: pluginkit check → `hdiutil attach -nomount -shadow` → `mount -F -t
  ntfsx` → ls/df/stat/xattr (+ `--rw` create/rename/delete) → umount → detach.
- `fskit/README.md`: signing steps (Team ID, App ID capabilities, profiles), mount-test procedure.
- `tools/windows/`: `Common.ps1`, `Prepare-Drive.ps1`, `Scenario-A/B/C/D/E.ps1`, `Dump-LogFile.ps1`,
  `README.md` implementing `docs/LOGFILE.md` §7 with hard refusals for C:/non-removable disks.

## Build
`cd fskit && xcodebuild -project NTFS.xcodeproj -scheme NTFS -configuration Debug
CODE_SIGNING_ALLOWED=NO -derivedDataPath build/DerivedData build` → BUILD SUCCEEDED (stub).
Add `NTFS_CORE_MODE=real` for the real core.

## Untestable here
- Anything past linking: no signing identity on this machine, so fskitd will not load the
  extension. `mount-test.sh` is ready for the integrator once the appex is signed.
- `tools/windows/*.ps1`: no `pwsh` on this Mac (not installed on purpose); syntax was checked by
  reading and a brace/paren balance script. First run on the PC is the real test.

## API uncertainties (FSKit, macOS 26 SDK)
- There is no API for a module to *declare* a volume read-only after load; FSKit passes `--rdonly`
  only when the kernel already wants MNT_RDONLY. A dirty volume therefore mounts with the kernel
  believing it is rw; we return EROFS on every write and publish the reason. `usableButLimited`
  from probe may make the system mount read-only — unverified.
- `--rdonly` is documented for `loadResource`; whether `-o ro` also reaches `activate` (via
  `FSActivateOptionSyntax "o:"`) vs `mount` is handled by parsing both.
- `closeItem(keepingModes:)` semantics assumed: modes that remain open after this close.
- Whether a third-party module wins the probe against Apple's `ntfs.fs` (kernel, order 1000) at
  order 500 is unverified (no signing).

## Next
- Sign, install, run `fskit/scripts/mount-test.sh` (read-only first, then `--rw`) against
  `tools/images/basic-4k.img`, and watch `log stream --predicate 'subsystem == "org.ntfsmac.NTFS"'`.
- Verify the read-only fallback path with a dirty fixture, and the status file/UI.
- Consider `FSVolume.AccessCheckOperations` if the kernel's permission checks fight noowners.

## 2026-09-13: read-write mount of a volume with a live journal

Two bugs, found from one contradictory status message ("Journal: clean, version
1.1" next to "mounted read-only because $LogFile is not clean").

1. `ntfs_glue_logfile_clean()` required the log to be closed **and** carry
   RESTART_VOLUME_IS_CLEAN. logfile.h says the two are alternatives, and that
   XP and later always leave the log open, so the rule condemned every modern
   cleanly-dismounted volume to read-only. `core/logfile` had it right; the two
   now agree.
2. Fixing (1) let the mount proceed and it crashed: `ntfs_empty_logfile()` uses
   `sb->s_bdev->bd_mapping`, which only `bdev_file.c` ever created. The FSKit
   bdev had NULL. Moved to `platform/src/bdev_mapping.c` and attached from both
   constructors.

Verified on a 1 TB Windows-formatted USB disk: mounts read-write, status file
reports readOnly false with no reason, create/rename/delete round trip, no
crash. Coverage gap that hid both: every image in `tools/images` has an empty
journal, so `ntfs_empty_logfile()` always returned at its first line.

