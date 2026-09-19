# Shared environment for the tools/ scripts. Source it, do not run it.
#
#   . "$(dirname "$0")/env.sh"
#
# Provides:
#   NTFS_TOOLS_DIR   absolute path of tools/
#   NTFS_LOCAL       tools/.local  (ntfsprogs + libntfs-3g install prefix)
#   NTFS_PLUS        tools/.local-plus  (ntfsprogs-plus: its real ntfsck)
#   NTFS_IMAGES      tools/images  (generated fixtures)
#   ensure_cmake     puts a native cmake/ninja on PATH, installing them into
#                    tools/.local/pip via pip if the host has none
#   need_tool NAME   fails with a clear message if NAME is not on PATH
#   ensure_autotools puts autoconf/automake/libtool on PATH for the one
#                    dependency that ships no generated configure

NTFS_TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NTFS_ROOT="$(cd "$NTFS_TOOLS_DIR/.." && pwd)"
NTFS_LOCAL="$NTFS_TOOLS_DIR/.local"
# ntfsprogs-plus keeps its own prefix: it installs binaries called ntfsck,
# ntfsfix and mkntfs, and the test runner needs Tuxera's ntfsls/ntfscat/ntfsinfo
# to stay the ones on PATH. Addressed by full path, never by shadowing.
NTFS_PLUS="$NTFS_TOOLS_DIR/.local-plus"
NTFS_IMAGES="$NTFS_TOOLS_DIR/images"
export NTFS_TOOLS_DIR NTFS_ROOT NTFS_LOCAL NTFS_PLUS NTFS_IMAGES

# Our own binaries first (mkntfs lives in sbin).
export PATH="$NTFS_LOCAL/bin:$NTFS_LOCAL/sbin:$NTFS_LOCAL/pip/bin:$PATH"

need_tool() {
	command -v "$1" >/dev/null 2>&1 && return 0
	echo "error: '$1' not found on PATH. ${2:-}" >&2
	return 1
}

# ntfsprogs-plus has no generated configure in its repository, so autogen.sh has
# to run, and macOS ships no autoconf. Same principle as ensure_cmake: install a
# self-contained toolchain under tools/ rather than asking for a system one, and
# never take the Intel Homebrew build. conda is used because autoconf and
# automake are not on PyPI. Set NTFS_AUTOTOOLS_BIN to point at your own.
ensure_autotools() {
	if [ -n "${NTFS_AUTOTOOLS_BIN:-}" ]; then
		export PATH="$NTFS_AUTOTOOLS_BIN:$PATH"
	fi
	# Homebrew installs GNU libtool's script as glibtoolize, keeping the
	# unprefixed name for Apple's unrelated libtool. autoreconf finds it if
	# LIBTOOLIZE says so. Without this, a Homebrew-only machine -- every
	# GitHub macOS runner -- fails here with everything installed.
	if command -v autoreconf >/dev/null 2>&1 && command -v glibtoolize >/dev/null 2>&1 &&
	   ! command -v libtoolize >/dev/null 2>&1; then
		export LIBTOOLIZE="$(command -v glibtoolize)"
		return 0
	fi
	if command -v autoreconf >/dev/null 2>&1 && command -v libtoolize >/dev/null 2>&1; then
		return 0
	fi
	if [ -x "$NTFS_LOCAL/autotools/bin/autoreconf" ]; then
		export PATH="$NTFS_LOCAL/autotools/bin:$PATH"
		export ACLOCAL_PATH="$NTFS_LOCAL/autotools/share/aclocal${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
		return 0
	fi
	if ! command -v conda >/dev/null 2>&1; then
		echo "error: autoconf/automake/libtool are needed to build ntfsprogs-plus." >&2
		echo "  conda create -p tools/.local/autotools -c conda-forge autoconf automake libtool pkg-config" >&2
		echo "  or: brew install autoconf automake libtool pkg-config" >&2
		echo "  or set NTFS_AUTOTOOLS_BIN to a directory containing them." >&2
		return 1
	fi
	echo "==> installing autoconf/automake/libtool into $NTFS_LOCAL/autotools"
	# libgcrypt is not for linking: ntfsprogs-plus's configure.ac calls
	# AM_PATH_LIBGCRYPT, and autoconf expands macros whether or not the shell
	# branch around them ever runs, so the macro has to exist even though the
	# build passes --disable-crypto. The package carries share/aclocal/libgcrypt.m4.
	conda create -y -p "$NTFS_LOCAL/autotools" -c conda-forge \
		autoconf automake libtool pkg-config libgcrypt >/dev/null || return 1
	export PATH="$NTFS_LOCAL/autotools/bin:$PATH"
	export ACLOCAL_PATH="$NTFS_LOCAL/autotools/share/aclocal${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
	command -v autoreconf >/dev/null 2>&1
}

# cmake is not part of macOS. Homebrew on this machine is the Intel build
# (/usr/local), so instead of pulling an x86_64 cmake through Rosetta we
# install the official arm64 wheels into tools/.local/pip. Nothing outside
# tools/ is modified. Set NTFS_CMAKE=/path/to/cmake to override.
ensure_cmake() {
	if [ -n "${NTFS_CMAKE:-}" ]; then
		export PATH="$(dirname "$NTFS_CMAKE"):$PATH"
	fi
	if command -v cmake >/dev/null 2>&1; then return 0; fi
	local py
	py="$(command -v python3 || true)"
	if [ -z "$py" ]; then
		echo "error: no cmake and no python3 to pip-install one; brew install cmake" >&2
		return 1
	fi
	echo "==> cmake not found; installing cmake+ninja wheels into $NTFS_LOCAL/pip"
	mkdir -p "$NTFS_LOCAL/pip"
	"$py" -m pip install --quiet --disable-pip-version-check --no-warn-script-location \
		--target "$NTFS_LOCAL/pip" cmake ninja || return 1
	# The wheels put the real binaries under <pkg>/data/bin; the --target
	# layout puts console_scripts into pip/bin with a python shebang that
	# lacks our sys.path, so link the binaries directly.
	mkdir -p "$NTFS_LOCAL/pip/bin"
	for b in cmake ctest cpack; do
		ln -sf "$NTFS_LOCAL/pip/cmake/data/bin/$b" "$NTFS_LOCAL/pip/bin/$b"
	done
	ln -sf "$NTFS_LOCAL/pip/ninja/data/bin/ninja" "$NTFS_LOCAL/pip/bin/ninja"
	command -v cmake >/dev/null 2>&1
}
