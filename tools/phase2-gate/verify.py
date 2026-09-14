#!/usr/bin/env python3
"""Verify the write-test tree against its manifest. Run before and after the
Windows round trip: the point is that the two runs agree."""
import os, sys, hashlib, json, ctypes, ctypes.util

ROOT, MF = sys.argv[1], sys.argv[2]
BASE = os.path.join(ROOT, "ttntfs-writetest")
m = json.load(open(MF))
_libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)

def getxattr(path, name):
    n = _libc.getxattr(path.encode(), name.encode(), None, ctypes.c_size_t(0),
                       ctypes.c_uint32(0), ctypes.c_int(0))
    if n < 0: raise OSError(ctypes.get_errno(), f"getxattr {name}")
    buf = ctypes.create_string_buffer(n)
    _libc.getxattr(path.encode(), name.encode(), buf, ctypes.c_size_t(n),
                   ctypes.c_uint32(0), ctypes.c_int(0))
    return buf.raw[:n]

bad, checked = [], 0
for rel, exp in sorted(m["manifest"].items()):
    p = os.path.join(BASE, rel)
    try:
        with open(p, "rb") as f: d = f.read()
    except OSError as e:
        bad.append(f"unreadable {rel}: {e}"); continue
    checked += 1
    if len(d) != exp["size"]:
        bad.append(f"size {rel}: {len(d)} != {exp['size']}")
    elif hashlib.sha256(d).hexdigest() != exp["sha256"]:
        bad.append(f"CONTENT {rel}")
print(f"  files: {checked}/{len(m['manifest'])} readable, {len(bad)} problems")

for rel, attrs in sorted(m["xattrs"].items()):
    p = os.path.join(BASE, rel)
    for k, h in attrs.items():
        try:
            if hashlib.sha256(getxattr(p, k)).hexdigest() != h:
                bad.append(f"xattr {k} on {rel}")
        except OSError as e:
            bad.append(f"xattr {k} on {rel}: {e}")
print(f"  xattrs: {sum(len(a) for a in m['xattrs'].values())} checked")

g = os.path.join(BASE, "G-links")
nl = os.stat(os.path.join(g, "link-target.txt")).st_nlink
if nl != 4: bad.append(f"hard link count is {nl}, expected 4")
print(f"  hard links: nlink={nl} (expected 4)")
for s, want in (("symlink-relative", "link-target.txt"),
                ("symlink-absolute", "/Volumes/Volume/ttntfs-writetest/G-links/link-target.txt")):
    try:
        got = os.readlink(os.path.join(g, s))
        if got != want: bad.append(f"symlink {s} -> {got!r}, expected {want!r}")
    except OSError as e:
        bad.append(f"symlink {s}: {e}")
print("  symlinks: 2 checked")

n = len(os.listdir(os.path.join(BASE, "D-manyentries")))
if n != 500: bad.append(f"D-manyentries has {n} entries, expected 500")
k = len(os.listdir(os.path.join(BASE, "H-churn")))
if k != 100: bad.append(f"H-churn has {k} entries, expected 100")
print(f"  directories: D={n} (expect 500), H-churn={k} (expect 100)")

print()
if bad:
    print(f"  *** {len(bad)} PROBLEMS ***")
    for b in bad[:20]: print("   ", b)
    sys.exit(1)
print("  ALL VERIFIED")
