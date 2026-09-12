# fskit/ — host app + FSKit file system extension

Xcode project for the macOS side of the driver: a menu-bar host app that
carries the FSKit extension, and the extension itself, which translates every
`FSVolume` operation onto the C ABI in `core/include/ntfscore.h`.

```
fskit/
  project.yml                  xcodegen spec (source of truth for the project)
  NTFS.xcodeproj               generated; regenerate with `xcodegen generate`
  Config/Shared.xcconfig       team ID + stub/real core switch
  NTFS/                        host app (SwiftUI, menu bar, Settings)
  NTFSExtension/               the .appex (Swift entry point + FSVolume)
  Bridge/                      ObjC: struct ntfs_bdev over FSBlockDeviceResource
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

`CODE_SIGNING_ALLOWED=NO` is needed until a team is configured (below). An
unsigned build compiles and links but **cannot be loaded by fskitd**.

## Core: stub vs real

`Config/Shared.xcconfig`:

```
NTFS_CORE_MODE = stub    # links core/stub/ntfscore_stub.c (every op -> ENOSYS)
NTFS_CORE_MODE = real    # excludes the stub, links ../build/libntfscore.a
```

That single line is the switch. In `real` mode the CMake build must have
produced `build/libntfscore.a` at the repository root first. Header search
paths already include `core/include` and `platform/include`.

## Signing and enabling the extension (what the integrator must do)

`security find-identity -v -p codesigning` on this machine reports **0 valid
identities**, so nothing here is signed yet. FSKit modules require the
managed entitlement `com.apple.developer.fskit.fsmodule`; fskitd refuses to
load an extension whose provisioning profile does not carry it.

1. In the Apple Developer portal, create App IDs `org.ntfsmac.NTFS` and
   `org.ntfsmac.NTFS.Extension` (or change `PRODUCT_BUNDLE_IDENTIFIER` in
   `project.yml`), enable the **FSKit Module** capability on the extension's
   App ID and the **App Groups** capability (`group.org.ntfsmac.NTFS`) on both.
2. Put the Team ID in `Config/Shared.xcconfig`: `NTFS_TEAM_ID = XXXXXXXXXX`
   (or pass `DEVELOPMENT_TEAM=XXXXXXXXXX` to xcodebuild). Signing is
   `Automatic`; Xcode/xcodebuild will create the profiles.
3. Build without `CODE_SIGNING_ALLOWED=NO`, copy `NTFS.app` to
   `/Applications`, launch it once so the system registers the extension.
4. System Settings → General → Login Items & Extensions → File System
   Extensions (the ⓘ button) → enable **NTFS**. The menu-bar app shows the
   state via `FSClient.installedExtensions` and deep-links there.
5. Plug in an NTFS drive: Disk Arbitration probes registered modules in
   `FSProbeOrder` order. Ours claims `Windows_NTFS` at 500 (Apple's kernel
   `ntfs.fs` uses 1000, its FSKit exfat module also probes NTFS partitions at
   2000). Whether a third-party module can win against the built-in bundle
   has not been verified on this machine (no signing identity); if it does
   not, the manual path is `mount -F -t ntfsx /dev/diskNsM /Volumes/X`
   (as root; sandboxed apps cannot mount third-party FSKit volumes on 26).

Do not ad-hoc sign or strip the entitlement to "make it load"; it will not,
and the stub core would only return ENOSYS anyway.

## Design notes

- **Short name `ntfsx`** (`FSShortName`, `mount -t ntfsx`). Apple's kernel
  driver already owns the type name `ntfs` in `/System/Library/Filesystems`;
  sharing it risks the wrong bundle being picked for `mount -t`. The volume
  reports `fileSystemTypeName = "ntfs"` in statfs so `df`/Finder show NTFS.
- **Item IDs**: NTFS root is MFT record 5 but FSKit wants `.rootDirectory`
  (2). `NTFSItem.identifier(forInode:)` maps 5 → 2 and moves MFT 1/2 (system
  files, only visible with `showsystem`) to `ino | 1<<62`.
- **Directory cookies**: 0 = `.`, 1 = `..`, `n+2` = core cookie `n`. Entries are
  packed one behind so each entry's `nextCookie` is the following entry's
  position; if the packer stops, the unpacked entry's own cookie is the resume
  point (matches `ntfs_readdir`'s contract that a rejected entry is returned
  again).
- **Block device**: `Bridge/ntfs_bdev_fskit.m` uses the resource's direct
  `readInto:`/`writeFrom:` path with a loop for partial transfers; flush is
  `metadataFlushWithError:` (the only flush the resource exposes). It does not
  use the kernel buffer cache (`metadataRead/Write`) because the core has its
  own metadata cache. Swift only sees the device as an `OpaquePointer`.
- **Options**: mount defaults come from the app group `UserDefaults`
  (`group.org.ntfsmac.NTFS`, keys in `SharedSettings.swift`), overridable per
  mount with `-o ro,rw,hidehidden,showsystem,allowillegal,discard,casesensitive`.
- **Ownership**: `doesNotSupportSettingFilePermissions` + uid/gid of the
  mounting user (PORTING.md §6 noowners). UF_HIDDEN ↔ FILE_ATTR_HIDDEN.
- **Check**: `FSManageableResourceMaintenanceOperations.startCheck` validates
  the boot sector via `ntfs_probe` and reports dirty/hibernated state; it
  cannot repair. `startFormat` returns ENOTSUP.
- **Concurrency**: Swift language mode 5, `SWIFT_STRICT_CONCURRENCY=minimal`;
  FSKit's classes are not Sendable-annotated and FSKit calls us on its own
  queues. Per-volume state is guarded with `NSLock`.
- `Bridge/ntfs_bdev_fskit.m` is compiled with `-fno-modules`: it includes
  `ntfsport/bdev.h`, whose `struct rb_node` (from `linux/types.h`) collides
  with Darwin's `<sys/rbtree.h>` when the Darwin module is imported wholesale.
  The same collision is why `bdev.h` is not in the Swift bridging header.
