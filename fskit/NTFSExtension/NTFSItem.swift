// SPDX-License-Identifier: GPL-2.0

import Foundation
import FSKit

/// An FSItem backed by a referenced core inode (`ntfs_inode_t *`).
/// The reference is dropped in `NTFSVolume.reclaimItem`.
final class NTFSItem: FSItem {
    let inode: OpaquePointer            // ntfs_inode_t *
    let inodeNumber: UInt64
    let id: FSItem.Identifier
    var parentID: FSItem.Identifier
    var itemType: FSItem.ItemType
    /// Bumped on every namespace change inside this directory; it is the
    /// enumeration verifier (must be nonzero, so it starts at 1).
    var dirVersion: UInt64 = 1
    /// Set by write/truncate; cleared by the fsync on last close.
    var dirty = false
    var openCount = 0

    init(inode: OpaquePointer, parentID: FSItem.Identifier, type: FSItem.ItemType) {
        self.inode = inode
        self.inodeNumber = ntfs_inode_number(inode)
        self.id = NTFSItem.identifier(forInode: inodeNumber)
        self.parentID = parentID
        self.itemType = type
        super.init()
    }

    var isRoot: Bool { id == .rootDirectory }

    /// NTFS root is MFT record 5; FSKit wants the root's ID to be `.rootDirectory` (2).
    /// MFT records 1 and 2 ($MFTMirr, $LogFile) are only visible with `showsystem`;
    /// they are moved out of the way so IDs never collide with FSKit's reserved
    /// values (0 invalid, 1 parent-of-root, 2 root).
    static let rootInode: UInt64 = 5
    static let relocatedBit: UInt64 = 1 << 62
    static func identifier(forInode ino: UInt64) -> FSItem.Identifier {
        if ino == rootInode { return .rootDirectory }
        if ino <= 2 { return FSItem.Identifier(rawValue: ino | relocatedBit)! }
        return FSItem.Identifier(rawValue: ino)!
    }
    static func inode(forIdentifier id: FSItem.Identifier) -> UInt64 {
        if id == .rootDirectory { return rootInode }
        return id.rawValue & ~relocatedBit
    }
}

extension FSItem.ItemType {
    init(coreType: Int32) {
        switch coreType {
        case Int32(NTFS_ITEM_DIR.rawValue): self = .directory
        case Int32(NTFS_ITEM_SYMLINK.rawValue): self = .symlink
        default: self = .file            // NTFS_ITEM_FILE and NTFS_ITEM_OTHER (opaque reparse point)
        }
    }
}

extension timespec {
    init(_ t: ntfs_timespec) { self.init(tv_sec: Int(t.sec), tv_nsec: Int(t.nsec)) }
}

extension ntfs_timespec {
    init(_ t: timespec) { self.init(sec: Int64(t.tv_sec), nsec: Int32(t.tv_nsec)) }
}

/// BSD st_flags derived from NTFS FILE_ATTR_* bits. Only UF_HIDDEN is
/// round-tripped (Finder's "hidden"); the Windows read-only attribute is
/// reported as nothing because UF_IMMUTABLE would also stop deletion, which
/// is not what the Windows bit means.
func bsdFlags(fromFileAttributes fa: UInt32) -> UInt32 {
    var flags: UInt32 = 0
    if fa & NTFS_FILE_ATTR_HIDDEN != 0 { flags |= UInt32(UF_HIDDEN) }
    return flags
}

func fileAttributes(_ current: UInt32, applyingBSDFlags flags: UInt32) -> UInt32 {
    var fa = current
    if flags & UInt32(UF_HIDDEN) != 0 { fa |= NTFS_FILE_ATTR_HIDDEN } else { fa &= ~NTFS_FILE_ATTR_HIDDEN }
    return fa
}

/// Fill FSKit attributes from the core's `ntfs_attr`. `parentID` is the
/// directory the item was reached through (NTFS hard links have several).
func fillAttributes(_ out: FSItem.Attributes, from a: ntfs_attr, fileID: FSItem.Identifier, parentID: FSItem.Identifier) {
    out.type = FSItem.ItemType(coreType: a.type)
    out.mode = a.mode
    out.linkCount = a.nlink
    out.uid = a.uid
    out.gid = a.gid
    out.size = a.size
    out.allocSize = a.alloc_size
    out.fileID = fileID
    out.parentID = parentID
    out.accessTime = timespec(a.atime)
    out.modifyTime = timespec(a.mtime)
    out.changeTime = timespec(a.ctime)
    out.birthTime = timespec(a.crtime)
    out.flags = bsdFlags(fromFileAttributes: a.file_attributes)
    out.supportsLimitedXAttrs = false
    out.inhibitKernelOffloadedIO = false
}
