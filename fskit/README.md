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
                               DiskInventory.swift lists every NTFS partition,
                               ModuleEnabler.swift enables/disables the module,
                               Uninstaller.swift removes it, LoginItem.swift starts at login
  NTFSExtension/               the .appex (Swift entry point, FSUnaryFileSystem, FSVolume)
  Shared/                      MountStatus.swift, compiled into both targets
  Bridge/                      ObjC: struct ntfs_bdev over FSBlockDeviceResource, bridging header
  scripts/mount-test.sh        end-to-end mount test once the appex is signed
  scripts/enable-module.sh     enable/disable the module (System Settings cannot)
  scripts/bench.sh             throughput + metadata benchmark of a mounted volume
```

## Build

```
cd fskit
xcodegen generate                        # only after editing project.yml
xcodebuild -project NTFS.xcodeproj -scheme NTFS -configuration Debug \
           -derivedDataPath build/DerivedData CODE_SIGNING_ALLOWED=NO build
```

Output: `build/DerivedData/Build/Products/Debug/TT NTFS Native.app` with
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

> **Build outside iCloud-synced folders.** `~/Documents` is synced by File
> Provider, which stamps bundles with `com.apple.fileprovider.fpfs#P` /
> `FinderInfo`; codesign then fails with "resource fork, Finder information,
> or similar detritus not allowed". Use `-derivedDataPath /tmp/ttntfs-dd`
> (or any path outside the synced tree) for signed builds.

## Signing and enabling the extension (integrator checklist)

`security find-identity -v -p codesigning` on the development machine reports
**0 valid identities**, so nothing here is signed yet. FSKit modules require the
managed entitlement `com.apple.developer.fskit.fsmodule`; fskitd refuses to
load an extension whose provisioning profile does not carry it.

1. **Apple Developer portal → Identifiers.** Create two App IDs (explicit, not
   wildcard): `ch.techtag.ntfs` (app) and `ch.techtag.ntfs.extension`
   (extension). Or change `PRODUCT_BUNDLE_IDENTIFIER` for both targets in
   `project.yml` and regenerate. On **both**, enable *App Groups* and add the
   group `group.ch.techtag.ntfs`. On the **extension's** App ID enable the
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
3. **Build signed.** Debug uses automatic development signing. **Release is the
   shipping configuration**: manual signing with `Developer ID Application`, the
   two Developer ID profiles from the portal (`TT NTFS Native PP` and `TT NTFS
   Native Ext PP` — Xcode's automatic signing only ever creates *development*
   profiles, so these are made by hand at developer.apple.com → Profiles →
   Developer ID), a hardened runtime, `--timestamp`, and
   `CODE_SIGN_INJECT_BASE_ENTITLEMENTS = NO` because Xcode otherwise injects
   `com.apple.security.get-task-allow`, which the notary service rejects.
   The profiles matter for two entitlements that cannot be self-asserted: the
   extension's managed `com.apple.developer.fskit.fsmodule`, and — since
   macOS 15 — the app group, without which the app and extension get separate
   containers and stop sharing settings. Both App IDs must exist explicitly
   (a wildcard App ID cannot carry an app group) with the group
   `group.ch.techtag.ntfs` assigned, and the group itself registered under
   Identifiers → App Groups. Drop `CODE_SIGNING_ALLOWED=NO`:
   ```
   xcodebuild -project NTFS.xcodeproj -scheme NTFS -configuration Debug \
              -derivedDataPath build/DerivedData -allowProvisioningUpdates build
   codesign -d --entitlements - --xml \
     build/DerivedData/Build/Products/Debug/TT NTFS Native.app/Contents/Extensions/NTFSExtension.appex \
     | grep fskit.fsmodule       # must print the key
   ```
   Do not ad-hoc sign or strip the entitlement to "make it load": it will not.
4. **Install and register.** Copy `TT NTFS Native.app` to `/Applications` and launch it
   once (LaunchServices registers the appex; `pluginkit -m -i
   ch.techtag.ntfs.extension -v` lists it). Launch it **before** step 5: the
   first launch re-registers the bundle with a new plugin UUID, pkd then kills
   every running instance of the extension (unmounting its volumes, seen
   2026-09-13 as `launchd: remove all extension instances: caller = pkd`) and
   `fskit_agent` keeps the old UUID until it is restarted (step 5 does that).
5. **Enable.** Open the app and press **Enable Extension** in the menu-bar
   panel (`ModuleEnabler.swift`), or run `fskit/scripts/enable-module.sh` for the
   same thing from a terminal. The System Settings switch
   (General → Login Items & Extensions → File System Extensions ⓘ) does **not**
   work on macOS 26.3: LoginItems.appex calls fskitd as an unentitled
   `FSClient` (it lacks `com.apple.private.LiveFS.connection`, which only the
   never-consulted FSKitModuleManagement.appex holds) and fskitd answers
   EPERM — `log show` shows `Failed to enabled FSExtension:
   NSPOSIXErrorDomain Code=1` one millisecond after every click, with no
   `fskit_agent` activity. Any third-party client gets the same EPERM, team ID
   or not. The switch that matters is `fskit_agent`'s list
   `~/Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist`,
   read only at agent start; the script appends the bundle ID, SIGKILLs the
   agent (SIGTERM is ignored, `launchctl kickstart` refused) so launchd respawns
   it, and sets the `pluginkit` election too. Verified 2026-09-13: afterwards
   `FSClient.installedExtensions` reports `enabled=1` and the mount test
   passes. Re-run the script after every reinstall (unregistering the old
   bundle drops the ID from the list). The menu-bar app shows the state via
   `FSClient.installedExtensions`.
6. **Mount.** Plug in an NTFS drive: Disk Arbitration probes registered modules
   in `FSProbeOrder` order. Ours claims `Windows_NTFS` at 500 (Apple's kernel
   `ntfs.fs` uses 1000, its FSKit exfat module also probes NTFS partitions at
   2000). Whether a third-party module wins against the built-in bundle has not
   been verified (no signing identity); the manual path always works:
   `sudo mount -F -t ttntfs /dev/diskNsM /Volumes/X` (`-o ro` for read-only).

## Mount test (after signing)

```
fskit/scripts/mount-test.sh              # read-only, tools/images/basic-4k.img
fskit/scripts/mount-test.sh --rw         # + create/mkdir/rename/read/delete on a shadow file
fskit/scripts/mount-test.sh -i tools/images/names.img -m /tmp/ntfs-test
```

Steps and what each proves: (1) `pluginkit` shows the module registered and
enabled and `codesign` confirms the entitlement; (2) `hdiutil attach -nomount
-shadow` exposes the fixture as `/dev/diskN` without letting the kernel driver
mount it, writes go to a temporary shadow file; (3) `sudo mount -F -t ttntfs`
runs probe → load → check → activate → mount; (4) `ls`, `df`, `stat`, `xattr`
and optionally a write cycle exercise enumerate/getattr/statfs/read/xattr and
create/rename/remove; the `mount-status.json` the extension wrote is printed;
(5) `umount` and `hdiutil detach` run unmount → reclaim → deactivate → unload.
Failures print the step and a `log stream` predicate for the extension's
subsystem `ch.techtag.ntfs`. Fixtures come from `make -C tools/mkfixtures
fixtures` (see tools/README.md).

## Known issues (macOS 26.3 and 26.6.2)

- The System Settings switch for File System Extensions cannot enable a
  third-party module; use `scripts/enable-module.sh`. It will keep showing the
  module as off even while it is enabled and mounting — `FSClient` and the
  menu-bar app report the true state.
- The **first** launch of a freshly installed app bundle re-registers the
  extension, which kills the running module and unmounts its volumes
  (`launchd: remove all extension instances: caller = pkd`). Later launches of
  an already-registered bundle are harmless — verified 2026-09-13 on 26.6.2
  with a volume mounted: it stayed mounted and the plugin UUID did not change.
  So the install order matters (copy, launch once, then `enable-module.sh`,
  then mount), but day-to-day launching does not.
- LaunchServices registers **every** copy of the bundle it sees, Xcode's build
  output included, and System Settings then lists the extension twice. Both the
  enabled list and the `pluginkit` election name the bundle ID rather than a
  path, so with two copies registered it is undefined which one `fskit_agent`
  launches — and if that is a build directory that later gets deleted, probes
  fail and the disk falls back to Apple's driver. `enable-module.sh` now
  unregisters any copy that is not the installed app.
- **Dragging the app to the Trash is not a clean uninstall**, which is why
  Settings has a **Remove** section (`Uninstaller.swift`). It disables the
  module, removes the login item, unregisters the bundle, deletes the
  containers and preferences, then moves itself to the Trash. Doing only the
  drag leaves the module in FSKit's enabled list and the bundle registered, so
  System Settings keeps listing an extension that no longer exists.
  `scripts/uninstall.sh` is the same thing for a machine without the app.
- **Discarding a hibernation image is the one read-only case we can fix.**
  Windows' Fast Startup saves a kernel session into `hiberfil.sys` and resumes
  from it, including its own cached picture of this filesystem, so writing
  behind it corrupts the volume. Zeroing that file's header is exactly what
  Windows does once it consumes the image, and what our detection looks for:
  Windows boots instead of resuming and the volume is safe to write. No
  inferred on-disk layout is involved, unlike journal replay, which is why this
  is done and that is not. It destroys whatever the user had open in Windows,
  so it needs consent: the app writes a one-shot request
  (`pendingHibernationDiscard`, a BSD name) into the app group, the extension
  takes it at mount and the core does the work behind
  `NTFS_MOUNT_DISCARD_HIBERNATION`, only for a volume that is otherwise clean.
  Consuming the request at mount means a repeated or failed mount cannot
  silently discard a session the user agreed to once.
  The awkward part: `super.c`'s `check_windows_hibernation_status()` sets
  `NV_Errors` precisely to stop the volume going read-write, and writing the
  file needs it read-write — so the flag is cleared for the attempt and put back
  if the discard does not complete. Verified 2026-09-13 on a volume given a real
  `hibr` header: read-only without consent, read-write with it, header zeroed,
  and a second mount with a restored header stays read-only.
- **Read-only is invisible to the kernel.** When the core refuses writes because
  a volume is dirty, hibernated or has an unclean journal, it enforces that per
  operation with EROFS — FSKit cannot flip `MNT_RDONLY` after load — so
  `getmntinfo` still reports the mount as read/write. `mount-status.json` is the
  only place that truth exists, so `DiskInventory` merges it in; trusting the
  kernel alone made the app advertise a volume as writable when every write
  would fail. Seen on a Windows recovery partition, which mounts read-only
  because its `$LogFile` is unclean.
- **The menu lists every NTFS partition, mounted or not** (`DiskInventory`),
  with Mount, Eject, and "Use This Driver" for one another driver holds.
  Partitions come from IOKit (every leaf `IOMedia`), mount state from
  `getmntinfo`, and ownership from the extension's open block device. Mount
  state has to be attached *before* filtering: a whole-disk volume — an attached
  image, or a disk formatted without a partition table — has no partition type
  at all, and is only identifiable as NTFS from the filesystem the kernel
  reports once mounted.
  Which partitions *could* hold NTFS is judged by partition type, since reading
  a raw device needs root and this app deliberately has none. So the list is of
  candidates; mounting one is how you find out, and a failure is reported rather
  than hidden. The extension therefore also claims GPT **Windows Recovery**
  (`DE94BBA4-…`) at probe order 2500 — those partitions are NTFS, nothing on
  macOS mounts them today, and without claiming the type Disk Arbitration would
  never offer them to us, leaving a Mount button that could not work.
- **One extension process per mounted volume.** Verified with two NTFS volumes
  at once (a physical disk and an attached image): three processes, a probe
  instance plus one per volume, both volumes read/write, both listed in
  `mount-status.json`. That is why `MountStatus.update` takes an advisory lock
  on a sidecar file as well as an `NSLock` — the processes are separate, so an
  in-process lock alone would let two volumes activating together lose an entry.
- A Login Item record can only be removed by the app itself: Background Task
  Management has no per-item command line. `scripts/uninstall.sh` therefore runs
  the app with `--unregister-login-item` before deleting the bundle. Delete the
  bundle by hand first and the record is orphaned under System Settings →
  General → Login Items & Extensions → Open at Login, removable only with the
  "−" button there.
- The app registers itself as a login item on its first run (`LoginItem.swift`,
  `SMAppService.mainApp`), so the menu bar comes back after a restart; Settings
  has an "Open at login" switch, and switching it off is remembered. macOS shows
  its own "added a login item" notice the first time. The driver never needs the
  app — `fskit_agent` launches the extension on demand — so this only affects
  the status UI.
- `mount -F -t ttntfs` on a *physical* disk fails with EACCES opening
  `/dev/rdiskN` even as the owning user (acknowledged by Apple DTS). Mounts
  initiated by Disk Arbitration — plugging the disk in, `diskutil mount` — work.
  Disk images attached with `hdiutil` are fine for `mount -F`, which is what
  `scripts/mount-test.sh` uses.
- Once Disk Arbitration has staged a module for a volume it keeps using it, so
  disabling the module is not enough to hand the disk to Apple's driver for a
  comparison: unplug and replug. Right after `enable-module.sh` the first mount
  may still fall back to Apple's driver; unmount and mount again.
- The extension does not yet write `mount-status.json`, so the menu-bar app
  cannot show the read-only reason. `diskutil info` reports the personality of
  our mounts as "MS-DOS".
- Closing a read-only probe handle logs harmless `flush: Input/output error`
  lines; the core should not flush a handle it opened read-only.

## Design notes

- **Lifecycle** (FSVolume.h): `activate` is called *before* `mount`, so the core
  is mounted in `activate` (that is where `-o` options arrive through
  `FSActivateOptionSyntax`); `mount` is a signal only; `unmount` syncs;
  `deactivate` runs `ntfs_unmount` after FSKit has reclaimed every item;
  `unloadResource` unmounts late if deactivate never came and closes the device.
- **Short name `ttntfs`** (`FSShortName`, `mount -t ttntfs`). Apple's kernel
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
  reason to `~/Library/Group Containers/group.ch.techtag.ntfs/Library/
  Application Support/mount-status.json`, which the menu-bar app polls every
  2 s and shows under the volume. FSKit has no API to flip MNT_RDONLY after
  load; `--rdonly` is only passed when the kernel already wants it (that also
  marks the bdev read-only). Probe answers `usableButLimited` for such media.
- **Options**: mount defaults come from the app group `UserDefaults`
  (`group.ch.techtag.ntfs`, keys in `SharedSettings.swift` ↔ `Options.swift`),
  overridable per mount with `-o ro,rw,hidehidden,showsystem,allowillegal,
  discard,casesensitive`; FSKit's `--rdonly` and `-f` are recognised too.
- **The host app is deliberately not sandboxed.** It enables the module itself,
  which means writing `enabledModules.plist` in Apple's restricted
  `group.com.apple.fskit.settings` container and SIGKILLing `fskit_agent` —
  both impossible under the sandbox, and the System Settings switch that should
  do it is broken on macOS 26 (see Known issues). The extension stays sandboxed,
  as FSKit requires. This rules out the Mac App Store, which a module carrying a
  managed FSKit entitlement cannot use anyway. Everything the app does here is
  per-user state in the user's own home, so no admin password is involved —
  which is also why enabling belongs in the app and not in an installer, where a
  root `postinstall` would write the wrong user's container.
  The app keeps `com.apple.security.application-groups`: the UserDefaults suite
  redirect follows that entitlement rather than the sandbox, so settings and
  `mount-status.json` stay in the same container the extension uses. Verified
  2026-09-13 after unsandboxing: no `~/Library/Preferences/group.ch.techtag.ntfs.plist`
  appeared and the container file kept being used. Note that from macOS 15 the
  group container is only accessible when the group ID is authorized by an
  embedded provisioning profile (or the ID is team-prefixed), so the appex and
  the app both need a real profile — a Developer ID one for distribution.
- **The remount offer must not live in the "disabled" branch of the menu.**
  Enabling flips the header to "enabled" the instant it succeeds, so anything
  shown only while disabled is created and discarded in the same moment — the
  offer existed but was unreachable. It hangs off the enabled state instead and
  is recomputed whenever the menu opens.
- **Enabling does nothing to disks that are already mounted.** Disk Arbitration
  only chooses a module when it probes, and it probes on mount, so a volume
  Apple's driver picked up before the module was enabled stays on it — which
  looks to the user as though enabling did nothing. After a successful enable
  the app offers to remount those volumes (`ModuleEnabler.remountAll`, via
  DiskArbitration rather than spawning `diskutil`). It remounts up to three
  times and checks each time whether our module actually took the device: the
  first probe after an enable *always* loses, because enabling restarts
  `fskit_agent`, the agent still holds the extension's previous identity, and
  fskitd cannot check the module in — `invalid destination port`, probe status
  `0x1003` — so Disk Arbitration falls through to Apple's `ntfs`. The second
  attempt succeeds. The same one-retry pattern shows up after every agent
  restart, which is why `enable-module.sh` and the mount tests loop too.
- **The PlugInKit election is not what enables the module.** `pluginkit -e use`
  is cosmetic here: with the election set to `ignore`, FSKit still reported the
  module enabled and Disk Arbitration still mounted with it (verified
  2026-09-13). Only the `enabledModules.plist` entry plus an `fskit_agent`
  restart matter, which is why `ModuleEnabler` needs no helper tools.
- **Probe result is always `.usable`.** Disk Arbitration's FSKit bridge
  (`DASupport.m`, `DAProbeWithFSKit`) treats only `usable` as success;
  `notRecognized` is ENOENT and anything else, `usableButLimited` included, is
  EIO, logged as `unable to probe /dev/diskNsM (status code 0x00000005)`, and
  the disk then goes to Apple's read-only `ntfs` driver. The probe handle DA
  hands over is opened read-only, so `isWritable` must not feed the result.
  Verified 2026-09-13 on a 1 TB USB disk: with `usableButLimited` DA fell back
  to Apple's driver every time although our probe had succeeded.
- **Performance (measured 2026-09-13).** On a 1 TB USB SSD behind a JMicron
  bridge, same disk and files, `F_NOCACHE` reads: ours 59.2 MB/s sequential and
  141 IOPS / 7.1 ms random 4 KiB, versus Apple's built-in `ntfs` at 48.4 MB/s
  and 108 IOPS / 9.23 ms. We are faster on both. The ceiling is the enclosure:
  the link is USB 3.0 SuperSpeed but the bridge speaks Bulk-Only Transport
  (`bInterfaceProtocol=80`, not UAS), so one command is in flight at a time;
  `iostat` shows our 1 MiB file reads map to exactly one 1 MiB device transfer
  at ~17.5 ms, a fixed ~7 ms per command plus ~97 MB/s streaming. Do not tune
  against this enclosure; use UAS or internal media. Still worth fixing: a
  4 KiB random read fetches 16 KiB from the device, and we keep only one device
  request outstanding with no readahead. Benchmark with `scripts/bench.sh`.
- **DA probing runs our module twice** (a limited probe, then a full one); each
  `ntfs_probe` currently mounts the volume to read the label, about 2 s per
  call on a 1 TB disk. DA showed no timeout, but a boot-sector-only probe
  would be cheaper.
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
