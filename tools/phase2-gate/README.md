# Phase 2 gate: does Windows agree with what we wrote?

`docs/PORTING.md` phase 2 is done when "`chkdsk /f` reports clean after every
test on a Windows VM". Nothing else in this repository can settle it: ntfsprogs
and ntfsprogs-plus are good structural checkers and they run on every commit,
but Windows defines NTFS, so agreement with a third party is evidence and not
proof.

## Procedure

```
# 1. on the Mac, with the volume mounted read-write by THIS driver
python3 tools/phase2-gate/write.py /Volumes/<name> tools/phase2-gate/manifest.json
python3 tools/phase2-gate/verify.py /Volumes/<name> tools/phase2-gate/manifest.json

# 2. unmount cleanly, confirm the journal is clean, eject, move the disk
diskutil unmount /dev/diskNsM

# 3. on Windows, the repair-capable form, not scan mode
chkdsk /f E:

# 4. back on the Mac
python3 tools/phase2-gate/verify.py /Volumes/<name> tools/phase2-gate/manifest.json
```

The gate passes when step 3 reports no problems **and** step 4 matches step 1.
Step 4 matters as much as step 3: chkdsk repairs what it finds, so a volume it
had to fix can come back "clean" with our data altered.

## What the tree exercises, and why

Each group targets something chkdsk actually verifies, so that a failure names
the area rather than just the volume.

| Group | Exercises | chkdsk stage |
|---|---|---|
| `A-sizes` | 0 B to 1 MiB, across the resident/non-resident boundary | file records |
| `B-large` | 3 x 200 MiB on a nearly full disk, so runlists fragment | file records |
| `C-deep` | 8 nested directories | index/name linkage |
| `D-manyentries` | 500 entries, a multi-level index B-tree | index/name linkage |
| `E-names` | 255 UTF-16 units, CJK, NFC vs NFD, emoji, case | index/name linkage |
| `F-xattr` | xattrs as alternate data streams, one 8 KB | data attributes |
| `G-links` | 3 hard links (nlink 4) and 2 symlinks | link counts, reparse points |
| `H-churn` | 200 created, 100 deleted, 100 renamed | cluster bitmap, index removal |

Names are all legal on Windows on purpose: the volume has to open there, and
this driver refuses illegal names by default, so a refusal during step 1 is
itself a finding.

## Notes

`os.setxattr` is Linux-only and macOS `xattr(1)` cannot take a value from a
file, so `write.py` calls `setxattr(2)` through ctypes.

A manifest from a real run is kept here (`manifest-2026-09-14.json`, 626 files,
608 MiB) so a later run can be compared against an earlier disk.
