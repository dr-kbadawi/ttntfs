#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run the ntfslog analyzer over every fixture image and require each one to
# be clean for a read-write mount. Exit 77 (ctest SKIP_RETURN_CODE) when the
# tools stream has not generated the images.
#
# usage: run_images.sh <ntfslog binary> [images dir]   (default: $NTFS_IMAGES)
set -u
bin=$1
dir=${2:-${NTFS_IMAGES:-}}
if [ -z "$dir" ] || [ ! -d "$dir" ]; then
	echo "no fixture images at '$dir' (skipped)"
	exit 77
fi
n=0
rc=0
for f in "$dir"/*.img; do
	[ -e "$f" ] || continue
	n=$((n + 1))
	out=$("$bin" --no-plan "$f" 2>&1) || rc=1
	if echo "$out" | grep -q '^Clean for read-write mount: yes'; then
		echo "ok   $f: $(echo "$out" | grep '^State:')"
	else
		echo "FAIL $f"
		echo "$out"
		rc=1
	fi
done
if [ "$n" -eq 0 ]; then
	echo "no *.img in '$dir' (skipped)"
	exit 77
fi
echo "$n image(s) analyzed"
exit $rc
