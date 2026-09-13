#!/bin/bash
# tools/run-tests.sh - differential test runner for the NTFS core.
#
#   tools/run-tests.sh [options] MODE...
#
# Modes (several may be given; `all` = verify ground-truth write):
#   build         build ntfsprogs (if missing), mkfixtures, ntfscli; nothing else
#   fixtures      generate any missing image/manifest in tools/images
#   verify        `ntfscli verify IMG MANIFEST` for every fixture
#   ground-truth  diff ntfs-3g reads (ntfsls -R -l, ntfscat, ntfscat -n) against
#                 `ntfscli ls -R -l`, `ntfscli cat`, `ntfscli xattr get`
#   write         on a scratch copy of each image: mkdir / cp-in / mv / truncate /
#                 xattr / rm / rmdir through ntfscli, each step cross-checked with
#                 ntfs-3g reads; then ntfsfix -n, ntfsck (informational), and a
#                 re-verify of the untouched manifest entries
#
# Options:
#   --image NAME       only this fixture (repeatable; default: every manifest found)
#   --core-lib PATH    libntfscore.a to link ntfscli against
#                      (default: $NTFS_ROOT/build/libntfscore.a if present, else a
#                      native build of the top-level tree into tools/build/core)
#   --stub             link ntfscli against core/stub (every op fails with ENOSYS;
#                      exercises the runner itself)
#   --ntfscli PATH     use this ntfscli binary instead of building one
#                      (tools/ntfscli-shim.sh answers ls/cat/xattr with ntfs-3g and
#                      is the runner's own plumbing check)
#   --sanitize         build ntfscli with -fsanitize=address,undefined
#   --no-data          pass --no-data to `ntfscli verify` (skip content hashes)
#   --no-times         pass --no-times to `ntfscli verify`
#   --quick            ground-truth: hash at most 200 files per image (evenly spaced)
#   --max-files N      ground-truth: hash at most N files per image (0 = all)
#   --keep             keep the scratch directory (write mode) and print its path
#   -v                 show every command's output on the terminal, not only in logs
#
# Every check is PASS/FAIL/SKIP; a summary is printed at the end and the exit
# status is 1 if anything failed, 2 on usage/setup errors. Full output of every
# command is in tools/build/test-logs/.
set -u
. "$(dirname "$0")/env.sh"

# ntfsls/ntfscat print names in the current locale; force UTF-8 so names.img
# round-trips byte for byte.
if locale -a 2>/dev/null | grep -qx 'en_US.UTF-8'; then export LC_ALL=en_US.UTF-8
elif locale -a 2>/dev/null | grep -qx 'C.UTF-8'; then export LC_ALL=C.UTF-8
fi

usage() {
	sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
	exit 2
}

MODES=()
ONLY_IMAGES=()
CORE_LIB="${NTFSCORE_LIB:-}"
USE_STUB=0
NTFSCLI_BIN="${NTFSCLI:-}"
SANITIZE=0
VERIFY_OPTS=()
MAX_FILES=0
KEEP=0
VERBOSE=0
while [ $# -gt 0 ]; do
	case "$1" in
	--image) ONLY_IMAGES+=("$2"); shift ;;
	--core-lib) CORE_LIB="$2"; shift ;;
	--stub) USE_STUB=1 ;;
	--ntfscli) NTFSCLI_BIN="$2"; shift ;;
	--sanitize) SANITIZE=1 ;;
	--no-data|--no-times) VERIFY_OPTS+=("$1") ;;
	--quick) MAX_FILES=200 ;;
	--max-files) MAX_FILES="$2"; shift ;;
	--keep) KEEP=1 ;;
	-v) VERBOSE=1 ;;
	-h|--help) usage ;;
	build|fixtures|verify|ground-truth|write) MODES+=("$1") ;;
	all) MODES+=(verify ground-truth write) ;;
	*) echo "run-tests: unknown argument '$1'" >&2; usage ;;
	esac
	shift
done
[ ${#MODES[@]} -gt 0 ] || usage

LOGDIR="$NTFS_TOOLS_DIR/build/test-logs"
rm -rf "$LOGDIR"
mkdir -p "$LOGDIR"

# ---------------------------------------------------------------------------
# result bookkeeping

NPASS=0; NFAIL=0; NSKIP=0
FAILED=()
pass() { NPASS=$((NPASS + 1)); printf '  PASS  %s\n' "$1"; }
fail() { NFAIL=$((NFAIL + 1)); FAILED+=("$1: $2"); printf '  FAIL  %s: %s\n' "$1" "$2"; }
skip() { NSKIP=$((NSKIP + 1)); printf '  SKIP  %s: %s\n' "$1" "$2"; }
die() { echo "run-tests: $*" >&2; exit 2; }
note() { printf '==> %s\n' "$*"; }

# run LOGNAME cmd... : run a command, tee its output to a log, return its status
run() {
	local log="$LOGDIR/$1.log"; shift
	if [ "$VERBOSE" = 1 ]; then
		printf '$ %s\n' "$*" | tee -a "$log"
		"$@" 2>&1 | tee -a "$log"
		return "${PIPESTATUS[0]}"
	fi
	printf '$ %s\n' "$*" >>"$log"
	"$@" >>"$log" 2>&1
}

# ---------------------------------------------------------------------------
# prerequisites

ensure_ntfsprogs() {
	if [ ! -x "$NTFS_LOCAL/sbin/mkntfs" ] || [ ! -x "$NTFS_LOCAL/bin/ntfsls" ] || [ ! -f "$NTFS_LOCAL/lib/libntfs-3g.a" ]; then
		note "ntfsprogs not built; running build-ntfsprogs.sh"
		"$NTFS_TOOLS_DIR/build-ntfsprogs.sh" || die "build-ntfsprogs.sh failed"
	fi
	for t in mkntfs ntfsls ntfscat ntfsinfo ntfsfix ntfslabel; do
		need_tool "$t" "(run tools/build-ntfsprogs.sh)" || exit 2
	done
}

# ntfsprogs-plus's ntfsck is the only checker available here that actually
# verifies structure: MFT records, index B-trees, the cluster bitmap. Tuxera's
# ntfsck is a stub in 2026.7.7 and its ntfsfix only looks at $MFT/$MFTMirr and
# the alternate boot sector -- an image with a zeroed in-use MFT record passes
# ntfsfix and fails this one, which is why it is worth a second toolchain.
#
# Not built by default: it needs autotools, which macOS does not ship. Missing
# means skip and say so, unless NTFS_REQUIRE_FSCK=1 makes it a hard failure for
# CI, where a silently absent gate is worse than a red one.
NTFSCK_PLUS="$NTFS_PLUS/sbin/ntfsck"
have_fsck_plus() { [ -x "$NTFSCK_PLUS" ]; }

ensure_fixtures() {
	note "fixtures"
	make -s -C "$NTFS_TOOLS_DIR/mkfixtures" mkfixtures >"$LOGDIR/mkfixtures-build.log" 2>&1 || {
		cat "$LOGDIR/mkfixtures-build.log"; die "mkfixtures did not build"; }
	local names missing=0 n
	names=$("$NTFS_TOOLS_DIR/mkfixtures/mkfixtures" --help 2>&1 | sed -n 's/^fixtures: //p')
	[ -n "$names" ] || die "mkfixtures --help did not list fixtures"
	for n in $names; do
		[ -f "$NTFS_IMAGES/$n.img" ] && [ -f "$NTFS_IMAGES/$n.manifest.json" ] || missing=1
	done
	if [ "$missing" = 1 ]; then
		note "generating missing fixtures into $NTFS_IMAGES"
		mkdir -p "$NTFS_IMAGES"
		( cd "$NTFS_TOOLS_DIR/mkfixtures" && ./mkfixtures -o "$NTFS_IMAGES" --mkntfs "$NTFS_LOCAL/sbin/mkntfs" ) \
			2>&1 | tee "$LOGDIR/mkfixtures.log" || die "mkfixtures failed"
	fi
	for n in $names; do
		[ -f "$NTFS_IMAGES/$n.img" ] && [ -f "$NTFS_IMAGES/$n.manifest.json" ] || die "fixture $n still missing"
	done
	echo "    $(echo $names | wc -w | tr -d ' ') fixtures present: $names"
}

# Build libntfscore.a natively (the top-level tree may be missing or built by
# an x86_64 cmake). Only used when build/libntfscore.a does not exist.
build_core() {
	local bdir="$NTFS_TOOLS_DIR/build/core"
	note "no $NTFS_ROOT/build/libntfscore.a; building the core into $bdir"
	ensure_cmake || die "no cmake"
	run core-configure cmake -S "$NTFS_ROOT" -B "$bdir" -DNTFS_BUILD_TESTS=OFF \
		$([ "$SANITIZE" = 1 ] && echo -DNTFS_SANITIZE=ON) || { cat "$LOGDIR/core-configure.log"; die "core configure failed"; }
	run core-build cmake --build "$bdir" -j "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" || {
		tail -40 "$LOGDIR/core-build.log"; die "core build failed (see $LOGDIR/core-build.log)"; }
	CORE_LIB="$bdir/libntfscore.a"
}

build_ntfscli() {
	[ -z "$NTFSCLI_BIN" ] || { note "using ntfscli: $NTFSCLI_BIN"; [ -x "$NTFSCLI_BIN" ] || die "$NTFSCLI_BIN not executable"; return; }
	ensure_cmake || die "no cmake"
	local bdir args=()
	if [ "$USE_STUB" = 1 ]; then
		bdir="$NTFS_TOOLS_DIR/build/ntfscli-stub"
		args+=(-DNTFSCLI_USE_STUB=ON)
		note "building ntfscli against core/stub (every operation returns ENOSYS)"
	else
		if [ -z "$CORE_LIB" ]; then
			if [ -f "$NTFS_ROOT/build/libntfscore.a" ]; then CORE_LIB="$NTFS_ROOT/build/libntfscore.a"; else build_core; fi
		fi
		[ -f "$CORE_LIB" ] || die "core library $CORE_LIB not found"
		bdir="$NTFS_TOOLS_DIR/build/ntfscli"
		args+=(-DNTFSCLI_USE_STUB=OFF "-DNTFSCORE_LIB=$CORE_LIB")
		note "building ntfscli against $CORE_LIB ($(lipo -archs "$CORE_LIB" 2>/dev/null || echo '?'))"
	fi
	[ "$SANITIZE" = 1 ] && args+=(-DNTFSCLI_SANITIZE=ON) || args+=(-DNTFSCLI_SANITIZE=OFF)
	# a stale cache from the other mode must not leak across
	[ -f "$bdir/CMakeCache.txt" ] && ! grep -q "NTFSCORE_LIB:FILEPATH=${CORE_LIB:-none}" "$bdir/CMakeCache.txt" 2>/dev/null && [ "$USE_STUB" = 0 ] && rm -rf "$bdir"
	run ntfscli-configure cmake -S "$NTFS_TOOLS_DIR/ntfscli" -B "$bdir" "${args[@]}" || { cat "$LOGDIR/ntfscli-configure.log"; die "ntfscli configure failed"; }
	if ! run ntfscli-build cmake --build "$bdir"; then
		echo
		echo "ntfscli does not link against $CORE_LIB. Undefined symbols:"
		grep -E '^\s+"_' "$LOGDIR/ntfscli-build.log" | sed 's/, referenced from://' | sort -u | sed 's/^ */    /'
		echo "(the ntfs_* entry points of core/include/ntfscore.h are implemented by core/vfs/;"
		echo " until that lands use --stub to exercise the runner, see $LOGDIR/ntfscli-build.log)"
		fail build "ntfscli link failed against $CORE_LIB"
		return 1
	fi
	NTFSCLI_BIN="$bdir/ntfscli"
	pass "build ntfscli ($("$NTFSCLI_BIN" -h 2>&1 | sed -n 's/^core: //p'))"
}

select_images() {
	IMAGES=()
	local m n
	if [ ${#ONLY_IMAGES[@]} -gt 0 ]; then
		for n in "${ONLY_IMAGES[@]}"; do
			n="${n%.img}"
			[ -f "$NTFS_IMAGES/$n.manifest.json" ] || die "no manifest for image '$n' in $NTFS_IMAGES"
			IMAGES+=("$n")
		done
	else
		for m in "$NTFS_IMAGES"/*.manifest.json; do
			[ -f "$m" ] || die "no manifests in $NTFS_IMAGES (run: $0 fixtures)"
			n="$(basename "$m" .manifest.json)"
			[ -f "$NTFS_IMAGES/$n.img" ] && IMAGES+=("$n")
		done
	fi
}

# ---------------------------------------------------------------------------
# normalisers: both listings become sorted "path<TAB>size" (files) / "path/" (dirs)

# ntfsls -R -l -F: "/dir:" headers, then "<size> Mon dd HH:MM YYYY name[/]"
norm_ntfsls() {
	awk '
	/^\/.*:$/ { dir = substr($0, 1, length($0) - 1); sub(/\/$/, "", dir); next }
	/^$/ { next }
	{
		if (!match($0, /^ *[0-9]+ [A-Z][a-z][a-z] [ 0-9][0-9] [0-9][0-9]:[0-9][0-9] +-?[0-9]+ /)) next
		hdr = substr($0, RSTART, RLENGTH); name = substr($0, RLENGTH + 1)
		if (name == "./" || name == "../" || name == "." || name == "..") next
		size = hdr; sub(/^ */, "", size); sub(/ .*/, "", size)
		if (name ~ /\/$/) print dir "/" name; else print dir "/" name "\t" size
	}' | LC_ALL=C sort
}

# ntfscli ls -R -l: "/dir:" headers, then "fffff nn <size> YYYY-MM-DDTHH:MM:SS.fffffffZ name[/]"
norm_ntfscli() {
	awk '
	/^\/.*:$/ { dir = substr($0, 1, length($0) - 1); sub(/\/$/, "", dir); next }
	/^$/ { next }
	{
		if (!match($0, /^[-dl][-c][-s][-a][-e] +[0-9]+ +[0-9]+ [-0-9]+T[0-9:.]+Z /)) next
		hdr = substr($0, RSTART, RLENGTH); name = substr($0, RLENGTH + 1)
		n = split(hdr, f, " +"); size = f[3]
		if (name ~ /\/$/) print dir "/" name; else print dir "/" name "\t" size
	}' | LC_ALL=C sort
}

sha() { shasum -a 256 | cut -d' ' -f1; }

# named $DATA streams of PATH on IMG, one per line, from the ntfs-3g manifest
manifest_streams() {	# IMAGE-NAME PATH
	python3 - "$NTFS_IMAGES/$1.manifest.json" "$2" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
for e in m["entries"]:
    if e["path"] == sys.argv[2]:
        for s in e.get("streams", []):
            print(s["name"])
EOF
}

# ---------------------------------------------------------------------------
# verify mode

mode_verify() {
	local n img man rc
	note "verify: ntfscli verify against mkfixtures manifests"
	for n in "${IMAGES[@]}"; do
		img="$NTFS_IMAGES/$n.img"; man="$NTFS_IMAGES/$n.manifest.json"
		run "verify-$n" "$NTFSCLI_BIN" --ro verify "$img" "$man" ${VERIFY_OPTS[@]+"${VERIFY_OPTS[@]}"}; rc=$?
		case $rc in
		0) pass "verify $n ($(grep -o '[0-9]* entries checked' "$LOGDIR/verify-$n.log" | tail -1))" ;;
		3) fail "verify $n" "$(grep -m1 -o '[0-9]* mismatches.*' "$LOGDIR/verify-$n.log"); first: $(grep -m1 '^mismatch\|^ntfscli' "$LOGDIR/verify-$n.log" | cut -c1-120)" ;;
		*) fail "verify $n" "exit $rc: $(tail -1 "$LOGDIR/verify-$n.log" | cut -c1-140)" ;;
		esac
	done
}

# ---------------------------------------------------------------------------
# ground-truth mode

files_of() {	# normalised listing -> file paths (NUL separated)
	awk -F'\t' 'NF == 2 { printf "%s%c", $1, 0 }' "$1"
}

mode_ground_truth() {
	local n img a b step total done_n bad errs f s r1 r2 t1="$LOGDIR/gt.a.tmp" t2="$LOGDIR/gt.b.tmp"
	note "ground-truth: ntfs-3g (ntfsls/ntfscat) vs ntfscli"
	for n in "${IMAGES[@]}"; do
		img="$NTFS_IMAGES/$n.img"
		a="$LOGDIR/gt-$n.ntfsls"; b="$LOGDIR/gt-$n.ntfscli"
		if ! ntfsls -R -l -F "$img" >"$a.raw" 2>"$LOGDIR/gt-$n.ntfsls.err"; then
			fail "ground-truth $n names" "ntfsls failed: $(tail -1 "$LOGDIR/gt-$n.ntfsls.err")"; continue
		fi
		norm_ntfsls <"$a.raw" >"$a"
		if ! "$NTFSCLI_BIN" --ro ls "$img" -R -l >"$b.raw" 2>"$LOGDIR/gt-$n.ntfscli.err"; then
			fail "ground-truth $n names" "ntfscli ls -R failed: $(tail -1 "$LOGDIR/gt-$n.ntfscli.err" | cut -c1-140)"; continue
		fi
		norm_ntfscli <"$b.raw" >"$b"
		if diff "$a" "$b" >"$LOGDIR/gt-$n.names.diff"; then
			pass "ground-truth $n names+sizes ($(wc -l <"$a" | tr -d ' ') entries)"
		else
			fail "ground-truth $n names+sizes" "$(grep -c '^[<>]' "$LOGDIR/gt-$n.names.diff") differing lines, see $LOGDIR/gt-$n.names.diff"
		fi

		# content: every file's unnamed stream (or an evenly spaced sample)
		total=$(awk -F'\t' 'NF == 2' "$a" | wc -l | tr -d ' ')
		step=1
		[ "$MAX_FILES" -gt 0 ] && [ "$total" -gt "$MAX_FILES" ] && step=$(( (total + MAX_FILES - 1) / MAX_FILES ))
		done_n=0; bad=0; errs=""
		while IFS= read -r -d '' f; do
			[ $(( done_n % step )) -eq 0 ] || { done_n=$((done_n + 1)); continue; }
			done_n=$((done_n + 1))
			ntfscat "$img" "$f" >"$t1" 2>>"$LOGDIR/gt-$n.cat.err"; r1=$?
			"$NTFSCLI_BIN" -q --ro cat "$img" "$f" >"$t2" 2>>"$LOGDIR/gt-$n.cat.err"; r2=$?
			if [ "$r1" != 0 ] || [ "$r2" != 0 ] || ! cmp -s "$t1" "$t2"; then
				bad=$((bad + 1)); errs="$errs $f"
				printf 'MISMATCH %s: ntfscat rc=%d %s, ntfscli rc=%d %s\n' "$f" "$r1" "$(sha <"$t1")" "$r2" "$(sha <"$t2")" >>"$LOGDIR/gt-$n.cat.err"
			fi
		done < <(files_of "$a")
		local checked=$(( (total + step - 1) / step ))
		[ "$total" -gt 0 ] || checked=0
		if [ "$bad" = 0 ]; then
			pass "ground-truth $n content ($checked of $total files hashed)"
		else
			fail "ground-truth $n content" "$bad of $checked files differ, e.g.$(echo "$errs" | cut -c1-100); see $LOGDIR/gt-$n.cat.err"
		fi

		# named streams: which files have them comes from the manifest, the
		# bytes from ntfscat -n vs ntfscli xattr get
		if command -v python3 >/dev/null 2>&1; then
			bad=0; errs=""; local nstreams=0
			while IFS=$'\t' read -r f s; do
				[ -n "$s" ] || continue
				nstreams=$((nstreams + 1))
				ntfscat -n "$s" "$img" "$f" >"$t1" 2>>"$LOGDIR/gt-$n.ads.err"; r1=$?
				"$NTFSCLI_BIN" -q --ro xattr "$img" get "$f" "$s" >"$t2" 2>>"$LOGDIR/gt-$n.ads.err"; r2=$?
				if [ "$r1" != 0 ] || [ "$r2" != 0 ] || ! cmp -s "$t1" "$t2"; then
					bad=$((bad + 1)); errs="$errs $f:$s"
					printf 'MISMATCH %s:%s: ntfscat rc=%d %s, ntfscli rc=%d %s\n' "$f" "$s" "$r1" "$(sha <"$t1")" "$r2" "$(sha <"$t2")" >>"$LOGDIR/gt-$n.ads.err"
				fi
			done < <(python3 - "$NTFS_IMAGES/$n.manifest.json" <<'EOF'
import json, sys
for e in json.load(open(sys.argv[1]))["entries"]:
    for s in e.get("streams", []):
        print(e["path"] + "\t" + s["name"])
EOF
)
			if [ "$nstreams" = 0 ]; then :
			elif [ "$bad" = 0 ]; then pass "ground-truth $n named streams ($nstreams streams)"
			else fail "ground-truth $n named streams" "$bad of $nstreams differ, e.g.$(echo "$errs" | cut -c1-100)"
			fi
		else
			skip "ground-truth $n named streams" "python3 not found (needed to read the manifest)"
		fi
	done
}

# ---------------------------------------------------------------------------
# write mode

SCRATCH=""
cleanup() { [ "$KEEP" = 1 ] && [ -n "$SCRATCH" ] && echo "scratch kept: $SCRATCH" || rm -rf "$SCRATCH"; }
trap cleanup EXIT

# wstep NAME cmd... : one write-mode step; failure recorded, returns status
wstep() {
	local name="$1"; shift
	if run "write-$WIMG" "$@"; then pass "write $WIMG: $name"; return 0; fi
	fail "write $WIMG: $name" "$(grep -v '^\$ ' "$LOGDIR/write-$WIMG.log" | tail -1 | cut -c1-140)"
	return 1
}
# wcheck NAME EXPECTED ACTUAL : compare two strings
wcheck() {
	if [ "$2" = "$3" ]; then pass "write $WIMG: $1"; return 0; fi
	fail "write $WIMG: $1" "expected '$2', got '$3'"
	return 1
}

# the mutating steps on one scratch image; returns early only when nothing
# further could work (no directory to work in)
write_steps() {	# IMG
	local img="$1" cli="$NTFSCLI_BIN" d=/rt-write big="$SCRATCH/big.bin" small="$SCRATCH/small.txt"
	local bigsize=$((3 * 1024 * 1024 + 12345)) bigsha smallsha
	bigsha=$(sha <"$big"); smallsha=$(sha <"$small")

	wstep "mkdir $d" "$cli" mkdir "$img" "$d" || return 1
	wcheck "ntfsls sees $d" "$d/" "$(ntfsls -R -l -F "$img" 2>/dev/null | norm_ntfsls | grep -x "$d/")"
	wstep "cp-in big.bin" "$cli" cp-in "$img" "$big" "$d/big.bin"
	wstep "cp-in small.txt" "$cli" cp-in "$img" "$small" "$d/small.txt"
	wcheck "ntfscat big.bin == host" "$bigsha" "$(ntfscat "$img" "$d/big.bin" 2>/dev/null | sha)"
	wcheck "ntfscat small.txt == host" "$smallsha" "$(ntfscat "$img" "$d/small.txt" 2>/dev/null | sha)"
	wcheck "ntfscli cat big.bin == host" "$bigsha" "$("$cli" -q --ro cat "$img" "$d/big.bin" 2>/dev/null | sha)"
	wstep "cp-in overwrite small.txt with big.bin" "$cli" cp-in "$img" "$big" "$d/small.txt"
	wcheck "ntfscat overwritten small.txt" "$bigsha" "$(ntfscat "$img" "$d/small.txt" 2>/dev/null | sha)"

	wstep "mkdir $d/sub" "$cli" mkdir "$img" "$d/sub"
	wstep "mv big.bin -> sub/moved.bin" "$cli" mv "$img" "$d/big.bin" "$d/sub/moved.bin"
	wcheck "ntfsls after mv" "$(printf '%s\n%s\t%s\n%s\n%s\t%s' "$d/" "$d/small.txt" "$bigsize" "$d/sub/" "$d/sub/moved.bin" "$bigsize")" \
		"$(ntfsls -R -l -F "$img" 2>/dev/null | norm_ntfsls | grep "^$d/")"
	wstep "mv sub/moved.bin over small.txt (replace)" "$cli" mv "$img" "$d/sub/moved.bin" "$d/small.txt"
	wcheck "ntfscat after replacing mv" "$bigsha" "$(ntfscat "$img" "$d/small.txt" 2>/dev/null | sha)"

	wstep "truncate small.txt to 4000" "$cli" truncate "$img" "$d/small.txt" 4000
	wcheck "ntfscat size after shrink" "$(head -c 4000 "$big" | sha)" "$(ntfscat "$img" "$d/small.txt" 2>/dev/null | sha)"
	wstep "truncate small.txt to 2 MiB (grow)" "$cli" truncate "$img" "$d/small.txt" $((2 * 1024 * 1024))
	wcheck "ntfscat after grow: prefix kept, tail zero" \
		"$( { head -c 4000 "$big"; head -c $((2 * 1024 * 1024 - 4000)) /dev/zero; } | sha)" \
		"$(ntfscat "$img" "$d/small.txt" 2>/dev/null | sha)"

	wstep "xattr set user.note" "$cli" xattr "$img" set "$d/small.txt" user.note "a note in an ADS"
	wcheck "ntfscat -n user.note" "a note in an ADS" "$(ntfscat -n user.note "$img" "$d/small.txt" 2>/dev/null)"
	wstep "xattr set from file (@big)" "$cli" xattr "$img" set "$d/small.txt" com.apple.blob "@$big"
	wcheck "ntfscat -n com.apple.blob" "$bigsha" "$(ntfscat -n com.apple.blob "$img" "$d/small.txt" 2>/dev/null | sha)"
	wcheck "xattr list" "$(printf 'com.apple.blob\nuser.note')" "$("$cli" -q --ro xattr "$img" list "$d/small.txt" 2>/dev/null | LC_ALL=C sort)"
	wcheck "ntfsinfo lists both streams" "$(printf 'com.apple.blob\nuser.note')" "$(ntfsinfo -F "$d/small.txt" "$img" 2>/dev/null | sed -n "s/^.Attribute name:[[:space:]]*'\(.*\)'$/\1/p" | LC_ALL=C sort)"
	wstep "xattr rm user.note" "$cli" xattr "$img" rm "$d/small.txt" user.note
	wcheck "ntfsinfo after xattr rm" "com.apple.blob" "$(ntfsinfo -F "$d/small.txt" "$img" 2>/dev/null | sed -n "s/^.Attribute name:[[:space:]]*'\(.*\)'$/\1/p")"

	wstep "rm small.txt" "$cli" rm "$img" "$d/small.txt"
	wstep "rmdir sub" "$cli" rmdir "$img" "$d/sub"
	wstep "rmdir $d" "$cli" rmdir "$img" "$d"
	wstep "sync" "$cli" sync "$img"
	return 0
}

mode_write() {
	local n img rc
	note "write: mkdir/cp-in/mv/truncate/xattr/rm on scratch copies, cross-checked with ntfs-3g"
	[ -n "$SCRATCH" ] || SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/ntfs-tests.XXXXXX")"
	head -c $((3 * 1024 * 1024 + 12345)) /dev/urandom >"$SCRATCH/big.bin"
	printf 'hello, ntfs\n' >"$SCRATCH/small.txt"

	for n in "${IMAGES[@]}"; do
		WIMG="$n"
		img="$SCRATCH/$n.img"
		cp -c "$NTFS_IMAGES/$n.img" "$img" 2>/dev/null || cp "$NTFS_IMAGES/$n.img" "$img" || die "cannot copy image"
		echo "  -- $n ($img)"
		write_steps "$img" || skip "write $n: remaining steps" "no working directory on the volume"

		# consistency as ntfs-3g sees it, whatever the steps did
		if run "write-$n-ntfsfix" ntfsfix -n "$img"; then pass "write $n: ntfsfix -n clean"
		else fail "write $n: ntfsfix -n" "$(grep -v '^\$ ' "$LOGDIR/write-$n-ntfsfix.log" | tail -1 | cut -c1-140)"; fi
		# -n: check and never write. A checker that repaired the image would
		# be measuring its own repair rather than what our driver wrote.
		if have_fsck_plus; then
			run "write-$n-fsck" "$NTFSCK_PLUS" -n "$img"; rc=$?
			if [ $rc = 0 ]; then pass "write $n: fsck.ntfs (ntfsprogs-plus) clean"
			else fail "write $n: fsck.ntfs" "exit $rc: $(grep -viE 'percent completed' "$LOGDIR/write-$n-fsck.log" | grep -v '^\$ ' | tail -2 | tr '\n' ' ' | cut -c1-160)"; fi
		elif [ "${NTFS_REQUIRE_FSCK:-0}" = 1 ]; then
			fail "write $n: fsck.ntfs" "not built and NTFS_REQUIRE_FSCK=1 (run tools/build-ntfsprogs-plus.sh)"
		else
			skip "write $n: fsck.ntfs" "ntfsprogs-plus not built (tools/build-ntfsprogs-plus.sh)"
		fi
		if ntfsls -R -l -F "$img" >"$LOGDIR/write-$n.after.raw" 2>"$LOGDIR/write-$n.ntfsls.err"; then
			norm_ntfsls <"$LOGDIR/write-$n.after.raw" >"$LOGDIR/write-$n.after"
			if diff "$LOGDIR/gt-$n.ntfsls.before" "$LOGDIR/write-$n.after" >"$LOGDIR/write-$n.names.diff"; then
				pass "write $n: ntfsls identical to the pristine image (nothing left of /rt-write)"
			else
				fail "write $n: ntfsls after" "$(grep -c '^[<>]' "$LOGDIR/write-$n.names.diff") lines differ from the pristine listing, see $LOGDIR/write-$n.names.diff"
			fi
		else
			fail "write $n: ntfsls after" "ntfsls failed on the written image: $(tail -1 "$LOGDIR/write-$n.ntfsls.err")"
		fi
		# the original manifest must still verify (times of the untouched
		# entries are unchanged; the root directory is not in the manifest)
		run "write-$n-reverify" "$NTFSCLI_BIN" --ro verify "$img" "$NTFS_IMAGES/$n.manifest.json" ${VERIFY_OPTS[@]+"${VERIFY_OPTS[@]}"}; rc=$?
		case $rc in
		0) pass "write $n: re-verify manifest" ;;
		3) fail "write $n: re-verify manifest" "$(grep -m1 -o '[0-9]* mismatches.*' "$LOGDIR/write-$n-reverify.log")" ;;
		*) fail "write $n: re-verify manifest" "exit $rc: $(tail -1 "$LOGDIR/write-$n-reverify.log" | cut -c1-140)" ;;
		esac
	done
}

# ---------------------------------------------------------------------------

ensure_ntfsprogs
ensure_fixtures
select_images
echo "    images: ${IMAGES[*]}"
build_ntfscli || true

want() { local m; for m in "${MODES[@]}"; do [ "$m" = "$1" ] && return 0; done; return 1; }
if [ -n "$NTFSCLI_BIN" ]; then
	want verify && mode_verify
	want ground-truth && mode_ground_truth
	if want write; then
		for n in "${IMAGES[@]}"; do
			ntfsls -R -l -F "$NTFS_IMAGES/$n.img" 2>/dev/null | norm_ntfsls >"$LOGDIR/gt-$n.ntfsls.before"
		done
		mode_write
	fi
else
	for m in "${MODES[@]}"; do
		case $m in verify|ground-truth|write) skip "$m" "no ntfscli binary" ;; esac
	done
fi

echo
echo "==================== summary ===================="
printf '%d passed, %d failed, %d skipped   (logs: %s)\n' "$NPASS" "$NFAIL" "$NSKIP" "$LOGDIR"
if [ "$NFAIL" -gt 0 ]; then
	printf '  FAIL  %s\n' "${FAILED[@]}"
	echo "RESULT: FAIL"
	exit 1
fi
echo "RESULT: PASS"
exit 0
