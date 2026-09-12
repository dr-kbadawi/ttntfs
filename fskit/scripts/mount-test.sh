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
#   3. mount -F -t ttntfs <dev> <mountpoint>    (sudo)
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
cleanup() {
	[ $keep -eq 1 ] && { echo "    --keep: leaving $dev attached${mountpoint:+ at $mountpoint}"; return; }
	if [ -n "$mountpoint" ] && mount | grep -q " on $mountpoint "; then
		sudo umount "$mountpoint" 2>/dev/null || sudo diskutil unmount force "$mountpoint" >/dev/null 2>&1
	fi
	[ -n "$dev" ] && hdiutil detach "$dev" -quiet 2>/dev/null
	[ -n "$shadow" ] && rm -f "$shadow"
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

step "2/5 attach $image (nomount, shadow)"
[ -f "$image" ] || fail "image not found: $image (run: make -C tools/mkfixtures fixtures)"
shadow=$(mktemp -t ntfs-shadow).shadow
attach=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage -shadow "$shadow" "$image" 2>&1) \
	|| fail "hdiutil attach: $attach"
dev=$(echo "$attach" | awk '/^\/dev\/disk/ {print $1; exit}')
[ -n "$dev" ] || fail "no device in hdiutil output: $attach"
ok "attached as $dev (raw image, whole device holds the volume)"
diskutil info "$dev" | grep -E 'Device Node|Media Name|Disk Size|Device Block Size' | sed 's/^/    /'

step "3/5 mount -F -t $fstype $dev"
[ -n "$mountpoint" ] || mountpoint=$(mktemp -d /tmp/ttntfs-mount.XXXXXX)
opts=""
[ $rw -eq 1 ] || opts="-o ro"
echo "    sudo mount -F -t $fstype $opts $dev $mountpoint"
if ! out=$(sudo mount -F -t "$fstype" $opts "$dev" "$mountpoint" 2>&1); then
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
	cat "$t.dir/renamed.txt" | sed 's/^/    read back: /'
	rm "$t.dir/renamed.txt" && rmdir "$t.dir" || fail "delete"
	sync
	ok "create / mkdir / rename / read / delete"
else
	echo "    (read-only run; pass --rw for the write test — it only touches the shadow file)"
fi
if [ -f "$HOME/Library/Group Containers/group.ch.techtag.ntfs/Library/Application Support/mount-status.json" ]; then
	echo "    mount-status.json:"; sed 's/^/      /' "$HOME/Library/Group Containers/group.ch.techtag.ntfs/Library/Application Support/mount-status.json"
fi
ok "exercised"

step "5/5 unmount and detach"
sudo umount "$mountpoint" || fail "umount"
ok "unmounted"
hdiutil detach "$dev" -quiet || fail "detach"
ok "detached"
dev=""
echo
echo "PASS"
