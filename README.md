# TT NTFS Native

A free, open-source, kext-less NTFS read/write driver for macOS: the Linux 7.1
`ntfs` driver (ntfsplus) ported to a user-space FSKit file system extension.

No kernel extension. No Reduced Security. No reboot. GPL-2.0.

## Status (2026-09-14)

Phases 0 to 3 are done. Real NTFS disks mount read/write in Finder on macOS 26.3
and 26.6.2, from a notarized Developer ID DMG whose app enables its own
extension.

**Treat writes as unproven for data you cannot replace.** `chkdsk` has never
inspected anything this driver wrote. That is the one gate that matters and it
needs a Windows PC; see `docs/TESTING.md`.

Journal replay is off unless you ask for it per volume, at your own risk: the
v2.0 record layout is inferred rather than verified. A volume Windows left dirty
or hibernated mounts read-only with the reason shown, and the app offers to fix
either with the cost spelled out.

Performance, against Apple's own FSKit exFAT module on matched images: ahead on
streaming writes, level on overwrite and append, within the "2x of exFAT" bar on
every metadata operation. Sequential reads on a USB SSD are within about 11% of
Apple's built-in driver, close to the enclosure's ceiling.

Not yet: x86_64 (arm64 only), format, repair, a Homebrew cask.

## Using it

Install from the DMG, open the app, and press **Enable Extension**. The System
Settings switch for File System Extensions does not work for any third-party
FSKit module on macOS 26 -- `fskitd` refuses the call from every caller lacking
a private Apple entitlement -- which is why the app is unsandboxed and enables
itself. `fskit/README.md` has the detail, the evidence, and the known issues.

## Building

```
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

The Xcode project is generated: `cd fskit && xcodegen`. Release builds are
Developer ID signed and notarized by `fskit/scripts/package.sh`; that needs the
certificate and a notarization credential.

## Checking

```
tools/ci.sh              # everything checkable without hardware or signing
tools/ci.sh --quick      # skip the slow fixture suite
tools/install-hooks.sh   # run --quick before every push
```

Four suites: **15 `ctest` targets (~2,700 checks)**, 265 fixture checks against
ntfsprogs as ground truth plus a real structural `fsck`, 51 Swift unit tests,
and a manual end-to-end mount test. Everything runs twice, instrumented with
UBSan and not. **`docs/TESTING.md` says what each covers and, more usefully,
what is still not covered.**

## Layout

| path | what |
|---|---|
| `upstream/` | pristine kernel source being ported; never edited |
| `core/ntfs/` | the ported driver (Tier 0/1), kept close to upstream |
| `core/vfs/` | the Linux VFS glue replaced with our own, behind one C ABI |
| `core/logfile/` | the `$LogFile` parser, analyser and replay engine |
| `platform/` | user-space implementation of the kernel API the core uses |
| `fskit/` | Xcode project: menu-bar app + FSKit extension + C bridge |
| `tools/` | `ntfscli` harness, fixture generator, test runner, CI |

## Documents

| file | what |
|---|---|
| `docs/PORTING.md` | why this source, the dependency survey, phases, decisions |
| `docs/TESTING.md` | what is checked, what is not, how to add a test |
| `docs/LOGFILE.md` | `$LogFile` format, replay algorithm, verified vs inferred |
| `docs/CONTRACTS.md` | interface ownership between work streams |
| `docs/progress/` | per-area findings, measurements, and open questions |
| `COPYRIGHT` | licensing, and why two SPDX identifiers are in use |

## Licence

GPL-2.0. The core derives from the Linux kernel's `fs/ntfs`, so the licence is
inherited rather than chosen. Full text in `LICENSE`, details in `COPYRIGHT`.
