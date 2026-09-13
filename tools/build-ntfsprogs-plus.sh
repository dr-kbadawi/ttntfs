#!/bin/bash
# Build ntfsprogs-plus into tools/.local-plus, for its fsck.ntfs/ntfsck.
#
#   tools/build-ntfsprogs-plus.sh [--clean] [--version 1.0.0]
#   Env: NTFSPLUS_VERSION, NTFSPLUS_JOBS
#
# Why a second ntfsprogs at all. Tuxera's ntfsprogs (build-ntfsprogs.sh) ships
# an ntfsck that is a stub in 2026.7.7: it prints "Unsupported: check_volume()"
# and exits without checking anything, which is why run-tests.sh skips it.
# ntfsprogs-plus, the fork maintained alongside the ntfs3 kernel driver, has a
# real one: it walks the MFT, the index B-trees and the cluster bitmap. That is
# the closest thing to chkdsk that runs without Windows, and PORTING.md phase 2
# names it next to chkdsk for exactly that reason.
#
# Why a separate prefix. Both projects install binaries called ntfsck, ntfsfix
# and mkntfs. run-tests.sh depends on Tuxera's ntfsls/ntfscat/ntfsinfo for its
# ground-truth comparisons, so the two must not shadow each other on PATH: this
# one goes to tools/.local-plus and is addressed by full path.
#
# It is a checker, never a repairer, in this tree. ntfsck can write fixes; every
# invocation here passes the read-only flag. A test that repaired the image it
# was checking would be measuring the repair, not our writes.
set -euo pipefail
. "$(dirname "$0")/env.sh"

VERSION="${NTFSPLUS_VERSION:-1.0.0}"
# Pinned so a re-pointed tag cannot silently change what validates our writes.
# 1.0.0 is an *annotated* tag, so it has two hashes and they are easy to mix up:
# refs/tags/1.0.0 is the tag object, refs/tags/1.0.0^{} is the commit it wraps.
# `git ls-remote --tags --refs` prints the former; a checkout leaves HEAD at the
# latter. Both are pinned, so re-pointing the tag or rewriting the commit fails.
TAGOBJ_1_0_0="5ef42107896de70606bdd0d0b12c7c89758a26cb"
COMMIT_1_0_0="9cd989100e33623b0acd44c2edc14b6c8c89229b"
JOBS="${NTFSPLUS_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
CLEAN=0
while [ $# -gt 0 ]; do
	case "$1" in
	--clean) CLEAN=1 ;;
	--version) VERSION="$2"; shift ;;
	*) echo "usage: $0 [--clean] [--version VER]" >&2; exit 2 ;;
	esac
	shift
done

SRCROOT="$NTFS_TOOLS_DIR/build/src"
SRCDIR="$SRCROOT/ntfsprogs-plus-$VERSION"
ARCH="$(uname -m)"
STAMP="$NTFS_PLUS/.built-$VERSION-$ARCH"

if [ "$CLEAN" = 1 ]; then
	rm -rf "$NTFS_PLUS" "$SRCDIR"
fi
if [ -f "$STAMP" ] && [ -x "$NTFS_PLUS/sbin/ntfsck" ]; then
	echo "ntfsprogs-plus $VERSION ($ARCH) already built in $NTFS_PLUS (use --clean to rebuild)"
	exit 0
fi

need_tool git
need_tool make "Install the Xcode command line tools: xcode-select --install"
need_tool cc   "Install the Xcode command line tools: xcode-select --install"

# The repository ships no generated configure, unlike Tuxera's release tarball,
# so autogen.sh has to run and that needs the GNU build system. ensure_autotools
# puts a self-contained one on PATH rather than requiring a system install.
ensure_autotools || exit 2

mkdir -p "$SRCROOT"

# --- fetch (pinned) --------------------------------------------------------
if [ ! -d "$SRCDIR/.git" ]; then
	rm -rf "$SRCDIR"
	echo "==> cloning ntfsprogs-plus $VERSION"
	git clone --quiet https://github.com/ntfsprogs-plus/ntfsprogs-plus "$SRCDIR"
fi
cd "$SRCDIR"
git fetch --quiet --tags origin
git checkout --quiet "$VERSION"
HAVE_COMMIT="$(git rev-parse HEAD)"
HAVE_TAGOBJ="$(git rev-parse "refs/tags/$VERSION")"
case "$VERSION" in
1.0.0)
	[ "$HAVE_COMMIT" = "$COMMIT_1_0_0" ] && [ "$HAVE_TAGOBJ" = "$TAGOBJ_1_0_0" ] || {
		echo "error: $VERSION does not match the pin." >&2
		echo "  commit    $HAVE_COMMIT (expected $COMMIT_1_0_0)" >&2
		echo "  tag object $HAVE_TAGOBJ (expected $TAGOBJ_1_0_0)" >&2
		echo "Check what changed before trusting it to validate writes." >&2
		exit 1; } ;;
*)	echo "==> (no pin for $VERSION; commit $HAVE_COMMIT)" ;;
esac

# --- build -----------------------------------------------------------------
# --disable-ntfs-3g: no FUSE driver wanted, and it is what pulls in the
#   pkg-config/FUSE probing that has nothing to satisfy it on macOS.
# --disable-crypto : we only want the checker; it also avoids needing gnutls.
# --disable-nls    : nothing here reads translations.
# ac_cv_header_libintl_h=no : --disable-nls is not enough. /usr/local/include is
#   on clang's default search path and the Intel Homebrew there has gettext, so
#   the probe succeeds, config.h gets HAVE_LIBINTL_H, and <libintl.h> macro-
#   redefines setlocale to libintl_setlocale. Every tool then fails to link
#   against a library that is not on the line and would be x86_64 anyway. Same
#   Homebrew hazard env.sh warns about for cmake.
echo "==> autogen"
./autogen.sh >"$NTFS_TOOLS_DIR/build/ntfsprogs-plus-autogen.log" 2>&1 || {
	tail -20 "$NTFS_TOOLS_DIR/build/ntfsprogs-plus-autogen.log" >&2
	echo "autogen.sh failed, see tools/build/ntfsprogs-plus-autogen.log" >&2; exit 1; }

echo "==> configure --prefix=$NTFS_PLUS"
./configure --prefix="$NTFS_PLUS" --disable-ntfs-3g --disable-crypto --disable-nls \
	--disable-shared --enable-static ac_cv_header_libintl_h=no \
	>"$NTFS_TOOLS_DIR/build/ntfsprogs-plus-configure.log" 2>&1 || {
	tail -30 "$NTFS_TOOLS_DIR/build/ntfsprogs-plus-configure.log" >&2
	echo "configure failed, see tools/build/ntfsprogs-plus-configure.log" >&2; exit 1; }

echo "==> make -j$JOBS"
make -j"$JOBS" >"$NTFS_TOOLS_DIR/build/ntfsprogs-plus-make.log" 2>&1 || {
	tail -30 "$NTFS_TOOLS_DIR/build/ntfsprogs-plus-make.log" >&2
	echo "make failed, see tools/build/ntfsprogs-plus-make.log" >&2; exit 1; }
# rootlibdir=libdir: libntfs/Makefile.am has a Linux install hook that moves
# libntfs.so* into /lib when rootlibdir differs from libdir. We build static, so
# the glob matches nothing and the mv fails; pointing rootlibdir at our own
# libdir makes its -ef test true and the hook skips itself.
make install rootlibdir="$NTFS_PLUS/lib" >"$NTFS_TOOLS_DIR/build/ntfsprogs-plus-install.log" 2>&1 || {
	tail -20 "$NTFS_TOOLS_DIR/build/ntfsprogs-plus-install.log" >&2
	echo "make install failed" >&2; exit 1; }

CK="$(command -v "$NTFS_PLUS/sbin/ntfsck" || command -v "$NTFS_PLUS/bin/ntfsck" || true)"
[ -n "$CK" ] || { echo "error: no ntfsck in $NTFS_PLUS after install" >&2; ls -R "$NTFS_PLUS" | head -40 >&2; exit 1; }
mkdir -p "$(dirname "$STAMP")" && : >"$STAMP"
echo
echo "ntfsprogs-plus $VERSION built: $CK"
