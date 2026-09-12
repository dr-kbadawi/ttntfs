#!/bin/bash
# Build ntfs-3g/ntfsprogs 2026.7.7 from source into tools/.local for the
# host architecture, without FUSE (--disable-ntfs-3g). Produces:
#   tools/.local/bin/{ntfsls,ntfscat,ntfsinfo,ntfscp,ntfsfix,ntfscluster,ntfscmp,...}
#   tools/.local/sbin/{mkntfs,ntfsclone,ntfslabel,ntfsresize,ntfsundelete,...}
#   tools/.local/lib/libntfs-3g.a  and  tools/.local/include/ntfs-3g/*.h
#
# Do not use Homebrew's ntfs-3g: on this machine it is a 2017 x86_64 build.
#
# Usage: tools/build-ntfsprogs.sh [--clean] [--version 2026.7.7]
# Env:   NTFS3G_VERSION, NTFS3G_JOBS
set -euo pipefail
. "$(dirname "$0")/env.sh"

VERSION="${NTFS3G_VERSION:-2026.7.7}"
# Tuxera's release tarball (it ships a generated ./configure, so no
# autoconf/automake/libtool are needed). The GitHub tag tarball has no
# configure and would need autotools; it is the fallback below.
SHA256_2026_7_7="d67b769025d32860549d35c2147e45024d172f81c540d750390ce3602c059dab"
CLEAN=0
while [ $# -gt 0 ]; do
	case "$1" in
	--clean) CLEAN=1 ;;
	--version) VERSION="$2"; shift ;;
	-h|--help) sed -n 2,14p "$0"; exit 0 ;;
	*) echo "unknown arg $1" >&2; exit 2 ;;
	esac
	shift
done

ARCH="$(uname -m)"
JOBS="${NTFS3G_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
SRCROOT="$NTFS_LOCAL/src"
SRCDIR="$SRCROOT/ntfs-3g-$VERSION"
TARBALL="$SRCROOT/ntfs-3g_ntfsprogs-$VERSION.tgz"
STAMP="$NTFS_LOCAL/.built-ntfs-3g-$VERSION-$ARCH"

if [ "$CLEAN" = 1 ]; then
	rm -rf "$NTFS_LOCAL/bin" "$NTFS_LOCAL/sbin" "$NTFS_LOCAL/lib" \
	       "$NTFS_LOCAL/include" "$NTFS_LOCAL/share" "$SRCDIR" "$STAMP"
fi
if [ -f "$STAMP" ] && [ -x "$NTFS_LOCAL/sbin/mkntfs" ] && [ -f "$NTFS_LOCAL/lib/libntfs-3g.a" ]; then
	echo "ntfs-3g $VERSION ($ARCH) already built in $NTFS_LOCAL (use --clean to rebuild)"
	exit 0
fi

need_tool curl
need_tool make "Install the Xcode command line tools: xcode-select --install"
need_tool cc   "Install the Xcode command line tools: xcode-select --install"
mkdir -p "$SRCROOT"

# --- fetch -----------------------------------------------------------------
if [ ! -s "$TARBALL" ]; then
	URL="https://download.tuxera.com/opensource/ntfs-3g_ntfsprogs-$VERSION.tgz"
	echo "==> fetching $URL"
	if ! curl -fSL --retry 3 -o "$TARBALL.part" "$URL"; then
		echo "==> Tuxera tarball not available, falling back to GitHub tag $VERSION"
		URL="https://github.com/tuxera/ntfs-3g/archive/refs/tags/$VERSION.tar.gz"
		curl -fSL --retry 3 -o "$TARBALL.part" "$URL"
	fi
	mv "$TARBALL.part" "$TARBALL"
fi
case "$VERSION" in
2026.7.7)
	echo "$SHA256_2026_7_7  $TARBALL" | shasum -a 256 -c - >/dev/null \
		|| { echo "sha256 mismatch for $TARBALL (expected the Tuxera dist tarball)" >&2; exit 1; } ;;
*)	echo "==> (no pinned sha256 for $VERSION)" ;;
esac

# --- unpack ----------------------------------------------------------------
rm -rf "$SRCDIR"
tar xzf "$TARBALL" -C "$SRCROOT"
[ -d "$SRCDIR" ] || { echo "tarball did not unpack to $SRCDIR" >&2; ls "$SRCROOT"; exit 1; }
cd "$SRCDIR"
if [ ! -x ./configure ]; then
	echo "==> no ./configure in tarball; running autogen (needs autoconf/automake/libtool)"
	for t in autoreconf aclocal automake libtoolize; do
		command -v "$t" >/dev/null 2>&1 || command -v "g$t" >/dev/null 2>&1 || {
			echo "error: $t missing. brew install autoconf automake libtool pkg-config" >&2; exit 1; }
	done
	./autogen.sh
fi

# --- configure -------------------------------------------------------------
# --disable-ntfs-3g : no FUSE driver -> configure skips the FUSE/pkg-config
#                     check on Darwin entirely.
# --disable-nfconv  : the Mac-only NFD/NFC name normalisation patch lives in the
#                     FUSE driver; we want libntfs-3g to store names verbatim
#                     (fixtures deliberately contain both NFC and NFD names).
# --enable-extras   : ntfstruncate/ntfswipe/ntfsdump_logfile/ntfsmove/...
# --enable-quarantined : ntfsck etc. (still useful as a second opinion)
# configure's PKG_PROG_PKG_CONFIG (pkg.m4 >= 0.29) aborts when pkg-config is
# missing even though nothing in this configuration queries it: the FUSE
# branch is skipped by --disable-ntfs-3g, gnutls is only for --enable-crypto,
# and the uuid probe uses AC_CHECK_HEADER/AC_CHECK_FUNC (macOS has
# uuid/uuid.h and uuid_generate in libSystem). If the host has no
# pkg-config, provide a shim that answers only the version probe and reports
# "not found" for any real query. `brew install pkg-config` works too.
if ! command -v pkg-config >/dev/null 2>&1; then
	mkdir -p "$NTFS_LOCAL/shim"
	cat >"$NTFS_LOCAL/shim/pkg-config" <<'SHIM'
#!/bin/sh
# Minimal stand-in used only while configuring ntfs-3g (see build-ntfsprogs.sh).
case "$1" in
--version) echo 1.9.5; exit 0 ;;
--atleast-pkgconfig-version) exit 0 ;;
esac
echo "pkg-config shim: no packages available ($*)" >&2
exit 1
SHIM
	chmod +x "$NTFS_LOCAL/shim/pkg-config"
	export PATH="$NTFS_LOCAL/shim:$PATH"
	echo "==> no pkg-config on host; using shim $NTFS_LOCAL/shim/pkg-config"
fi

echo "==> configuring ntfs-3g $VERSION for $ARCH into $NTFS_LOCAL"
# -isysroot: macOS clang otherwise searches /usr/local/include, where the
# Intel Homebrew's gettext drops a libintl.h that #defines setlocale to
# libintl_setlocale (x86_64-only library -> link failure). With a sysroot
# the default search path becomes $SDK/usr/local/include instead.
SDK="$(xcrun --show-sdk-path 2>/dev/null || true)"
export CC="${CC:-cc}"
export CFLAGS="${CFLAGS:--O2 -g -arch $ARCH${SDK:+ -isysroot $SDK}}"
export LDFLAGS="${LDFLAGS:--arch $ARCH${SDK:+ -isysroot $SDK}}"
export ac_cv_header_libintl_h=no
# --exec-prefix is required: without it configure sets rootlibdir=/lib and
# rootbindir=/bin (so a boot-time automounter can find the FUSE driver) and
# "make install" tries to move libntfs-3g.so* into /lib.
./configure --prefix="$NTFS_LOCAL" --exec-prefix="$NTFS_LOCAL" \
	--disable-ntfs-3g --enable-ntfsprogs \
	--disable-shared --enable-static \
	--disable-nfconv --disable-ldconfig --disable-mtab --disable-plugins \
	--disable-posix-acls --disable-xattr-mappings \
	--enable-extras --enable-quarantined \
	--with-uuid --without-hd \
	>"$SRCROOT/configure-$VERSION.log" 2>&1 || { tail -40 "$SRCROOT/configure-$VERSION.log"; exit 1; }

# --- build + install -------------------------------------------------------
echo "==> make -j$JOBS"
make -j"$JOBS" >"$SRCROOT/make-$VERSION.log" 2>&1 || { tail -60 "$SRCROOT/make-$VERSION.log"; exit 1; }
make install >"$SRCROOT/install-$VERSION.log" 2>&1 || { tail -40 "$SRCROOT/install-$VERSION.log"; exit 1; }
# The static-only build still installs a .la file; harmless.
touch "$STAMP"

echo "==> installed:"
for b in mkntfs ntfsls ntfscat ntfsinfo ntfscp ntfsfix ntfscluster ntfsclone ntfscmp ntfslabel ntfstruncate ntfsck; do
	p="$(command -v "$b" 2>/dev/null || true)"
	if [ -n "$p" ]; then printf '  %-12s %s\n' "$b" "$p"; else printf '  %-12s MISSING\n' "$b"; fi
done
printf '  %-12s %s\n' libntfs-3g.a "$NTFS_LOCAL/lib/libntfs-3g.a"
printf '  %-12s %s\n' headers "$NTFS_LOCAL/include/ntfs-3g/"
file "$NTFS_LOCAL/sbin/mkntfs" | sed 's/^/  /'
"$NTFS_LOCAL/sbin/mkntfs" --version 2>&1 | head -1 | sed 's/^/  /'
