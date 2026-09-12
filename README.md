# NTFS for macOS (working title)

A free, open-source, kext-less NTFS read/write driver for macOS: the Linux 7.1
`ntfs` driver (ntfsplus) ported to a user-space FSKit file system extension.

No kernel extension. No Reduced Security. No reboot. GPL-2.0.

- `docs/PORTING.md` — design, dependency survey, phases
- `upstream/` — pristine kernel source being ported (never edited)
- `core/` — the port
- `platform/` — user-space implementation of the kernel API the core uses
- `fskit/` — Xcode project: host app + FSKit extension
- `tools/` — `ntfscli` test harness, image fixtures
