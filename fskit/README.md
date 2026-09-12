# fskit/ — host app + FSKit file system extension

Xcode project for the macOS side of the driver: a menu-bar host app that
carries the FSKit extension, and the extension itself, which translates every
`FSVolume` operation onto the C ABI in `core/include/ntfscore.h`.

```
fskit/
  project.yml                  xcodegen spec (source of truth for the project)
  NTFS.xcodeproj               generated; regenerate with `xcodegen generate`
  Config/Shared.xcconfig       team ID + stub/real core switch
  NTFS/                        host app (SwiftUI, menu bar, Settings, volume list)
  NTFSExtension/               the .appex (Swift entry point, FSUnaryFileSystem, FSVolume)
  Shared/                      MountStatus.swift, compiled into both targets
  Bridge/                      ObjC: struct ntfs_bdev over FSBlockDeviceResource, bridging header
  scripts/mount-test.sh        end-to-end mount test once the appex is signed
```

## Build

```
cd fskit
xcodegen generate                        # only after editing project.yml
xcodebuild -project NTFS.xcodeproj -scheme NTFS -configuration Debug \
           -derivedDataPath build/DerivedData CODE_SIGNING_ALLOWED=NO build
```

Output: `build/DerivedData/Build/Products/Debug/NTFS.app` with
`Contents/Extensions/NTFSExtension.appex` inside. Requires Xcode 26 / macOS 26 SDK
(FSKit API as shipped in macOS 26; deployment target 26.0 per docs/PORTING.md §6).
Debug builds put the code in `NTFSExtension.debug.dylib` next to the tiny
launcher binary (Xcode 26 layout); look there with `nm`.

`CODE_SIGNING_ALLOWED=NO` is needed until a team is configured (below). An
unsigned build compiles and links but **cannot be loaded by fskitd**.

## Core: stub vs real

`Config/Shared.xcconfig`:

```
NTFS_CORE_MODE = stub    # links core/stub/ntfscore_stub.c (every op -> ENOSYS)
NTFS_CORE_MODE = real    # excludes the stub, links ../build/libntfscore.a
```

That single line is the switch (or pass `NTFS_CORE_MODE=real` on the
`xcodebuild` command line without editing anything). In `real` mode the CMake
build must have produced `build/libntfscore.a` at the repository root first
(`cmake -S . -B build && cmake --build build`, arm64). Header search paths
already include `core/include` and `platform/include`. Verified 2026-09-12: the
real-mode link excludes the stub and resolves every `ntfs_*` and platform
symbol from the archive.

## Signing and enabling the extension (integrator checklist)

`security find-identity -v -p codesigning` on the development machine reports
**0 valid identities**, so nothing here is signed yet. FSKit modules require the
managed entitlement `com.apple.developer.fskit.fsmodule`; fskitd refuses to
load an extension whose provisioning profile does not carry it.

1. **Apple Developer portal → Identifiers.** Create two App IDs (explicit, not
   wildcard): `org.ntfsmac.NTFS` (app) and `org.ntfsmac.NTFS.Extension`
   (extension). Or change `PRODUCT_BUNDLE_IDENTIFIER` for both targets in
   `project.yml` and regenerate. On **both**, enable *App Groups* and add the
   group `group.org.ntfsmac.NTFS`. On the **extension's** App ID enable the
   capability **FSKit Module** (it appears under "Additional Capabilities" for
   paid accounts on macOS 15.4+ SDKs; it is a managed capability, no request
   form).
2. **Team ID.** Put it in `Config/Shared.xcconfig`:
   ```
   NTFS_TEAM_ID = XXXXXXXXXX
   ```
   (or pass `DEVELOPMENT_TEAM=XXXXXXXXXX` to xcodebuild). Both targets use
   `CODE_SIGN_STYLE = Automatic`, so Xcode creates the two development
   provisioning profiles (they must include the FSKit Module entitlement; if a
   stale profile is cached, delete `~/Library/MobileDevice/Provisioning Profiles/*`
   and let it regenerate). The entitlements files already contain
   `com.apple.developer.fskit.fsmodule` (extension) and the app group (both).
3. **Build signed.** Drop `CODE_SIGNING_ALLOWED=NO`:
   ```
   xcodebuild -project NTFS.xcodeproj -scheme NTFS -configuration Debug \
              -derivedDataPath build/DerivedData -allowProvisioningUpdates build
   codesign -d --entitlements - --xml \
     build/DerivedData/Build/Products/Debug/NTFS.app/Contents/Extensions/NTFSExtension.appex \
     | grep fskit.fsmodule       # must print the key
   ```
   Do not ad-hoc sign or strip the entitlement to "make it load": it will not.
4. **Install and register.** Copy `NTFS.app` to `/Applications` and launch it
   once (LaunchServices registers the appex; `pluginkit -m -i
   org.ntfsmac.NTFS.Extension -v` lists it).
5. **Enable.** System Settings → General → Login Items & Extensions → File
   System Extensions (the ⓘ button) → enable **NTFS**. `pluginkit -e use -i
   org.ntfsmac.NTFS.Extension` may do the same from a terminal. The menu-bar
   app shows the state via `FSClient.installedExtensions` and deep-links there.
6. **Mount.** Plug in an NTFS drive: Disk Arbitration probes registered modules
   in `FSProbeOrder` order. Ours claims `Windows_NTFS` at 500 (Apple's kernel
   `ntfs.fs` uses 1000, its FSKit exfat module also probes NTFS partitions at
   2000). Whether a third-party module wins against the built-in bundle has not
   been verified (no signing identity); the manual path always works:
   `sudo mount -F -t ntfsx /dev/diskNsM /Volumes/X` (`-o ro` for read-only).

## Mount test (after signing)

```
fskit/scripts/mount-test.sh              # read-only, tools/images/basic-4k.img
fskit/scripts/mount-test.sh --rw         # + create/mkdir/rename/read/delete on a shadow file
fskit/scripts/mount-test.sh -i tools/images/names.img -m /tmp/ntfs-test
```

Steps and what each proves: (1) `pluginkit` shows the module registered and
enabled and `codesign` confirms the entitlement; (2) `hdiutil attach -nomount
-shadow` exposes the fixture as `/dev/diskN` without letting the kernel driver
mount it, writes go to a temporary shadow file; (3) `sudo mount -F -t ntfsx`
runs probe → load → check → activate → mount; (4) `ls`, `df`, `stat`, `xattr`
and optionally a write cycle exercise enumerate/getattr/statfs/read/xattr and
create/rename/remove; the `mount-status.json` the extension wrote is printed;
(5) `umount` and `hdiutil detach` run unmount → reclaim → deactivate → unload.
Failures print the step and a `log stream` predicate for the extension's
subsystem `org.ntfsmac.NTFS`. Fixtures come from `make -C tools/mkfixtures
fixtures` (see tools/README.md).

## Design notes

- **Lifecycle** (FSVolume.h): `activate` is called *before* `mount`, so the core
  is mounted in `activate` (that is where `-o` options arrive through
  `FSActivateOptionSyntax`); `mount` is a signal only; `unmount` syncs;
  `deactivate` runs `ntfs_unmount` after FSKit has reclaimed every item;
  `unloadResource` unmounts late if deactivate never came and closes the device.
- **Short name `ntfsx`** (`FSShortName`, `mount -t ntfsx`). Apple's kernel
  driver already owns the type name `ntfs` in `/System/Library/Filesystems`;
  sharing it risks the wrong bundle being picked for `mount -t`. The volume
  reports `fileSystemTypeName = "ntfs"` in statfs so `df`/Finder show NTFS.
- **Item IDs**: NTFS root is MFT record 5 but FSKit wants `.rootDirectory`
  (2). `NTFSItem.identifier(forInode:)` maps 5 → 2 and moves MFT 0/1/2 (system
  files, only visible with `showsystem`) to `ino | 1<<62`. One `NTFSItem` per
  inode number (hard links share it); FSKit reclaims each object once.
- **Directory cookies** are the core's own: the ported kernel readdir emits
  `.` (cookie 0) and `..` (1) itself and real entries from 2. When FSKit asks
  for attributes the dot entries are not packed (FSKit rule). Entries are
  packed one behind so each entry's `nextCookie` is the following entry's
  position; a rejected entry is left unconsumed by the core and returned again.
  The verifier is a per-directory version counter (nonzero).
- **Errors**: `platform_errno_to_host()` from `platform/include/linux/errno.h`
  is included through the bridging header; `Errors.swift` funnels every core
  return through it and collapses anything Darwin does not know to EIO.
  Xattr misses become ENOATTR.
- **xattr flags**: the core expects Linux `XATTR_CREATE=1 / XATTR_REPLACE=2`
  (`NTFS_XATTR_*_FLAG` in the bridging header), never Darwin's 2/4.
- **Block device**: `Bridge/ntfs_bdev_fskit.m` issues one `readInto:`/
  `writeFrom:` per request, enforces block-size alignment (the core promises
  it), and flushes with `metadataFlushWithError:` (the only flush the resource
  exposes; it is also what covers the device write cache). It does not use the
  kernel buffer cache (`metadataRead/Write`) because the core has its own
  metadata cache. Swift only sees the device as an `OpaquePointer`. No TRIM:
  the resource has no discard primitive on macOS 26.
- **Read-only fallback**: the core is always asked with
  `NTFS_MOUNT_RDONLY_FALLBACK`; when `ntfs_volume_info.read_only` comes back
  (dirty, hibernated, unclean `$LogFile`, write-protected device, requested),
  the volume refuses every mutating operation with EROFS and publishes the
  reason to `~/Library/Group Containers/group.org.ntfsmac.NTFS/Library/
  Application Support/mount-status.json`, which the menu-bar app polls every
  2 s and shows under the volume. FSKit has no API to flip MNT_RDONLY after
  load; `--rdonly` is only passed when the kernel already wants it (that also
  marks the bdev read-only). Probe answers `usableButLimited` for such media.
- **Options**: mount defaults come from the app group `UserDefaults`
  (`group.org.ntfsmac.NTFS`, keys in `SharedSettings.swift` ↔ `Options.swift`),
  overridable per mount with `-o ro,rw,hidehidden,showsystem,allowillegal,
  discard,casesensitive`; FSKit's `--rdonly` and `-f` are recognised too.
- **Ownership**: `doesNotSupportSettingFilePermissions` + uid/gid of the
  mounting user (PORTING.md §6 noowners); chown/chmod are accepted or ignored,
  never failed. UF_HIDDEN ↔ FILE_ATTR_HIDDEN; `doesNotSupportImmutableFiles`.
- **Check**: `FSManageableResourceMaintenanceOperations.startCheck` validates
  the boot sector via `ntfs_probe` and reports dirty/hibernated/journal state;
  it cannot repair. `startFormat` returns ENOTSUP.
- **Concurrency**: Swift language mode 5, `SWIFT_STRICT_CONCURRENCY=minimal`;
  FSKit's classes are not Sendable-annotated and FSKit calls us on its own
  queues. Per-volume state is guarded with `NSLock`.
- `Bridge/ntfs_bdev_fskit.m` is compiled with `-fno-modules`: it includes
  `ntfsport/bdev.h`, whose `struct rb_node` (from `linux/types.h`) collides
  with Darwin's `<sys/rbtree.h>` when the Darwin module is imported wholesale.
  The same collision is why `bdev.h` is not in the Swift bridging header.
