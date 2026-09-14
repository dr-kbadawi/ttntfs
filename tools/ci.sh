#!/bin/bash
# Everything that can be checked without hardware, signing, or a human.
#
#   tools/ci.sh              run all stages
#   tools/ci.sh --quick      skip the fixture suite (the slow one)
#   tools/ci.sh --list       print the stages and exit
#
# Why this exists. Until 2026-09-14 nothing ran these suites automatically, and
# two failures sat unnoticed for a day each: logfile_unit hung in ASan's own
# init so its 243 checks never executed, and a stale header aborted ntfslog on
# every fixture. Both were caught by tests that already existed. A test nobody
# runs is not coverage.
#
# NOT run here, and why:
#   fskit/scripts/mount-test.sh   needs the FSKit module enabled and a real
#                                 mount; no hosted runner can do it
#   fskit/scripts/package.sh      needs the Developer ID certificate and a
#                                 notarization credential
# Those two stay manual, so a release still needs a human.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
cd "$ROOT"

QUICK=0
for a in "$@"; do
	case "$a" in
	--quick) QUICK=1 ;;
	--list) sed -n '/^[^#]*run_stage "/{s/.*run_stage "\([^"]*\)".*/  \1/p;}' "$0" | grep -v '\[\^' ; exit 0 ;;
	*) echo "usage: $0 [--quick] [--list]" >&2; exit 2 ;;
	esac
done

fail=0
declare -a results
start_all=$(date +%s)

run_stage() {
	local name="$1"; shift
	local t0 t1 rc
	printf '\n=== %s ===\n' "$name"
	t0=$(date +%s)
	"$@"
	rc=$?
	t1=$(date +%s)
	if [ $rc -eq 0 ]; then
		results+=("  PASS  $name ($((t1 - t0))s)")
	else
		results+=("  FAIL  $name ($((t1 - t0))s, exit $rc)")
		fail=1
	fi
	return 0		# keep going: one failure should not hide the others
}

# --- toolchain -------------------------------------------------------------
# run-tests.sh builds ntfsprogs itself if missing, but doing it here keeps the
# slow part in its own stage so a CI log shows where the time went.
stage_toolchain() {
	"$HERE/build-ntfsprogs.sh" || return 1
	# ntfsprogs-plus is the only real structural checker available without
	# Windows. It needs autotools, which macOS does not ship; if they are
	# missing this stage fails loudly rather than letting the fixture suite
	# quietly skip its strongest check.
	"$HERE/build-ntfsprogs-plus.sh" || return 1
}

# --- C core and platform ---------------------------------------------------
stage_ctest() {
	cmake -S . -B build >/dev/null || return 1
	cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null || return 1
	ctest --test-dir build --output-on-failure
}

# --- the same tests, instrumented ------------------------------------------
# A separate build directory on purpose: the default tree produces the
# libntfscore.a the Xcode app links, and an instrumented static library cannot
# be linked into a shipping app. ASan is left off because it deadlocks in its
# own initialiser on macOS 26 (see the top-level CMakeLists).
stage_ubsan() {
	cmake -S . -B build-ubsan -DNTFS_UBSAN=ON -DNTFS_LOGFILE_UBSAN=ON >/dev/null || return 1
	cmake --build build-ubsan -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null || return 1
	ctest --test-dir build-ubsan --output-on-failure
}

# --- the differential fixture suite ----------------------------------------
stage_fixtures() {
	NTFS_REQUIRE_FSCK=1 "$HERE/run-tests.sh" all
}

# --- the app's Swift unit tests --------------------------------------------
# Deployment target is macOS 26, so this needs an SDK that has FSKit. A runner
# image older than that cannot build it; say so and fail, rather than reporting
# a green run that tested nothing.
stage_swift() {
	command -v xcodebuild >/dev/null 2>&1 || { echo "no xcodebuild"; return 1; }
	command -v xcodegen  >/dev/null 2>&1 || { echo "no xcodegen (brew install xcodegen)"; return 1; }
	( cd fskit && xcodegen ) >/dev/null || return 1
	xcodebuild test -project fskit/NTFS.xcodeproj -scheme NTFSTests \
		-derivedDataPath "${DERIVED_DATA:-/tmp/ttntfs-ci-dd}" \
		2>&1 | grep -E 'Executed|error:|\*\* TEST|Testing failed' | tail -6
	return "${PIPESTATUS[0]}"
}

# --- the app and extension must still compile ------------------------------
# Debug only: Release is Developer ID signed and CI has no certificate.
stage_build_app() {
	command -v xcodebuild >/dev/null 2>&1 || { echo "no xcodebuild"; return 1; }
	xcodebuild -project fskit/NTFS.xcodeproj -scheme NTFS -configuration Debug \
		-derivedDataPath "${DERIVED_DATA:-/tmp/ttntfs-ci-dd}" \
		CODE_SIGNING_ALLOWED=NO build \
		2>&1 | grep -E '^\*\* BUILD|error:' | tail -4
	return "${PIPESTATUS[0]}"
}

run_stage "toolchain (ntfsprogs + ntfsprogs-plus)" stage_toolchain
run_stage "ctest (platform, mount decisions, logfile, scripts)" stage_ctest
run_stage "ctest under UBSan" stage_ubsan
[ "$QUICK" = 1 ] || run_stage "fixtures (run-tests.sh all, fsck required)" stage_fixtures
run_stage "swift unit tests" stage_swift
run_stage "app + extension build (Debug, unsigned)" stage_build_app

printf '\n==================== ci summary ====================\n'
printf '%s\n' "${results[@]}"
printf 'total %ss\n' "$(( $(date +%s) - start_all ))"
[ "$QUICK" = 1 ] && echo "(--quick: the fixture suite was skipped)"
exit $fail
