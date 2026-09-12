# tools/ — ntfsprogs, fixtures, `ntfscli`, differential tests

Everything here is host-side test infrastructure for the NTFS core
(`core/include/ntfscore.h`). Nothing in `tools/` is shipped. The reference
implementation throughout is **libntfs-3g / ntfsprogs 2026.7.7**, built from
source into `tools/.local/`; the second, independent reference is a Linux
7.1 kernel mount (see the last section).

```
tools/
  env.sh               shared paths; `. tools/env.sh` puts .local/{bin,sbin} on PATH
  build-ntfsprogs.sh   builds ntfs-3g 2026.7.7 (no FUSE) into tools/.local  [~25 s]
  common/              json.{c,h} (writer/parser), sha256.{c,h}: shared by the two C tools
  mkfixtures/          generates tools/images/*.img + *.manifest.json with libntfs-3g
  ntfscli/             CLI harness over ntfscore.h (probe/ls/cat/cp-in/mkdir/mv/rm/xattr/verify/bench)
  ntfscli-shim.sh      fake ntfscli answered by ntfs-3g; self-test of run-tests.sh
  run-tests.sh         the differential test runner (verify / ground-truth / write)
  .local/              build products of build-ntfsprogs.sh          (ignored, never committed)
  images/              generated fixtures                             (ignored, never committed)
  build/               CMake trees and test logs                      (ignored)
```

## Quick start

```sh
tools/build-ntfsprogs.sh                # once: ntfsprogs + libntfs-3g into tools/.local
make -C tools/mkfixtures fixtures       # once: 7 images + manifests into tools/images (~7 s)
tools/run-tests.sh all                  # build ntfscli against build/libntfscore.a, run everything
```

`run-tests.sh` does the first two steps itself when their outputs are
missing, so `tools/run-tests.sh all` from a clean checkout is enough.

Requirements: Xcode command-line tools (clang, make, libtool, lipo), python3
(reads manifests; ships with the CLT), a cmake. macOS has no cmake and the
Homebrew one on this machine is an Intel binary, so `env.sh`'s `ensure_cmake`
pip-installs the official arm64 cmake+ninja wheels into `tools/.local/pip`
when none is on `PATH` (`NTFS_CMAKE=/path/to/cmake` overrides). No Homebrew
package is needed for anything in this directory.

## build-ntfsprogs.sh

```sh
tools/build-ntfsprogs.sh [--clean] [--version 2026.7.7]     # env: NTFS3G_VERSION, NTFS3G_JOBS
```

Downloads Tuxera's release tarball (sha256 pinned), configures with
`--disable-ntfs-3g` (no FUSE, no kernel bits) for the host architecture and
installs into `tools/.local`: `bin/{ntfsls,ntfscat,ntfsinfo,ntfscp,ntfsfix,
ntfsck,ntfscluster,ntfscmp,ntfstruncate,...}`, `sbin/{mkntfs,ntfsclone,
ntfslabel,ntfsresize,...}`, `lib/libntfs-3g.a`, `include/ntfs-3g/`.
Homebrew's ntfs-3g is not used (2017, x86_64). Notes: `ntfsck` in 2026.7.7 is
a quarantined stub (`Unsupported: check_volume()`, exit 1) and is reported as
SKIP by the runner; `ntfsfix -n` is the consistency check that counts.

## mkfixtures and the fixture catalogue

```sh
make -C tools/mkfixtures                 # builds ./mkfixtures against tools/.local/lib/libntfs-3g.a
make -C tools/mkfixtures fixtures        # = ./mkfixtures -o tools/images --mkntfs tools/.local/sbin/mkntfs
tools/mkfixtures/mkfixtures --help       # [-o OUTDIR] [--mkntfs PATH] [--seed N] [--force] [-v] [NAME...]
```

Each fixture is `mkntfs -F -Q -c <cluster> -L <label>` followed by a populate
function using the libntfs-3g inode API; then the image is unmounted,
re-mounted and walked again, and the manifest records what **libntfs-3g reads
back** (path, type, MFT number, size, sha256, nlink, compressed/sparse flags,
raw `file_attributes`, mtime/crtime at 100 ns, named streams with size and
sha256). Content is deterministic (xorshift64* seeded from the path and stream
name), the volume serial is set explicitly, so images are reproducible modulo
timestamps and allocation. Existing images are skipped unless `--force`.

| Image | Size / cluster | Entries | What it exercises |
|---|---|---|---|
| `basic-4k` | 256 MiB / 4 KiB | 234 (214 files) | `/files/sz/<size>/`: ~200 files of the interesting sizes (0, 1, 511, 512, 4095, 4096, 64 KiB, 1 MiB, 20 MiB), 1/3 text 2/3 random, spread over sub-directories so indexes have more than one block; `/nested/a/b/c/d`; hard links (one inode, three names, two directories); `/ads/with-streams.bin` with resident, 32-byte `com.apple.FinderInfo` and 300 KB streams; `/ads/only-stream.bin` (empty unnamed stream); `/times/` with 2001, 100 ns-precise 2009, pre-1970 and post-2038 timestamps, a directory with explicit times; `/empty-dir` |
| `cluster-512` | 128 MiB / 512 B | 231 | the same population without the 20 MiB files: smallest cluster, many runs per file |
| `cluster-64k` | 256 MiB / 64 KiB | 231 | the same at the largest cluster (index blocks smaller than a cluster; no compression possible) |
| `names` | 128 MiB / 4 KiB | 5058 (5033 files) | `/unicode/`: case-variants (`UPPER.TXT`/`upper.txt`/`Upper.Txt`), CJK, emoji (surrogate pairs), ZWJ family, NFC vs NFD `café`, combining stacks, leading space, `$`, quotes, dotfiles, C1 control; a Unicode directory; `/long/`: 255-UTF-16-unit names in ASCII and CJK (765 UTF-8 bytes) and a 255-unit directory; `/big/`: 5,000 entries in one directory inserted out of order (multi-level `$INDEX_ALLOCATION` B+ tree), resident and non-resident, plus 20 sub-directories |
| `compressed-sparse` | 128 MiB / 4 KiB | 23 | `/compressed/`: text, zeros, incompressible random, resident, `65537` bytes (one compression unit + 1), odd sizes, empty, a mixed file alternating compressible/incompressible 64 KiB blocks, a compressed file with a compressed ADS, a compressed directory whose children inherit the flag; `/sparse/`: data/hole/data with an unaligned write, hole-only (initialized_size 0), leading hole, sparse+compressed, a 1 GiB logical file with 8 KiB of data |
| `attrlist` | 64 MiB / 4 KiB | 11 | every file has an `$ATTRIBUTE_LIST`: `many-ads.bin` (40 resident streams overflow the 1 KiB base record), `fragmented.bin` (600 one-cluster extents, `$DATA` split over three MFT records; `interleaver.bin` holds the interleaved clusters), `kitchen-sink.bin` (300 KB + 12 streams + 6 hard links, each name another `$FILE_NAME` in the base record) |
| `empty` | 32 MiB / 4 KiB | 0 | freshly formatted; mount/unmount, `verify` with an empty manifest, the write tests on a blank volume |

Confirming the `attrlist` case (MFT numbers from the manifest):

```sh
. tools/env.sh
ntfsinfo -f -i 72 tools/images/attrlist.img | grep -E 'ATTRIBUTE_LIST|\$DATA'
#   Dumping attribute $ATTRIBUTE_LIST (0x20) from mft record 72 (0x48)
#   Dumping attribute $DATA (0x80) from mft record 72 (0x48)
#   Dumping attribute $DATA (0x80) from mft record 77 (0x4d)
#   Dumping attribute $DATA (0x80) from mft record 79 (0x4f)
```

How the fragmentation is forced: libntfs-3g's allocator takes an exact LCN
only as the hint of an *append*; anything else goes to the start of the
largest free range. mkfixtures reserves a contiguous region with one file,
fills the rest of the volume with another, deletes the reservation and then
appends one cluster at a time alternately to two files, so each append finds
its hint taken and lands on the next free cluster. The generator dies if the
result has fewer than 300 runs or no `$ATTRIBUTE_LIST`.

libntfs-3g quirks the generator works around are listed at the top of
`mkfixtures.c` and in `docs/progress/tools.md`.

## ntfscli

```
ntfscli [--ro] [--case-sensitive] [--allow-illegal] [--show-system] [--hide-hidden]
        [--no-fallback] [--discard] [-v|-vv|-q] COMMAND IMAGE [ARGS]

probe                        ntfs_probe(): is it NTFS? label/serial/sizes (no mount)
info                         mount + ntfs_volume_get_info()
ls [-R] [-l] [-a] [PATH]     list (default /); -l long, -a system files
stat PATH                    every ntfs_attr field + stream names (+ symlink target)
cat PATH                     unnamed stream to stdout
cp-out SRC DST               copy out (DST '-' = stdout)
cp-in HOST_SRC PATH          copy in (creates or truncates), fsync
mkdir | rmdir | rm PATH      mv OLD NEW      ln [-s] EXISTING|TARGET NEW
truncate PATH SIZE
xattr list|get|set|rm PATH [NAME] [VALUE|@FILE]    xattrs = named $DATA streams
sync                         ntfs_volume_sync()
verify MANIFEST [--no-data] [--no-times]
                             walk the volume against a mkfixtures manifest
bench [--size MiB] [--files N] [--dir PATH]
```

Exit status: 0 ok, 1 an operation failed (errno name printed), 2 usage,
3 `verify` found mismatches. Long listing: `<type c s a e> <nlink> <size>
<mtime ISO-8601, 100 ns> <name>[/]` (`c`ompressed, `s`parse, `a`ds,
`e`ncrypted).

Build (the `IMAGE` argument comes right after the command):

```sh
. tools/env.sh && ensure_cmake
# against the real core (top-level build first: cmake -S . -B build && cmake --build build)
cmake -S tools/ntfscli -B tools/build/ntfscli && cmake --build tools/build/ntfscli
# against core/stub (every operation returns ENOSYS; links without core/vfs)
cmake -S tools/ntfscli -B tools/build/ntfscli-stub -DNTFSCLI_USE_STUB=ON && cmake --build tools/build/ntfscli-stub
tools/build/ntfscli-stub/ntfscli probe tools/images/empty.img     # -> ENXIO, exit 1
```

Core selection in `ntfscli/CMakeLists.txt`: a `ntfscore` target (when added
from the top-level project) > `-DNTFSCORE_LIB=<path>` (default
`build/libntfscore.a`; `libntfsplatform.a` next to it is linked after the
core, then pthread) > `core/stub` (with `-DNTFSCLI_USE_STUB=ON`, or as a
fallback with a warning when the archive is missing). `-DNTFSCLI_SANITIZE=ON`
adds ASan+UBSan. The stub build compiles `bdev_compat.c` (a pread/pwrite
`ntfsport/bdev.h`); the real build gets `platform/src/bdev_file.c` from the
archive.

Architecture: if the archive was built by an x86_64 cmake (Homebrew's
`/usr/local/bin/cmake` runs under Rosetta and so does the `cc` it spawns),
`ld` silently ignores every member and all symbols come out undefined. The
CMake file checks `lipo -archs` and builds ntfscli for the archive's
architecture with a warning; the right fix is to configure the top-level tree
with a native cmake (`ensure_cmake` from `env.sh`).

## run-tests.sh

```
tools/run-tests.sh [options] MODE...        modes: build fixtures verify ground-truth write all
  --image NAME        only this fixture (repeatable)
  --core-lib PATH     libntfscore.a to link (default build/libntfscore.a, else a native
                      build of the top-level tree into tools/build/core)
  --stub              link ntfscli against core/stub (exercises the runner; everything fails)
  --ntfscli PATH      use this ntfscli binary (tools/ntfscli-shim.sh = ntfs-3g answering)
  --sanitize          ASan+UBSan ntfscli (and core, when it builds the core)
  --no-data --no-times    passed to `ntfscli verify`
  --quick / --max-files N ground-truth: hash at most 200 / N files per image, evenly spaced
  --keep              keep the scratch directory of write mode
  -v                  echo every command's output
```

Every check prints `PASS`/`FAIL`/`SKIP` with a one-line reason, a summary
ends the run, the exit status is 1 if anything failed (2 on setup errors),
and every command's full output is in `tools/build/test-logs/`. `LC_ALL` is
forced to a UTF-8 locale so ntfsprogs print names byte-exactly.

* **verify** — `ntfscli --ro verify IMG MANIFEST` per fixture: every manifest
  entry is looked up, `getattr` compared (type, size, nlink, compressed,
  sparse, MFT number, mtime, crtime), the unnamed stream and every named
  stream hashed, extra entries on the volume reported.
* **ground-truth** — per fixture, three checks that do not use the manifest
  for anything but stream names: (1) `ntfsls -R -l -F` and `ntfscli ls -R -l`
  are normalised to sorted `path<TAB>size` / `path/` lines and diffed;
  (2) every file (or an evenly spaced sample) is read with `ntfscat` and
  `ntfscli cat` and compared with `cmp`, exit statuses included; (3) every
  named stream in the manifest is read with `ntfscat -n` and `ntfscli xattr
  get`. Each read is a separate mount on both sides, so the full run over all
  seven images (5,500 files) takes about 2 minutes with ntfs-3g answering for
  both; `--quick` samples 200 files per image (30 s).
* **write** — per fixture, on an APFS-cloned scratch copy: `mkdir /rt-write`,
  `cp-in` a 3 MiB random file and a small one, overwrite, `mkdir` a
  sub-directory, `mv` into it, `mv` over an existing file, `truncate` shrink
  and grow, `xattr set` from a string and from a file, `xattr list`, `xattr
  rm`, `rm`, `rmdir`, `sync`. After each mutation the result is checked with
  ntfs-3g (`ntfsls`, `ntfscat`, `ntfscat -n`, `ntfsinfo -F`). Then `ntfsfix
  -n` must be clean, `ntfsck` runs (SKIP while it is a stub), the `ntfsls -R`
  listing must equal the pristine image's, and `ntfscli verify` against the
  original manifest must still pass.

Self-test of the runner (no core involved): `tools/run-tests.sh --ntfscli
tools/ntfscli-shim.sh ground-truth` must pass — the shim answers `ls`/`cat`/
`xattr get` with ntfsprogs, so this proves the normalisers, sampling and
hashing are sound. `tools/run-tests.sh --stub all` must fail every check
with `ENOSYS` and exit 1.

## A Linux 7.1 kernel mount as the second ground truth (documentation only)

ntfs-3g shares ancestry with the ported driver (both descend from
Altaparmakov's `fs/ntfs`), so agreement with it is necessary but not
sufficient. The independent reference is the in-tree Linux 7.1 `fs/ntfs`
driver itself, mounted on the same images. The cheapest way to get one on
Apple Silicon, without a GUI:

1. **Lima** (Virtualization.framework, arm64, shares `$HOME` with the guest):
   install the arm64 release tarball from github.com/lima-vm/lima (a native
   `brew install lima` needs an arm64 Homebrew; the one on this machine is
   Intel), then `limactl start --name ntfs --cpus 8 --memory 8
   template://ubuntu-lts`. The repository is visible in the guest at the same
   path, read-only by default; add a writable mount for `tools/images` in
   `~/.lima/ntfs/lima.yaml` (`mounts: - location: ~/Documents/NTFS/tools/images
   writable: true`) if the write tests are to run in the guest.
2. **Kernel**: distribution kernels lag; build 7.1 in the guest:
   `sudo apt install build-essential flex bison libssl-dev libelf-dev bc
   dwarves`, fetch `linux-7.1.tar.xz` from kernel.org, `make defconfig`,
   enable `CONFIG_NTFS_FS=y` and its write option in `menuconfig` (Filesystems
   → DOS/FAT/EXFAT/NT Filesystems → NTFS), `make -j8` (~15 min on an
   8-core M-series), `sudo make modules_install install`, reboot the VM
   (`limactl restart ntfs`) and confirm `uname -r` says 7.1. Alternatively
   `virtme-ng` (`pipx install virtme-ng; vng --build; vng --run`) boots the
   freshly built kernel with the guest's own root filesystem in QEMU
   without installing it — slower (nested virtualization is only available
   on M3+ / macOS 15+; otherwise TCG), but zero install steps.
3. **Mount and dump**:
   ```sh
   grep ntfs /proc/filesystems                                        # the 7.1 in-tree driver's name
   sudo mount -t ntfs -o loop,ro tools/images/basic-4k.img /mnt
   (cd /mnt && find . -type f -exec sha256sum {} + | sort -k2) > /tmp/basic-4k.kernel.sha
   ```
   and compare against the manifest (`path`, `sha256`) or against
   `ntfscli cp-out`. Named streams are not reachable through POSIX xattrs
   in the legacy-lineage driver, so ADS comparisons stay with ntfs-3g.
   For write-path ground truth, copy a scratch image into the guest, apply
   the same `run-tests.sh write` sequence through the kernel (`cp`, `mv`,
   `truncate`, `rm`), then `ntfscli verify` and `ntfsfix -n` it on the Mac.
4. The Windows side (`format`, `chkdsk /f` on USB media) is described in
   `docs/PORTING.md` §6 and `docs/LOGFILE.md` §7.

Not installed here: no VM is part of this checkout; `run-tests.sh` has no
kernel mode yet. When one is added it should consume the `find ... sha256sum`
listing above, which is the only exchange format needed.
