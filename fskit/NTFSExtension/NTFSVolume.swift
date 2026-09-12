// SPDX-License-Identifier: GPL-2.0
//
// FSVolume: every operation is a thin translation onto core/include/ntfscore.h.

import Foundation
import FSKit

final class NTFSVolume: FSVolume {
    let device: OpaquePointer
    private(set) var options: MountOptions
    private var vol: OpaquePointer?                  // ntfs_volume_t *
    private var root: NTFSItem?
    private let itemsLock = NSLock()
    private var items: [UInt64: NTFSItem] = [:]      // live FSItems by inode number
    private var deviceReleased = false

    init(volumeID: FSVolume.Identifier, volumeName: FSFileName,
         device: OpaquePointer, options: MountOptions) {
        self.device = device
        self.options = options
        super.init(volumeID: volumeID, volumeName: volumeName)
    }

    func releaseDevice() {
        if !deviceReleased {
            deviceReleased = true
            ntfs_bdev_fskit_close(device)
        }
    }

    // MARK: Item bookkeeping

    /// Returns the live FSItem for a core inode, adopting the reference passed in
    /// (or dropping it if an item already exists).
    private func item(adopting inode: OpaquePointer, parent: FSItem.Identifier, type: FSItem.ItemType) -> NTFSItem {
        let ino = ntfs_inode_number(inode)
        return itemsLock.withLock {
            if let existing = items[ino] {
                ntfs_inode_put(inode)
                return existing
            }
            let it = NTFSItem(inode: inode, parentID: parent, type: type)
            items[ino] = it
            return it
        }
    }

    private func ntfsItem(_ item: FSItem) -> NTFSItem? { item as? NTFSItem }

    private func coreType(of inode: OpaquePointer) -> FSItem.ItemType {
        var a = ntfs_attr()
        if ntfs_getattr(inode, &a) == 0 { return FSItem.ItemType(coreType: a.type) }
        return .file
    }
}

// MARK: - FSVolume.PathConfOperations

extension NTFSVolume: FSVolume.PathConfOperations {
    var maximumLinkCount: Int { 1023 }
    var maximumNameLength: Int { 255 }
    var restrictsOwnershipChanges: Bool { true }
    var truncatesLongNames: Bool { false }
    var maximumXattrSize: Int { 64 * 1024 }
    var maximumFileSize: UInt64 { 0xFFFF_FFFF_FFFF }   // 2^48-1: NTFS theoretical 16 EiB, keep sane
}

// MARK: - FSVolume.Operations

extension NTFSVolume: FSVolume.Operations {
    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let c = FSVolume.SupportedCapabilities()
        c.supportsPersistentObjectIDs = true
        c.supports64BitObjectIDs = true
        c.supportsSymbolicLinks = true
        c.supportsHardLinks = true
        c.supportsSparseFiles = true
        c.supportsZeroRuns = false
        c.supports2TBFiles = true
        c.supportsHiddenFiles = true
        c.supportsFastStatFS = true
        c.supportsOpenDenyModes = false
        c.doesNotSupportSettingFilePermissions = true     // noowners semantics (PORTING.md §6)
        c.doesNotSupportRootTimes = false
        c.supportsJournal = false
        c.supportsActiveJournal = false
        c.caseFormat = options.caseSensitive ? .sensitive : .insensitiveCasePreserving
        return c
    }

    var volumeStatistics: FSStatFSResult {
        let s = FSStatFSResult(fileSystemTypeName: "ntfs")
        guard let vol else { return s }
        var info = ntfs_volume_info()
        guard ntfs_volume_get_info(vol, &info) == 0 else { return s }
        let bs = UInt64(info.cluster_size)
        s.blockSize = Int(bs)
        s.ioSize = 1 << 20
        s.totalBlocks = info.total_clusters
        s.freeBlocks = info.free_clusters
        s.availableBlocks = info.free_clusters
        s.usedBlocks = info.total_clusters - info.free_clusters
        s.totalBytes = info.total_clusters * bs
        s.freeBytes = info.free_clusters * bs
        s.availableBytes = info.free_clusters * bs
        s.usedBytes = (info.total_clusters - info.free_clusters) * bs
        s.totalFiles = info.total_mft_records
        s.freeFiles = info.free_mft_records
        return s
    }

    func mount(options taskOptions: FSTaskOptions, replyHandler reply: @escaping (Error?) -> Void) {
        var opts = self.options
        opts.apply(taskOptions: taskOptions)
        self.options = opts
        var copts = opts.cOptions
        var v: OpaquePointer?
        let rc = ntfs_mount(device, &copts, &v)
        guard rc == 0, let v else {
            reply(posixError(rc, "mount"))
            return
        }
        vol = v
        var info = ntfs_volume_info()
        if ntfs_volume_get_info(v, &info) == 0, info.read_only {
            log.notice("mounted read-only, reason \(info.ro_reason)")
        }
        reply(nil)
    }

    func unmount(replyHandler reply: @escaping () -> Void) {
        if let root { ntfs_inode_put(root.inode); self.root = nil }
        itemsLock.withLock {
            for it in items.values { ntfs_inode_put(it.inode) }
            items.removeAll()
        }
        if let vol {
            let rc = ntfs_unmount(vol)
            if rc != 0 { log.error("unmount: \(rc)") }
            self.vol = nil
        }
        reply()
    }

    func synchronize(flags: FSSyncFlags, replyHandler reply: @escaping (Error?) -> Void) {
        guard let vol else { reply(nil); return }
        let rc = ntfs_volume_sync(vol)
        reply(rc == 0 ? nil : posixError(rc, "sync"))
    }

    func activate(options: FSTaskOptions, replyHandler reply: @escaping (FSItem?, Error?) -> Void) {
        guard let vol else { reply(nil, fs_errorForPOSIXError(ENXIO)); return }
        var r: OpaquePointer?
        let rc = ntfs_volume_root(vol, &r)
        guard rc == 0, let r else { reply(nil, posixError(rc, "root")); return }
        let item = self.item(adopting: r, parent: .parentOfRoot, type: .directory)
        root = item
        ntfs_inode_ref(r)   // root holds its own reference in addition to the items table
        reply(item, nil)
    }

    func deactivate(options: FSDeactivateOptions, replyHandler reply: @escaping (Error?) -> Void) {
        if let root { ntfs_inode_put(root.inode); self.root = nil }
        reply(nil)
    }

    func getAttributes(_ desired: FSItem.GetAttributesRequest, of item: FSItem,
                       replyHandler reply: @escaping (FSItem.Attributes?, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(nil, fs_errorForPOSIXError(EINVAL)); return }
        var a = ntfs_attr()
        let rc = ntfs_getattr(it.inode, &a)
        guard rc == 0 else { reply(nil, posixError(rc)); return }
        let out = FSItem.Attributes()
        fillAttributes(out, from: a, item: it)
        reply(out, nil)
    }

    func setAttributes(_ req: FSItem.SetAttributesRequest, on item: FSItem,
                       replyHandler reply: @escaping (FSItem.Attributes?, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(nil, fs_errorForPOSIXError(EINVAL)); return }
        var a = ntfs_attr()
        var valid: UInt32 = 0
        var consumed: FSItem.Attribute = []
        if req.isValid(.size) { a.size = req.size; valid |= NTFS_SETATTR_SIZE.rawValue; consumed.insert(.size) }
        if req.isValid(.mode) { a.mode = req.mode; valid |= NTFS_SETATTR_MODE.rawValue; consumed.insert(.mode) }
        if req.isValid(.accessTime) { a.atime = ntfs_timespec(req.accessTime); valid |= NTFS_SETATTR_ATIME.rawValue; consumed.insert(.accessTime) }
        if req.isValid(.modifyTime) { a.mtime = ntfs_timespec(req.modifyTime); valid |= NTFS_SETATTR_MTIME.rawValue; consumed.insert(.modifyTime) }
        if req.isValid(.changeTime) { a.ctime = ntfs_timespec(req.changeTime); valid |= NTFS_SETATTR_CTIME.rawValue; consumed.insert(.changeTime) }
        if req.isValid(.birthTime) { a.crtime = ntfs_timespec(req.birthTime); valid |= NTFS_SETATTR_CRTIME.rawValue; consumed.insert(.birthTime) }
        if req.isValid(.flags) {
            // UF_HIDDEN <-> FILE_ATTR_HIDDEN. Other BSD flags are not representable.
            var cur = ntfs_attr()
            if ntfs_getattr(it.inode, &cur) == 0 {
                a.file_attributes = cur.file_attributes
                if req.flags & UInt32(UF_HIDDEN) != 0 { a.file_attributes |= 0x2 } else { a.file_attributes &= ~UInt32(0x2) }
                valid |= NTFS_SETATTR_FLAGS.rawValue
                consumed.insert(.flags)
            }
        }
        // uid/gid: noowners; accept silently.
        if req.isValid(.uid) { consumed.insert(.uid) }
        if req.isValid(.gid) { consumed.insert(.gid) }

        if valid != 0 {
            let rc = ntfs_setattr(it.inode, &a, valid)
            guard rc == 0 else { reply(nil, posixError(rc, "setattr")); return }
        }
        req.consumedAttributes = consumed
        var now = ntfs_attr()
        let rc = ntfs_getattr(it.inode, &now)
        guard rc == 0 else { reply(nil, posixError(rc)); return }
        let out = FSItem.Attributes()
        fillAttributes(out, from: now, item: it)
        reply(out, nil)
    }

    func lookupItem(named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        guard let dir = ntfsItem(directory) else { reply(nil, nil, fs_errorForPOSIXError(EINVAL)); return }
        guard let cname = name.string else { reply(nil, nil, fs_errorForPOSIXError(EILSEQ)); return }
        var ino: OpaquePointer?
        let rc = ntfs_lookup(dir.inode, cname, &ino)
        guard rc == 0, let ino else { reply(nil, nil, posixError(rc)); return }
        let it = item(adopting: ino, parent: dir.id, type: coreType(of: ino))
        reply(it, name, nil)
    }

    func reclaimItem(_ item: FSItem, replyHandler reply: @escaping (Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(nil); return }
        itemsLock.withLock {
            if items[it.inodeNumber] === it { items.removeValue(forKey: it.inodeNumber) }
        }
        ntfs_inode_put(it.inode)
        reply(nil)
    }

    func readSymbolicLink(_ item: FSItem, replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(nil, fs_errorForPOSIXError(EINVAL)); return }
        var buf = [CChar](repeating: 0, count: Int(PATH_MAX))
        var len = 0
        let rc = ntfs_readlink(it.inode, &buf, buf.count, &len)
        guard rc == 0 else { reply(nil, posixError(rc)); return }
        let data = Data(bytes: buf, count: len)
        reply(FSFileName(data: data), nil)
    }

    func createItem(named name: FSFileName, type: FSItem.ItemType, inDirectory directory: FSItem,
                    attributes: FSItem.SetAttributesRequest,
                    replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        guard let dir = ntfsItem(directory) else { reply(nil, nil, fs_errorForPOSIXError(EINVAL)); return }
        guard let cname = name.string else { reply(nil, nil, fs_errorForPOSIXError(EILSEQ)); return }
        let mode: UInt32 = attributes.isValid(.mode) ? attributes.mode : (type == .directory ? 0o755 : 0o644)
        var ino: OpaquePointer?
        let rc: Int32
        switch type {
        case .file: rc = ntfs_create(dir.inode, cname, mode, &ino)
        case .directory: rc = ntfs_mkdir(dir.inode, cname, mode, &ino)
        default: reply(nil, nil, fs_errorForPOSIXError(ENOTSUP)); return
        }
        guard rc == 0, let ino else { reply(nil, nil, posixError(rc, "create")); return }
        let it = item(adopting: ino, parent: dir.id, type: type)
        attributes.consumedAttributes = [.mode]
        reply(it, name, nil)
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem,
                            attributes: FSItem.SetAttributesRequest, linkContents contents: FSFileName,
                            replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        guard let dir = ntfsItem(directory) else { reply(nil, nil, fs_errorForPOSIXError(EINVAL)); return }
        guard let cname = name.string, let target = contents.string else {
            reply(nil, nil, fs_errorForPOSIXError(EILSEQ)); return
        }
        var ino: OpaquePointer?
        let rc = ntfs_symlink(dir.inode, cname, target, &ino)
        guard rc == 0, let ino else { reply(nil, nil, posixError(rc, "symlink")); return }
        let it = item(adopting: ino, parent: dir.id, type: .symlink)
        reply(it, name, nil)
    }

    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        guard let it = ntfsItem(item), let dir = ntfsItem(directory) else {
            reply(nil, fs_errorForPOSIXError(EINVAL)); return
        }
        guard let cname = name.string else { reply(nil, fs_errorForPOSIXError(EILSEQ)); return }
        let rc = ntfs_link(it.inode, dir.inode, cname)
        reply(rc == 0 ? name : nil, rc == 0 ? nil : posixError(rc, "link"))
    }

    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem,
                    replyHandler reply: @escaping (Error?) -> Void) {
        guard let it = ntfsItem(item), let dir = ntfsItem(directory) else {
            reply(fs_errorForPOSIXError(EINVAL)); return
        }
        guard let cname = name.string else { reply(fs_errorForPOSIXError(EILSEQ)); return }
        let rc = it.itemType == .directory ? ntfs_rmdir(dir.inode, cname) : ntfs_unlink(dir.inode, cname)
        reply(rc == 0 ? nil : posixError(rc, "remove"))
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName,
                    to destinationName: FSFileName, inDirectory destinationDirectory: FSItem,
                    overItem: FSItem?, replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        guard let it = ntfsItem(item), let src = ntfsItem(sourceDirectory), let dst = ntfsItem(destinationDirectory) else {
            reply(nil, fs_errorForPOSIXError(EINVAL)); return
        }
        guard let sname = sourceName.string, let dname = destinationName.string else {
            reply(nil, fs_errorForPOSIXError(EILSEQ)); return
        }
        let rc = ntfs_rename(src.inode, sname, dst.inode, dname)
        guard rc == 0 else { reply(nil, posixError(rc, "rename")); return }
        it.parentID = dst.id
        reply(destinationName, nil)
    }

    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie,
                            verifier: FSDirectoryVerifier, attributes: FSItem.GetAttributesRequest?,
                            packer: FSDirectoryEntryPacker,
                            replyHandler reply: @escaping (FSDirectoryVerifier, Error?) -> Void) {
        guard let dir = ntfsItem(directory) else { reply(verifier, fs_errorForPOSIXError(EINVAL)); return }

        // Cookie space: 0 = ".", 1 = "..", 2+ = core cookie + 2.
        var pos = cookie.rawValue
        if pos == 0 {
            let a = FSItem.Attributes()
            if attributes != nil { var ca = ntfs_attr(); if ntfs_getattr(dir.inode, &ca) == 0 { fillAttributes(a, from: ca, item: dir) } }
            guard packer.packEntry(name: FSFileName(string: "."), itemType: .directory, itemID: dir.id,
                                   nextCookie: FSDirectoryCookie(rawValue: 1), attributes: attributes != nil ? a : nil)
            else { reply(verifier, nil); return }
            pos = 1
        }
        if pos == 1 {
            guard packer.packEntry(name: FSFileName(string: ".."), itemType: .directory, itemID: dir.parentID,
                                   nextCookie: FSDirectoryCookie(rawValue: 2), attributes: nil)
            else { reply(verifier, nil); return }
            pos = 2
        }

        // Lag-by-one packing so each entry's nextCookie is the following entry's position.
        final class State {
            var packer: FSDirectoryEntryPacker
            var wantAttr: Bool
            var pending: (name: Data, type: FSItem.ItemType, id: FSItem.Identifier, attr: ntfs_attr?, cookie: UInt64)?
            var stopped = false
            var resumeAt: UInt64 = 0
            init(packer: FSDirectoryEntryPacker, wantAttr: Bool) { self.packer = packer; self.wantAttr = wantAttr }
            func flush(nextCookie: UInt64, parent: FSItem.Identifier) -> Bool {
                guard let p = pending else { return true }
                var attrs: FSItem.Attributes?
                if let a = p.attr {
                    attrs = FSItem.Attributes()
                    fillAttributes(attrs!, from: a, item: nil)
                    attrs!.fileID = p.id
                    attrs!.parentID = parent
                }
                let ok = packer.packEntry(name: FSFileName(data: p.name), itemType: p.type, itemID: p.id,
                                          nextCookie: FSDirectoryCookie(rawValue: nextCookie + 2), attributes: attrs)
                if !ok { resumeAt = p.cookie + 2; stopped = true }
                pending = nil
                return ok
            }
        }
        let state = State(packer: packer, wantAttr: attributes != nil)
        let parentID = dir.id
        var coreCookie = pos - 2
        var eof = false
        let ctx = Unmanaged.passUnretained(state).toOpaque()
        let rc = ntfs_readdir(dir.inode, &coreCookie, attributes != nil, { entp, ctxp in
            guard let entp, let ctxp else { return 1 }
            let st = Unmanaged<State>.fromOpaque(ctxp).takeUnretainedValue()
            let e = entp.pointee
            // Pack the previous entry now that we know this one's position.
            if !st.flush(nextCookie: e.cookie, parent: .invalid) { return 1 }
            let name = Data(bytes: e.name, count: e.name_len)
            st.pending = (name, FSItem.ItemType(coreType: e.type), NTFSItem.identifier(forInode: e.inode_no),
                          e.has_attr ? e.attr : nil, e.cookie)
            return 0
        }, ctx, &eof)
        if rc < 0 && !state.stopped {
            reply(verifier, posixError(rc, "readdir"))
            return
        }
        if !state.stopped {
            // Last entry: nextCookie is the core's resume position.
            _ = state.flush(nextCookie: coreCookie, parent: parentID)
        }
        reply(verifier, nil)
    }
}

// MARK: - Open/close, read/write

extension NTFSVolume: FSVolume.OpenCloseOperations {
    func openItem(_ item: FSItem, modes: FSVolume.OpenModes, replyHandler reply: @escaping (Error?) -> Void) {
        ntfsItem(item)?.openCount += 1
        reply(nil)
    }
    func closeItem(_ item: FSItem, modes: FSVolume.OpenModes, replyHandler reply: @escaping (Error?) -> Void) {
        if let it = ntfsItem(item) {
            it.openCount = max(0, it.openCount - 1)
            if it.openCount == 0 && !options.readOnly { _ = ntfs_fsync(it.inode, true) }
        }
        reply(nil)
    }
}

extension NTFSVolume: FSVolume.ReadWriteOperations {
    func read(from item: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer,
              replyHandler reply: @escaping (Int, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(0, fs_errorForPOSIXError(EINVAL)); return }
        let n = buffer.withUnsafeMutableBytes { raw -> Int in
            ntfs_read(it.inode, raw.baseAddress, min(length, raw.count), UInt64(offset))
        }
        if n < 0 { reply(0, posixError(Int32(n), "read")) } else { reply(n, nil) }
    }

    func write(contents: Data, to item: FSItem, at offset: off_t,
               replyHandler reply: @escaping (Int, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(0, fs_errorForPOSIXError(EINVAL)); return }
        let n = contents.withUnsafeBytes { raw -> Int in
            ntfs_write(it.inode, raw.baseAddress, raw.count, UInt64(offset))
        }
        if n < 0 { reply(0, posixError(Int32(n), "write")) } else { reply(n, nil) }
    }
}

// MARK: - Xattrs (stored as alternate data streams by the core)

extension NTFSVolume: FSVolume.XattrOperations {
    func getXattr(named name: FSFileName, of item: FSItem, replyHandler reply: @escaping (Data?, Error?) -> Void) {
        guard let it = ntfsItem(item), let cname = name.string else { reply(nil, fs_errorForPOSIXError(EINVAL)); return }
        var len = 0
        var rc = ntfs_getxattr(it.inode, cname, nil, 0, &len)
        guard rc == 0 else { reply(nil, posixError(rc)); return }
        var data = Data(count: len)
        if len > 0 {
            rc = data.withUnsafeMutableBytes { ntfs_getxattr(it.inode, cname, $0.baseAddress, $0.count, &len) }
            guard rc == 0 else { reply(nil, posixError(rc)); return }
            data.count = len
        }
        reply(data, nil)
    }

    func setXattr(named name: FSFileName, to value: Data?, on item: FSItem, policy: FSVolume.SetXattrPolicy,
                  replyHandler reply: @escaping (Error?) -> Void) {
        guard let it = ntfsItem(item), let cname = name.string else { reply(fs_errorForPOSIXError(EINVAL)); return }
        let rc: Int32
        switch policy {
        case .delete:
            rc = ntfs_removexattr(it.inode, cname)
        default:
            var flags: Int32 = 0
            if policy == .mustCreate { flags = XATTR_CREATE }
            if policy == .mustReplace { flags = XATTR_REPLACE }
            let v = value ?? Data()
            rc = v.withUnsafeBytes { ntfs_setxattr(it.inode, cname, $0.baseAddress, $0.count, flags) }
        }
        reply(rc == 0 ? nil : posixError(rc, "setxattr"))
    }

    func listXattrs(of item: FSItem, replyHandler reply: @escaping ([FSFileName]?, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(nil, fs_errorForPOSIXError(EINVAL)); return }
        var len = 0
        var rc = ntfs_listxattr(it.inode, nil, 0, &len)
        guard rc == 0 else { reply(nil, posixError(rc)); return }
        var buf = [CChar](repeating: 0, count: max(len, 1))
        if len > 0 {
            rc = ntfs_listxattr(it.inode, &buf, buf.count, &len)
            guard rc == 0 else { reply(nil, posixError(rc)); return }
        }
        var names: [FSFileName] = []
        var start = 0
        for i in 0..<len where buf[i] == 0 {
            if i > start { names.append(FSFileName(data: Data(bytes: buf[start..<i].map { UInt8(bitPattern: $0) }, count: i - start))) }
            start = i + 1
        }
        reply(names, nil)
    }
}

// MARK: - Item deactivation, preallocate, volume rename

extension NTFSVolume: FSVolume.ItemDeactivation {
    var itemDeactivationPolicy: FSVolume.ItemDeactivationOptions { [.forRemovedItems] }
    func deactivateItem(_ item: FSItem, replyHandler reply: @escaping (Error?) -> Void) {
        // Reference is dropped in reclaimItem; nothing else to do.
        reply(nil)
    }
}

extension NTFSVolume: FSVolume.PreallocateOperations {
    func preallocateSpace(for item: FSItem, at offset: off_t, length: Int, flags: FSVolume.PreallocateFlags,
                          replyHandler reply: @escaping (Int, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(0, fs_errorForPOSIXError(EINVAL)); return }
        let rc = ntfs_fallocate(it.inode, UInt64(offset), UInt64(length), true)
        reply(rc == 0 ? length : 0, rc == 0 ? nil : posixError(rc, "preallocate"))
    }
}

extension NTFSVolume: FSVolume.RenameOperations {
    func setVolumeName(_ name: FSFileName, replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        guard let vol, let s = name.string else { reply(nil, fs_errorForPOSIXError(EINVAL)); return }
        let rc = ntfs_volume_set_label(vol, s)
        if rc == 0 { self.name = name }
        reply(rc == 0 ? name : nil, rc == 0 ? nil : posixError(rc, "setlabel"))
    }
}
