# Shared environment for the tools/ scripts. Source it, do not run it.
#
#   . "$(dirname "$0")/env.sh"
#
# Provides:
#   NTFS_TOOLS_DIR   absolute path of tools/
#   NTFS_LOCAL       tools/.local  (ntfsprogs + libntfs-3g install prefix)
#   NTFS_IMAGES      tools/images  (generated fixtures)
#   ensure_cmake     puts a native cmake/ninja on PATH, installing them into
#                    tools/.local/pip via pip if the host has none
#   need_tool NAME   fails with a clear message if NAME is not on PATH

NTFS_TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NTFS_ROOT="$(cd "$NTFS_TOOLS_DIR/.." && pwd)"
NTFS_LOCAL="$NTFS_TOOLS_DIR/.local"
NTFS_IMAGES="$NTFS_TOOLS_DIR/images"
export NTFS_TOOLS_DIR NTFS_ROOT NTFS_LOCAL NTFS_IMAGES

# Our own binaries first (mkntfs lives in sbin).
export PATH="$NTFS_LOCAL/bin:$NTFS_LOCAL/sbin:$NTFS_LOCAL/pip/bin:$PATH"

need_tool() {
	command -v "$1" >/dev/null 2>&1 && return 0
	echo "error: '$1' not found on PATH. ${2:-}" >&2
	return 1
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
