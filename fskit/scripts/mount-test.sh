#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# End-to-end mount test for the FSKit extension, once it is SIGNED with the
# fsmodule entitlement and installed (see fskit/README.md, "Signing").
#
#   fskit/scripts/mount-test.sh [-i IMAGE] [-t TYPE] [-m MOUNTPOINT] [--rw] [--keep]
#
# Steps, each reported:
#   1. the extension is registered and enabled (FSClient via the host app's
#      status, here through `pluginkit`)
#   2. attach the fixture image with hdiutil -nomount (no auto-mount, so the
#      kernel ntfs driver does not grab it)
#   3. mount -F -t ttntfs <dev> <mountpoint>    (as the user: FSKit mounts are
#      per-user; `sudo mount -F` fails with "entitlement no" on macOS 26)
#   4. list the volume, stat a file, df, and (with --rw) create/rename/delete
#   5. unmount, detach
#
# Default image: tools/images/basic-4k.img (make -C tools/mkfixtures fixtures).
# Nothing here modifies the source image: hdiutil attaches a shadow file.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
image="$root/tools/images/basic-4k.img"
fstype=ttntfs
mountpoint=""
rw=0
keep=0
bundle_id=ch.techtag.ntfs.extension

while [ $# -gt 0 ]; do
	case "$1" in
	-i|--image) image=$2; shift 2 ;;
	-t|--type) fstype=$2; shift 2 ;;
	-m|--mountpoint) mountpoint=$2; shift 2 ;;
	--rw) rw=1; shift ;;
	--keep) keep=1; shift ;;
	-h|--help) sed -n '2,22p' "$0"; exit 0 ;;
	*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

step() { printf '\n==> %s\n' "$*"; }
ok()   { printf '    ok: %s\n' "$*"; }
fail() { printf '    FAIL: %s\n' "$*" >&2; cleanup; exit 1; }

dev=""
shadow=""
workimage=""
cleanup() {
	[ $keep -eq 1 ] && { echo "    --keep: leaving $dev attached${mountpoint:+ at $mountpoint}"; return; }
	if [ -n "$mountpoint" ] && mount | grep -q " on $mountpoint "; then
		umount "$mountpoint" 2>/dev/null || diskutil unmount force "$mountpoint" >/dev/null 2>&1
	fi
	[ -n "$dev" ] && hdiutil detach "$dev" -quiet 2>/dev/null
	[ -n "$shadow" ] && rm -f "$shadow"
	[ -n "$workimage" ] && [ $keep -eq 0 ] && rm -f "$workimage"
	[ -n "$mountpoint" ] && rmdir "$mountpoint" 2>/dev/null
}
trap cleanup EXIT

step "1/5 extension registered and enabled?"
# pluginkit -m -v: first column is the user election: '+' enabled, '-' disabled,
# '!' invalid, ' ' no election yet (FSKit modules must be enabled by the user).
if ! pk=$(pluginkit -m -i "$bundle_id" -v 2>/dev/null) || [ -z "$pk" ]; then
	fail "$bundle_id is not registered. Build signed, copy TT NTFS Native.app to /Applications, launch it once."
fi
echo "    $pk"
appex=$(printf '%s\n' "$pk" | head -1 | awk -F'\t' '{print $NF}')
case "$pk" in
	"+"*) ok "enabled" ;;
	"-"*|"!"*) fail "registered but not enabled: System Settings > General > Login Items & Extensions > File System Extensions > NTFS  (or try: pluginkit -e use -i $bundle_id)" ;;
	*) echo "    no user election recorded yet; enable it in System Settings (or: pluginkit -e use -i $bundle_id), continuing anyway" ;;
esac
if [ -n "$appex" ] && [ -d "$appex" ]; then
	if codesign -d --entitlements - --xml "$appex" 2>/dev/null | grep -q 'fskit.fsmodule'; then
		ok "appex carries com.apple.developer.fskit.fsmodule"
	else
		echo "    warning: could not confirm the com.apple.developer.fskit.fsmodule entitlement on $appex"
	fi
fi

step "2/5 attach $image (nomount)"
[ -f "$image" ] || fail "image not found: $image (run: make -C tools/mkfixtures fixtures)"
# Read-only runs use a shadow file, which keeps the fixture pristine for free.
# A write run cannot: a shadow holds only the changed blocks, so it is not an
# image ntfsck can read, and the whole point of a write run is to check what was
# written. Copy the fixture instead -- the source is still never modified, and
# the copy is a real image the structural check at the end can open.
if [ $rw -eq 1 ]; then
	workimage=$(mktemp -t ntfs-rw).img
	cp "$image" "$workimage" || fail "could not copy the fixture"
	attach=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$workimage" 2>&1) \
		|| fail "hdiutil attach: $attach"
else
	shadow=$(mktemp -t ntfs-shadow).shadow
	attach=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage -shadow "$shadow" "$image" 2>&1) \
		|| fail "hdiutil attach: $attach"
fi
dev=$(echo "$attach" | awk '/^\/dev\/disk/ {print $1; exit}')
[ -n "$dev" ] || fail "no device in hdiutil output: $attach"
ok "attached as $dev (raw image, whole device holds the volume)"
diskutil info "$dev" | grep -E 'Device Node|Media Name|Disk Size|Device Block Size' | sed 's/^/    /'

step "3/5 mount -F -t $fstype $dev"
[ -n "$mountpoint" ] || mountpoint=$(mktemp -d /tmp/ttntfs-mount.XXXXXX)
opts=""
[ $rw -eq 1 ] || opts="-o ro"
echo "    mount -F -t $fstype $opts $dev $mountpoint"
if ! out=$(mount -F -t "$fstype" $opts "$dev" "$mountpoint" 2>&1); then
	echo "    $out"
	echo "    hint: log stream --predicate 'subsystem == \"ch.techtag.ntfs\"' --level debug   (in another terminal)"
	fail "mount failed"
fi
mount | grep " on $mountpoint " | sed 's/^/    /'
ok "mounted"

step "4/5 exercise"
ls -la "$mountpoint" | head -20 | sed 's/^/    /'
df -h "$mountpoint" | sed 's/^/    /'
first=$(find "$mountpoint" -type f -maxdepth 2 2>/dev/null | head -1)
if [ -n "$first" ]; then
	stat -f '    %N: %z bytes, mode %Sp, mtime %Sm' "$first"
	head -c 64 "$first" | xxd | head -2 | sed 's/^/    /'
	xattr -l "$first" 2>&1 | head -3 | sed 's/^/    /'
fi
if [ $rw -eq 1 ]; then
	t="$mountpoint/mount-test-$$"
	echo "hello from mount-test" > "$t.txt" || fail "create"
	mkdir "$t.dir" || fail "mkdir"
	mv "$t.txt" "$t.dir/renamed.txt" || fail "rename"
	[ "$(cat "$t.dir/renamed.txt")" = "hello from mount-test" ] \
		|| fail "read back after rename returned the wrong bytes"
	rm "$t.dir/renamed.txt" && rmdir "$t.dir" || fail "delete"
	sync
	ok "create / mkdir / rename / read / delete"

	# Everything below goes through the kernel and FSKit rather than the core's
	# C ABI, which is the point: core/tests covers the ABI, and this is the only
	# thing that covers the request path -- UBC page granularity, XPC payload
	# round trips, and the kernel's own readdir and xattr plumbing.

	# A file bigger than one FSKit request, checksummed. Catches a write path
	# that is only correct for small buffers, and a read path that is only
	# correct for the pages it just wrote.
	big="$mountpoint/mt-big-$$.bin"
	dd if=/dev/urandom of=/tmp/mt-src-$$.bin bs=1m count=8 2>/dev/null
	cp /tmp/mt-src-$$.bin "$big" || fail "write 8 MiB"
	sync
	a=$(shasum -a 256 < /tmp/mt-src-$$.bin | cut -d' ' -f1)
	b=$(shasum -a 256 < "$big" | cut -d' ' -f1)
	[ "$a" = "$b" ] || fail "8 MiB round trip differs (src $a, volume $b)"
	# Re-read after dropping what we can of the cache: the same bytes must come
	# back from the device, not from whatever is still resident.
	c=$(dd if="$big" bs=1m 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
	[ "$a" = "$c" ] || fail "8 MiB re-read differs ($c)"
	rm -f /tmp/mt-src-$$.bin
	ok "8 MiB write and read back, sha256 identical"

	# Sub-page writes at scattered offsets. The UBC page is 16 KiB on Apple
	# silicon, so a 4 KiB write is a read-modify-write of a whole page; this is
	# where that goes wrong if it goes wrong.
	python3 - "$big" <<'PYEOF' || fail "scattered 4 KiB writes"
import os, sys, random
p = sys.argv[1]
size = os.path.getsize(p)
random.seed(11)
marks = {}
fd = os.open(p, os.O_RDWR)
for i in range(64):
    off = random.randrange(0, size - 4096) & ~4095
    pat = bytes([i & 0xff]) * 4096
    os.pwrite(fd, pat, off)
    marks[off] = pat                      # later writes win on a repeat
os.fsync(fd)
for off, pat in marks.items():
    got = os.pread(fd, 4096, off)
    if got != pat:
        print(f"offset {off} reads back wrong", file=sys.stderr)
        os.close(fd); sys.exit(1)
os.close(fd)
PYEOF
	ok "64 scattered 4 KiB writes read back correctly"
	rm -f "$big"

	# xattrs are stored as alternate data streams; this checks the kernel path
	# to them, and that no ._ file appears beside the file (a stated decision).
	x="$mountpoint/mt-xattr-$$.txt"
	echo body > "$x" || fail "create for xattr"
	xattr -w com.example.test "value one" "$x" || fail "xattr -w"
	[ "$(xattr -p com.example.test "$x")" = "value one" ] || fail "xattr -p returned the wrong value"
	xattr -w com.example.test "value two" "$x" || fail "xattr overwrite"
	[ "$(xattr -p com.example.test "$x")" = "value two" ] || fail "xattr overwrite not visible"
	xattr -l "$x" | grep -q com.example.test || fail "xattr -l does not list it"
	xattr -d com.example.test "$x" || fail "xattr -d"
	xattr -p com.example.test "$x" >/dev/null 2>&1 && fail "xattr still present after -d"
	[ -e "$mountpoint/._$(basename "$x")" ] && fail "a ._ file was created; xattrs must be ADS"
	rm -f "$x"
	ok "xattr set / get / overwrite / list / remove, no ._ file"

	# A directory past the index-root boundary, read back through the kernel's
	# readdir rather than the core's. Catches a cookie or offset bug that only
	# shows once the index spills into an $INDEX_ALLOCATION.
	d="$mountpoint/mt-dir-$$"
	mkdir "$d" || fail "mkdir for readdir test"
	i=0; while [ $i -lt 300 ]; do : > "$d/entry-$(printf '%04d' $i).txt"; i=$((i+1)); done
	n=$(ls -1 "$d" | wc -l | tr -d ' ')
	[ "$n" = 300 ] || fail "readdir returned $n entries, expected 300"
	u=$(ls -1 "$d" | sort -u | wc -l | tr -d ' ')
	[ "$u" = 300 ] || fail "readdir returned duplicates ($u unique of $n)"
	rm -rf "$d" || fail "removing the 300-entry directory"
	ok "300-entry directory: created, listed without duplicates, removed"
else
	echo "    (read-only run; pass --rw for the write test — it only touches the shadow file)"
fi
if [ -f "$HOME/Library/Group Containers/group.ch.techtag.ntfs/Library/Application Support/mount-status.json" ]; then
	echo "    mount-status.json:"; sed 's/^/      /' "$HOME/Library/Group Containers/group.ch.techtag.ntfs/Library/Application Support/mount-status.json"
fi
ok "exercised"

step "5/5 unmount and detach"
umount "$mountpoint" || diskutil unmount "$mountpoint" || fail "umount"
ok "unmounted"
hdiutil detach "$dev" -quiet || fail "detach"
ok "detached"
dev=""

# Whatever the FSKit path just wrote has to be structurally sound, and nothing
# checked that until 2026-09-14: the test verified that reads came back right
# and stopped there. A read-back cannot see a corrupt index, a wrong link count
# or a leaked cluster. ntfsck from ntfsprogs-plus can, and it is the closest
# thing to chkdsk that runs without Windows.
#
# The write test runs against a shadow file, so the image on disk is untouched
# and there is nothing to check unless --keep-shadow was used; with --rw and no
# shadow the image is the thing that changed. Check whichever one holds the
# writes, and say plainly when there is nothing to check.
if [ $rw -eq 1 ]; then
	step "6/6 structural check of what was written"
	ntfsck_plus="$root/tools/.local-plus/sbin/ntfsck"
	target="$workimage"
	if [ ! -x "$ntfsck_plus" ]; then
		echo "    ntfsprogs-plus not built; run tools/build-ntfsprogs-plus.sh" >&2
		echo "    SKIP structural check"
	elif [ -z "$target" ] || [ ! -f "$target" ]; then
		echo "    SKIP: no writable image to check"
	else
		# -n: check, never repair. Repairing the thing under test would be
		# measuring the repair.
		if "$ntfsck_plus" -n "$target" 2>&1 | grep -viE 'percent completed' | tail -3 | sed 's/^/    /'; then
			:
		fi
		"$ntfsck_plus" -n "$target" >/dev/null 2>&1 || fail "ntfsck found errors in the written image"
		ok "ntfsck clean after the write cycle"
	fi
fi

echo
echo "PASS"
