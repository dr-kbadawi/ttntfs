#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Generate the synthetic full-index fixture on macOS. Requires the source and
# library produced by tools/build-ntfsprogs.sh (ntfs-3g 2026.7.7).
set -eu
here=$(CDPATH='' cd "$(dirname "$0")" && pwd)
tools="$here/../../../tools/.local"
src="$tools/src/ntfs-3g-2026.7.7"
if [ "$#" -ne 1 ] || [ -e "$1" ] || [ -L "$1" ]; then
    echo "usage: $0 NEW_IMAGE.img.bz2" >&2
    exit 2
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# The fixture writer bypasses only libntfs-3g's 120-byte headroom policy.
# The hard MFT record size check remains in force.
python3 - "$src/libntfs-3g/attrib.c" "$tmp/attrib.c" <<'PY'
import sys
from pathlib import Path
src, out = map(Path, sys.argv[1:])
guard = ("if (a->type == AT_INDEX_ROOT && new_size > attr_size &&\n"
         "\t\t    new_muse + 120 > alloc_size && old_size + 120 <= alloc_size)")
text = src.read_text()
if text.count(guard) != 1:
    sys.exit("unexpected ntfs-3g index headroom check")
out.write_text(text.replace(guard, "if (0)"))
PY
cc -DHAVE_CONFIG_H -I"$src" -I"$src/include/ntfs-3g" -O2 \
    -c "$tmp/attrib.c" -o "$tmp/libntfs_3g_la-attrib.o"
cp "$tools/lib/libntfs-3g.a" "$tmp/libfixture.a"
ar -r "$tmp/libfixture.a" "$tmp/libntfs_3g_la-attrib.o"
ranlib "$tmp/libfixture.a"

cat >"$tmp/write-fixture.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/unistr.h>

static ntfs_inode *create(ntfs_inode *parent, const char *name, mode_t type)
{
    ntfschar *wide = NULL;
    int len = ntfs_mbstoucs(name, &wide);
    if (len < 1 || len > 255)
        exit(1);
    ntfs_inode *node = ntfs_create(parent, const_cpu_to_le32(0), wide, (u8)len, type);
    free(wide);
    if (!node) {
        perror("ntfs_create");
        exit(1);
    }
    return node;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    ntfs_volume *vol = ntfs_mount(argv[1], NTFS_MNT_NONE);
    if (!vol)
        return 1;
    ntfs_inode *root = ntfs_pathname_to_inode(vol, NULL, "/");
    if (!root)
        return 1;
    ntfs_inode *dir = create(root, "lab", S_IFDIR);
    if (ntfs_attr_remove(dir, AT_SECURITY_DESCRIPTOR, AT_UNNAMED, 0))
        return 1;
    for (int i = 0; i < 7; i++) {
        char name[16];
        snprintf(name, sizeof(name), "%03d%s", i, i < 4 ? "xxxx" : "xxxxxxxx");
        if (ntfs_inode_close_in_dir(create(dir, name, S_IFREG), dir))
            return 1;
    }
    return ntfs_inode_close_in_dir(dir, root) ||
        ntfs_inode_close(root) || ntfs_umount(vol, FALSE);
}
C
cc -I"$tools/include" "$tmp/write-fixture.c" "$tmp/libfixture.a" -o "$tmp/write-fixture"
# mkntfs reserves the last sector. Give it just over its 1 MiB minimum.
mkfile -n 1025k "$tmp/fixture.img"
"$tools/sbin/mkntfs" -Q -F -c 4096 -L EMPTY "$tmp/fixture.img" >"$tmp/mkntfs.log" 2>&1 ||
    { cat "$tmp/mkntfs.log" >&2; exit 1; }
"$tmp/write-fixture" "$tmp/fixture.img"
"$here/../../../tools/.local-plus/sbin/ntfsck" -n "$tmp/fixture.img" >"$tmp/fsck.log" 2>&1 ||
    { tail -8 "$tmp/fsck.log" >&2; exit 1; }
bzip2 -9c "$tmp/fixture.img" >"$1"
