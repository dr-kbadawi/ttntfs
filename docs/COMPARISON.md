# TT NTFS Native against the alternatives

Checked 2026-09-18. Our column is measured in this project. The others were
verified by two research passes against vendor documentation (quoted), the
vendors' **shipped installers** (downloaded and unpacked: Paragon 18.0.24,
Tuxera 26.2), and live tests of Apple's driver on macOS 26.6.2. `?` means no
public source settles it. This goes stale; re-check before quoting it.

## Platform

| | TT NTFS Native | Apple built-in | Paragon 18 | Tuxera 26.2 | NTFS-3G + macFUSE | iBoysoft | EaseUS |
|---|---|---|---|---|---|---|---|
| Mechanism | FSKit | UserFS plugin (Apple-private; not FSKit despite the `fskit` flag in `mount`) | **kext** | userland ntfs-3g + FUSE kext | FUSE kext; FSKit backend opt-in, experimental, slower | kext, or user-mode "Extension mode" | kext |
| Full Security on Apple Silicon | yes | yes | **no, Reduced Security** | **no, Reduced Security** | no by default | yes in Extension mode | no |
| x86_64 | **no** | yes | yes | yes (vendor table says sunset 2024; binary universal) | yes | yes | yes |
| Licence | GPL-2.0 | proprietary | proprietary | proprietary | GPL | proprietary | proprietary |
| Price | free | free | $29.95 one-time per major version | $28.95 one-time, lifetime, 5 Macs | free | $19.95/yr or $49.95 | $14.95/mo or $49.95 |
| Read / write | yes / yes | yes / **no** (write mode removed since Ventura) | yes / yes | yes / yes | yes / yes | yes / yes | yes / yes |

Neither Paragon nor Tuxera has shipped FSKit. We are the only driver where
kext-free is the default and only path with the full feature set.

## Journal and dirty volumes

| | TT NTFS Native | Apple | Paragon | Tuxera | NTFS-3G |
|---|---|---|---|---|---|
| Dirty journal, default | read-only + Close Journal button | read-only, silent | own `fsck` before mount | **auto-recovery on, mounts rw** | **clears the log, mounts rw** |
| Replays the journal | yes: documented, opt-in, verified vs Windows | no | no (not claimed) | **yes, in the binary** (`ntfs_jnl_play`), undocumented | never |
| When replay fails | refuses; nothing written | -- | -- | **resets the journal, mounts rw anyway** | -- |
| Regenerates a missing log header | yes, verified vs Windows | ? | ? | ? | no |
| Hibernated volume | refuses rw | read-only | read-only or destructive Force Mount | refuses or destructive Purge | refuses rw |

## Files and metadata

| | TT NTFS Native | Apple | Paragon | Tuxera | NTFS-3G |
|---|---|---|---|---|---|
| Symlinks we write: Windows follows them | yes, verified | -- | ? | ? | **no** ("Neither mode are interoperable with Windows") |
| Reads `mklink /J` and `/D` as links | yes, verified on real ones | **no** (plain file, tested) | ? | yes by design | yes |
| Symlinks survive `chkdsk` | yes, verified | -- | ? | ? | ? |
| Hard links | yes | yes | yes | yes | yes |
| xattrs as ADS | yes, verified | yes | ? | yes, on by default | yes |
| Compressed: read / modify existing | yes / yes, verified vs Windows | yes / -- | yes / ? | yes / yes | yes / yes |
| **Compressed: create / inherit** | **no** (finding 25) | -- | ? | experimental tool | **yes, default on** |
| EFS | no | no | no | no | raw only |
| 4Kn | yes, measured | ? | ? | mkntfs accepts | source accepts |

## Tooling and posture

| | TT NTFS Native | Apple | Paragon | Tuxera | NTFS-3G |
|---|---|---|---|---|---|
| Format / repair | **no / no** | no / no | yes / own fsck | yes / own ntfsck | mkntfs / ntfsfix (resets journal) |
| Per-volume UI | yes | no | yes | yes | no (Mounty) |
| Public defect record | 25 findings, each with a test | OS-compat notes | release notes to 2009, self-disclosed CVE | GitHub; 9 CVEs fixed Jul 2026 |
| Verified against Windows, published | yes | no | no | no |
| Windows builds tested | **1** | years | years | years |

## The honest summary

Only driver that is kext-free by default, open source, read/write, and both
reads and writes Windows-compatible symlinks, with a documented, opt-in,
Windows-verified journal replay that refuses rather than guesses. Trailing on
compressed-file creation, x86_64, format/repair, and above all on years of
exposure across Windows versions and hardware.
