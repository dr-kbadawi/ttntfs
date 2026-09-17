#!/usr/bin/env python3
"""Byte-patch a $REPARSE_POINT attribute into an existing directory MFT record.

Why this exists: Windows makes directory symlinks (mklink /D) and junctions
(mklink /J) as a directory record carrying a reparse point, and nothing on
macOS can create one. This driver deliberately does not write junctions, and
until 2026-09-17 could not even read them -- every one showed as an empty
folder. To test the read path without a Windows round trip, this script turns
an ordinary directory on an image into one of those records, byte for byte:
it inserts the resident $REPARSE_POINT in attribute order, sets
FILE_ATTR_REPARSE_POINT in $STANDARD_INFORMATION, in the record's $FILE_NAME
and in the parent's index entry, puts the tag in the $FILE_NAME union, and
redoes the update-sequence fixups. ntfsinfo decodes the result and
ntfsprogs-plus ntfsck passes it. It does NOT add the $Extend/$Reparse index
entry, which ntfsck does not cross-check.

core/tests/test_links.c drives it (test_windows_dir_links).

Emulates what Windows leaves on disk after `mklink /J`, `mklink /D` or
`mountvol`: the directory record keeps MFT_RECORD_IS_DIRECTORY and its empty
$INDEX_ROOT, gains a resident $REPARSE_POINT (0xC0), and FILE_ATTR_REPARSE_POINT
(0x400) is set in $STANDARD_INFORMATION and $FILE_NAME (with the tag in the
$FILE_NAME dup-info union). The parent's index entry copy is patched too.

Not done: the $Extend/$Reparse index entry (chkdsk would want it; our reader
does not).

usage: patch_reparse.py IMG MFTNO KIND TARGET [PRINT]
  KIND = junction | symlink-abs | symlink-rel | volume
"""
import struct, sys, uuid

IO_REPARSE_TAG_MOUNT_POINT = 0xA0000003
IO_REPARSE_TAG_SYMLINK     = 0xA000000C
FILE_ATTR_REPARSE_POINT    = 0x400
AT_STANDARD_INFORMATION = 0x10
AT_FILE_NAME            = 0x30
AT_REPARSE_POINT        = 0xC0
AT_INDEX_ROOT           = 0x90
AT_INDEX_ALLOCATION     = 0xA0
AT_END                  = 0xFFFFFFFF


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]


def fixup_undo(rec, sector=512):
    usa_ofs, usa_cnt = u16(rec, 4), u16(rec, 6)
    usn = rec[usa_ofs:usa_ofs + 2]
    for i in range(1, usa_cnt):
        pos = i * sector - 2
        assert rec[pos:pos + 2] == usn, "fixup mismatch at %d" % pos
        rec[pos:pos + 2] = rec[usa_ofs + 2 * i:usa_ofs + 2 * i + 2]


def fixup_apply(rec, sector=512, bump=True):
    usa_ofs, usa_cnt = u16(rec, 4), u16(rec, 6)
    usn = u16(rec, usa_ofs)
    if bump:
        usn = (usn + 1) & 0xFFFF
        if usn == 0:
            usn = 1
    struct.pack_into('<H', rec, usa_ofs, usn)
    for i in range(1, usa_cnt):
        pos = i * sector - 2
        rec[usa_ofs + 2 * i:usa_ofs + 2 * i + 2] = rec[pos:pos + 2]
        struct.pack_into('<H', rec, pos, usn)


class Vol:
    def __init__(self, path):
        self.f = open(path, 'r+b')
        bs = self.f.read(512)
        self.bps = u16(bs, 0x0B)
        spc = bs[0x0D]
        self.cs = self.bps * spc
        self.mft_lcn = u64(bs, 0x30)
        cpr = struct.unpack_from('<b', bs, 0x40)[0]
        self.mft_rec = (1 << -cpr) if cpr < 0 else cpr * self.cs
        cpi = struct.unpack_from('<b', bs, 0x44)[0]
        self.idx_blk = (1 << -cpi) if cpi < 0 else cpi * self.cs
        # runlist of $MFT
        self.mft_runs = self.runlist_of(self.read_mft_raw(0), 0x80)

    def read_mft_raw(self, n):
        # only handles the first extent of $MFT for records we care about
        if hasattr(self, 'mft_runs'):
            off = self.vcn_to_off(self.mft_runs, n * self.mft_rec)
        else:
            off = self.mft_lcn * self.cs + n * self.mft_rec
        self.f.seek(off)
        return bytearray(self.f.read(self.mft_rec)), off

    def vcn_to_off(self, runs, byte_off):
        for vcn, lcn, length in runs:
            lo, hi = vcn * self.cs, (vcn + length) * self.cs
            if lo <= byte_off < hi:
                return lcn * self.cs + (byte_off - lo)
        raise RuntimeError("byte offset not mapped")

    def attrs(self, rec):
        o = u16(rec, 0x14)
        while u32(rec, o) != AT_END:
            yield o, u32(rec, o), u32(rec, o + 4)
            o += u32(rec, o + 4)
        yield o, AT_END, 8

    def runlist_of(self, recoff, atype, undone=False):
        rec = bytearray(recoff[0])
        if not undone:
            fixup_undo(rec)
        for o, t, l in self.attrs(rec):
            if t == atype and rec[o + 8]:
                mp = u16(rec, o + 0x20)
                p = o + mp
                runs, vcn, lcn = [], 0, 0
                while rec[p]:
                    h = rec[p]; ls, os_ = h & 15, h >> 4
                    length = int.from_bytes(rec[p + 1:p + 1 + ls], 'little')
                    d = int.from_bytes(rec[p + 1 + ls:p + 1 + ls + os_], 'little', signed=True)
                    lcn += d
                    runs.append((vcn, lcn, length)); vcn += length
                    p += 1 + ls + os_
                return runs
        raise RuntimeError("no non-resident attr 0x%x" % atype)


def build_payload(kind, target, print_name):
    subst = target.encode('utf-16-le')
    prt = print_name.encode('utf-16-le')
    # Windows NUL-terminates both strings; lengths exclude the NULs (ReactOS mklink.c, [MS-FSCC] 2.1.2.5)
    pathbuf = subst + b'\0\0' + prt + b'\0\0'
    so, sl, po, pl = 0, len(subst), len(subst) + 2, len(prt)
    if kind == 'junction' or kind == 'volume':
        data = struct.pack('<HHHH', so, sl, po, pl) + pathbuf
        tag = IO_REPARSE_TAG_MOUNT_POINT
    else:
        flags = 1 if kind == 'symlink-rel' else 0
        data = struct.pack('<HHHHI', so, sl, po, pl, flags) + pathbuf
        tag = IO_REPARSE_TAG_SYMLINK
    return tag, struct.pack('<IHH', tag, len(data), 0) + data


def patch_record(vol, mftno, tag, value):
    rec, off = vol.read_mft_raw(mftno)
    assert rec[:4] == b'FILE'
    fixup_undo(rec)
    assert u16(rec, 0x16) & 3 == 3, "record must be IN_USE | DIRECTORY"
    used, alloc = u32(rec, 0x18), u32(rec, 0x1C)
    inst = u16(rec, 0x28)
    # locate insertion point (attrs sorted by type) and patch SI / FN
    insert_at = None
    fn_name = None
    parent = None
    for o, t, l in vol.attrs(rec):
        vo = u16(rec, o + 0x14)
        if t == AT_STANDARD_INFORMATION:
            fa = u32(rec, o + vo + 0x20) | FILE_ATTR_REPARSE_POINT
            struct.pack_into('<I', rec, o + vo + 0x20, fa)
        elif t == AT_FILE_NAME:
            fa = u32(rec, o + vo + 0x38) | FILE_ATTR_REPARSE_POINT
            struct.pack_into('<I', rec, o + vo + 0x38, fa)
            struct.pack_into('<I', rec, o + vo + 0x3C, tag)
            nlen = rec[o + vo + 0x40]
            fn_name = bytes(rec[o + vo + 0x42:o + vo + 0x42 + 2 * nlen])
            parent = u64(rec, o + vo) & 0xFFFFFFFFFFFF
        if insert_at is None and t > AT_REPARSE_POINT:
            insert_at = o
    assert insert_at is not None
    alen = (0x18 + len(value) + 7) & ~7
    hdr = struct.pack('<IIBBHHHIHBB', AT_REPARSE_POINT, alen, 0, 0, 0x18, 0, inst,
                      len(value), 0x18, 0, 0)
    attr = hdr + value + b'\0' * (alen - 0x18 - len(value))
    assert used + alen <= alloc, "record full"
    tail = rec[insert_at:used]
    rec[insert_at:insert_at + alen] = attr
    rec[insert_at + alen:insert_at + alen + len(tail)] = tail
    struct.pack_into('<I', rec, 0x18, used + alen)
    struct.pack_into('<H', rec, 0x28, inst + 1)
    fixup_apply(rec)
    vol.f.seek(off); vol.f.write(rec)
    return fn_name, parent


def patch_index_entry(vol, parent, fn_name, mftno, tag):
    """Find the parent's index entry for (mftno, name) and set the dup flags."""
    rec, _ = vol.read_mft_raw(parent)
    fixup_undo(rec)
    def scan_entries(buf, base, end, writeback):
        p = base
        while p < end:
            elen = u16(buf, p + 8)
            klen = u16(buf, p + 10)
            flags = u16(buf, p + 12)
            if flags & 2:  # END
                return False
            mref = u64(buf, p) & 0xFFFFFFFFFFFF
            fn = p + 16
            nlen = buf[fn + 0x40]
            name = bytes(buf[fn + 0x42:fn + 0x42 + 2 * nlen])
            if mref == mftno and name == fn_name:
                fa = u32(buf, fn + 0x38) | FILE_ATTR_REPARSE_POINT
                struct.pack_into('<I', buf, fn + 0x38, fa)
                struct.pack_into('<I', buf, fn + 0x3C, tag)
                writeback()
                return True
            p += elen
        return False
    for o, t, l in vol.attrs(rec):
        if t == AT_INDEX_ROOT and rec[o + 9] == 4:
            vo = u16(rec, o + 0x14)
            ir = o + vo
            ih = ir + 16
            eo = u32(rec, ih + 0)
            il = u32(rec, ih + 4)
            found = scan_entries(rec, ih + eo, ih + il, lambda: None)
            if found:
                fixup_apply(rec)
                _, off = vol.read_mft_raw(parent)
                vol.f.seek(off); vol.f.write(rec)
                return 'root'
        if t == AT_INDEX_ALLOCATION and rec[o + 9] == 4:
            runs = vol.runlist_of((bytes(rec), 0), AT_INDEX_ALLOCATION, undone=True)
            size = u64(rec, o + 0x30)
            for boff in range(0, size, vol.idx_blk):
                doff = vol.vcn_to_off(runs, boff)
                vol.f.seek(doff)
                blk = bytearray(vol.f.read(vol.idx_blk))
                if blk[:4] != b'INDX':
                    continue
                fixup_undo(blk)
                ih = 0x18
                eo = u32(blk, ih + 0)
                il = u32(blk, ih + 4)
                def wb():
                    fixup_apply(blk)
                    vol.f.seek(doff); vol.f.write(blk)
                if scan_entries(blk, ih + eo, ih + il, wb):
                    return 'block@%d' % boff
    return None


def main():
    img, mftno, kind, target = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    prt = sys.argv[5] if len(sys.argv) > 5 else None
    if prt is None:
        if kind == 'volume':
            prt = ''
        elif target.startswith('\\??\\'):
            prt = target[4:]
        else:
            prt = target
    vol = Vol(img)
    tag, value = build_payload(kind, target, prt)
    fn_name, parent = patch_record(vol, mftno, tag, value)
    where = patch_index_entry(vol, parent, fn_name, mftno, tag)
    vol.f.flush()
    print("patched mft %d: tag 0x%08X subst=%r print=%r (%d bytes); index entry: %s"
          % (mftno, tag, target, prt, len(value), where))


if __name__ == '__main__':
    main()
