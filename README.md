# NTFS for macOS (working title)

A free, open-source, kext-less NTFS read/write driver for macOS: the Linux 7.1
`ntfs` driver (ntfsplus) ported to a user-space FSKit file system extension.

No kernel extension. No Reduced Security. No reboot. GPL-2.0.

**Status (2026-09-13):** mounts real NTFS disks read/write on macOS 26.3 through
Disk Arbitration, with create/read/rename/delete verified on a 1 TB USB disk and
the volume remounting clean afterwards. On that disk it is faster than Apple's
built-in read-only `ntfs` driver (59.2 vs 48.4 MB/s sequential). Not yet
verified against `chkdsk` on Windows — treat writes as unproven for data you
cannot replace. Journal replay is not enabled; dirty, hibernated or
unclean-`$LogFile` volumes mount read-only. Setup, caveats and benchmarks are in
`fskit/README.md`.

- `docs/PORTING.md` — design, dependency survey, phases
- `upstream/` — pristine kernel source being ported (never edited)
- `core/` — the port
- `platform/` — user-space implementation of the kernel API the core uses
- `fskit/` — Xcode project: host app + FSKit extension
- `tools/` — `ntfscli` test harness, image fixtures
