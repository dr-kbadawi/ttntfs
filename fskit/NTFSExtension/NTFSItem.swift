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
    var openCount = 0

    init(inode: OpaquePointer, parentID: FSItem.Identifier, type: FSItem.ItemType) {
        self.inode = inode
        self.inodeNumber = ntfs_inode_number(inode)
        self.id = NTFSItem.identifier(forInode: inodeNumber)
        self.parentID = parentID
        self.itemType = type
        super.init()
    }

    /// NTFS root is MFT record 5; FSKit wants the root's ID to be `.rootDirectory` (2).
    /// MFT records 1 and 2 ($MFTMirr, $LogFile) are only visible with `showsystem`;
    /// they are moved out of the way so IDs never collide with FSKit's reserved values.
    static let rootInode: UInt64 = 5
    static func identifier(forInode ino: UInt64) -> FSItem.Identifier {
        if ino == rootInode { return .rootDirectory }
        if ino == 1 || ino == 2 { return FSItem.Identifier(rawValue: ino | (1 << 62))! }
        return FSItem.Identifier(rawValue: ino)!
    }
    static func inode(forIdentifier id: FSItem.Identifier) -> UInt64 {
        if id == .rootDirectory { return rootInode }
        return id.rawValue & ~(UInt64(1) << 62)
    }
}

extension FSItem.ItemType {
    init(coreType: Int32) {
        switch coreType {
        case Int32(NTFS_ITEM_DIR.rawValue): self = .directory
        case Int32(NTFS_ITEM_SYMLINK.rawValue): self = .symlink
        default: self = .file
        }
    }
}

extension timespec {
    init(_ t: ntfs_timespec) { self.init(tv_sec: Int(t.sec), tv_nsec: Int(t.nsec)) }
}

extension ntfs_timespec {
    init(_ t: timespec) { self.init(sec: Int64(t.tv_sec), nsec: Int32(t.tv_nsec)) }
}

/// Fill FSKit attributes from the core's `ntfs_attr`.
func fillAttributes(_ out: FSItem.Attributes, from a: ntfs_attr, item: NTFSItem?) {
    out.type = FSItem.ItemType(coreType: a.type)
    out.mode = a.mode
    out.linkCount = a.nlink
    out.uid = a.uid
    out.gid = a.gid
    out.size = a.size
    out.allocSize = a.alloc_size
    out.fileID = item?.id ?? NTFSItem.identifier(forInode: a.inode_no)
    if let item { out.parentID = item.parentID }
    out.accessTime = timespec(a.atime)
    out.modifyTime = timespec(a.mtime)
    out.changeTime = timespec(a.ctime)
    out.birthTime = timespec(a.crtime)
    // BSD flags: map NTFS hidden -> UF_HIDDEN, read-only -> UF_IMMUTABLE is too strong;
    // expose hidden only. FILE_ATTR_HIDDEN = 0x2.
    var flags: UInt32 = 0
    if a.file_attributes & 0x2 != 0 { flags |= UInt32(UF_HIDDEN) }
    out.flags = flags
    out.supportsLimitedXAttrs = false
}
