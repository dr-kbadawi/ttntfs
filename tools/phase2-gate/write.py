#!/usr/bin/env python3
"""Phase 2 gate: write a varied tree with our driver, then let Windows judge it.

Every group here targets something chkdsk actually verifies -- file records,
index/name linkage, reparse points, security descriptors, the cluster bitmap.
Names are all legal on Windows on purpose: the volume has to be openable there,
and our driver refuses illegal ones, so a rejection here is itself a finding.
"""
import os, sys, hashlib, json, random, shutil, unicodedata, ctypes, ctypes.util

# os.setxattr is Linux-only, and macOS xattr(1) cannot take a value from a file,
# so call the libc entry point: setxattr(path, name, value, size, position, options)
_libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
def setxattr(path, name, value):
    rc = _libc.setxattr(path.encode(), name.encode(), value, ctypes.c_size_t(len(value)),
                        ctypes.c_uint32(0), ctypes.c_int(0))
    if rc != 0:
        raise OSError(ctypes.get_errno(), f"setxattr {name} on {path}")

ROOT = sys.argv[1]
BASE = os.path.join(ROOT, "ttntfs-writetest")
random.seed(20260914)
manifest, notes = {}, []

def blob(n, seed):
    r = random.Random(seed)
    return bytes(r.getrandbits(8) for _ in range(min(n, 4096))) * (n // 4096 + 1)

def put(path, data):
    with open(path, "wb") as f:
        f.write(data)
    manifest[os.path.relpath(path, BASE)] = {
        "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}

def note(s):
    notes.append(s); print("  " + s)

shutil.rmtree(BASE, ignore_errors=True)
os.makedirs(BASE)

# A. resident and non-resident boundary. NTFS keeps small files inside the MFT
#    record; the crossover is what chkdsk's file-record stage looks at.
d = os.path.join(BASE, "A-sizes"); os.makedirs(d)
for n in (0, 1, 100, 700, 1024, 1500, 4096, 65536, 1 << 20):
    put(os.path.join(d, f"size-{n}.bin"), blob(n, n))
note(f"A-sizes: 9 files, 0 B to 1 MiB (resident/non-resident crossover)")

# B. large files: multi-extent runlists on a 98%-full disk, so the allocator
#    has to work for them rather than taking one clean run.
d = os.path.join(BASE, "B-large"); os.makedirs(d)
for i in range(3):
    put(os.path.join(d, f"large-{i}.bin"), blob(200 << 20, 900 + i))
note("B-large: 3 x 200 MiB (fragmented runlists on a full disk)")

# C. deep nesting: parent/child index updates all the way down.
p = os.path.join(BASE, "C-deep"); os.makedirs(p)
for i in range(8):
    p = os.path.join(p, f"level-{i}"); os.makedirs(p)
put(os.path.join(p, "bottom.txt"), b"eight levels down\n")
note("C-deep: 8 nested directories")

# D. past the index-root boundary, which this driver crosses at the 4th entry,
#    so this is a real B-tree with an $INDEX_ALLOCATION and several levels.
d = os.path.join(BASE, "D-manyentries"); os.makedirs(d)
for i in range(500):
    put(os.path.join(d, f"entry-{i:04d}.txt"), f"entry {i}\n".encode())
note("D-manyentries: 500 entries (multi-level index B-tree)")

# E. names. 255 UTF-16 units is the NTFS limit; surrogate pairs count as two.
#    NFC and NFD are different byte sequences and must stay distinct -- this
#    driver stores names verbatim rather than normalising.
d = os.path.join(BASE, "E-names"); os.makedirs(d)
cases = {
    "max-length": "n" * 250 + ".txt",
    "cjk": "中文文件名-テスト-한국어.txt",
    "accented-nfc": unicodedata.normalize("NFC", "café-résumé.txt"),
    "accented-nfd": unicodedata.normalize("NFD", "café-résumé.txt"),
    "emoji": "test-\U0001F600-\U0001F4BE.txt",
    "spaces and dots": "a file. with dots and spaces.txt",
    "mixed-CASE": "MiXeD-CaSe-Name.TXT",
}
for k, name in cases.items():
    try:
        put(os.path.join(d, name), f"{k}\n".encode())
    except OSError as e:
        note(f"E-names: REFUSED {k!r}: {e}")
note(f"E-names: {len(cases)} name shapes (255-unit, CJK, NFC vs NFD, emoji, case)")

# F. xattrs are stored as alternate data streams, and chkdsk checks those.
d = os.path.join(BASE, "F-xattr"); os.makedirs(d)
xa = {}
for i in range(5):
    fp = os.path.join(d, f"tagged-{i}.txt")
    put(fp, f"body {i}\n".encode())
    vals = {"user.comment": f"comment {i}".encode(),
            "user.big": bytes(random.Random(i).getrandbits(8) for _ in range(8000))}
    for k, v in vals.items():
        setxattr(fp, k, v)
    xa[os.path.relpath(fp, BASE)] = {k: hashlib.sha256(v).hexdigest() for k, v in vals.items()}
note("F-xattr: 5 files with 2 xattrs each, one 8 KB (non-resident stream)")

# G. link counts and reparse points: both are their own chkdsk stage, and a
#    wrong link count is exactly the silent damage it exists to catch.
d = os.path.join(BASE, "G-links"); os.makedirs(d)
tgt = os.path.join(d, "link-target.txt")
put(tgt, b"the target\n")
for i in range(3):
    os.link(tgt, os.path.join(d, f"hardlink-{i}.txt"))
os.symlink("link-target.txt", os.path.join(d, "symlink-relative"))
os.symlink("/Volumes/Volume/ttntfs-writetest/G-links/link-target.txt",
           os.path.join(d, "symlink-absolute"))
note("G-links: 1 file + 3 hard links (nlink 4), 2 symlinks (reparse points)")

# H. churn: make the allocator reuse space and the index remove nodes, so the
#    cluster bitmap and the B-tree are not merely grown but exercised.
d = os.path.join(BASE, "H-churn"); os.makedirs(d)
tmp = [os.path.join(d, f"scratch-{i:03d}.bin") for i in range(200)]
for i, fp in enumerate(tmp):
    with open(fp, "wb") as f:
        f.write(blob(64 << 10, 5000 + i))
for fp in tmp[::2]:                      # delete every other one
    os.unlink(fp)
for fp in tmp[1::2]:                     # rename the survivors
    os.rename(fp, fp.replace("scratch-", "kept-"))
for fp in tmp[1::2]:
    p = fp.replace("scratch-", "kept-")
    with open(p, "rb") as f:
        data = f.read()
    manifest[os.path.relpath(p, BASE)] = {
        "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
note("H-churn: 200 created, 100 deleted, 100 renamed (bitmap + index removal)")

out = {"manifest": manifest, "xattrs": xa, "notes": notes,
       "files": len(manifest), "bytes": sum(v["size"] for v in manifest.values())}
with open(sys.argv[2], "w") as f:
    json.dump(out, f, indent=1, sort_keys=True)
print(f"\n  {out['files']} files recorded, {out['bytes']/(1<<20):.1f} MiB")
