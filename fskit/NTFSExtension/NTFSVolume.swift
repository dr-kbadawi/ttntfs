// SPDX-License-Identifier: GPL-2.0
//
// FSVolume: every operation is a thin translation onto core/include/ntfscore.h.
//
// Lifecycle as FSKit drives a unary file system (FSVolume.h doc comments):
//   loadResource  -> NTFSVolume created, device wrapped (NTFSFileSystem)
//   activate      -> ntfs_mount, publish status, return the root item
//   mount         -> signal only ("no defined options"); nothing to do
//   ... operations ...
//   unmount       -> ntfs_volume_sync ("clear and flush all cached state")
//   reclaimItem   -> for every item FSKit still holds (drops core references)
//   deactivate    -> ntfs_unmount (after every reclaim; "avoid I/O" - the
//                    sync above already made it cheap)
//   unloadResource-> device closed (ntfs_unmount here too if deactivate never came)
//
// Cookies: the core's readdir emits "." (cookie 0) and ".." (cookie 1) itself
// (kernel dir_emit_dots) and real entries from 2 on; FSKit cookies are the
// core's cookies verbatim. When FSKit asks for attributes the dot entries are
// filtered out (FSKit: "don't pack . and .. if attributes isn't nil").

import Foundation
import FSKit

final class NTFSVolume: FSVolume {
    let device: OpaquePointer                        // struct ntfs_bdev *
    let bsdName: String
    let coreLabel: String
    private(set) var options: MountOptions
    private var vol: OpaquePointer?                  // ntfs_volume_t *
    private var root: NTFSItem?
    private let itemsLock = NSLock()
    private var items: [UInt64: NTFSItem] = [:]      // live FSItems by inode number
    private let stateLock = NSLock()
    private var deviceReleased = false
    /// True when the core mounted read-only (requested, device, dirty,
    /// hibernated, ...) or --rdonly was given. Every mutating operation is
    /// refused with EROFS here so the core never sees a write on such a
    /// volume, whatever the kernel believes about the mount flags.
    private(set) var isReadOnly = false
    private(set) var roReason: Int32 = 0

    init(volumeID: FSVolume.Identifier, volumeName: FSFileName,
         device: OpaquePointer, bsdName: String, label: String, options: MountOptions) {
        self.device = device
        self.bsdName = bsdName
        self.coreLabel = label
        self.options = options
        super.init(volumeID: volumeID, volumeName: volumeName)
    }

    /// Called from unloadResource: last chance to unmount, then close the device.
    func releaseDevice() {
        stateLock.lock(); defer { stateLock.unlock() }
        if let v = vol {
            log.notice("unload: volume still mounted, unmounting now")
            dropAllItems()
            let rc = ntfs_unmount(v)
            if rc != 0 { log.error("unmount (late): rc \(rc)") }
            vol = nil
            MountStatus.remove(bsdName: bsdName)
        }
        if !deviceReleased {
            deviceReleased = true
            ntfs_bdev_fskit_close(device)
        }
    }

    // MARK: Item bookkeeping

    /// Returns the live FSItem for a core inode, adopting the reference passed in
    /// (or dropping it if an item already exists). FSKit reclaims each FSItem
    /// object exactly once, so one item per inode keeps the reference count balanced.
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

    private func dropAllItems() {
        if let r = root { ntfs_inode_put(r.inode); root = nil }
        itemsLock.withLock {
            for it in items.values { ntfs_inode_put(it.inode) }
            items.removeAll()
        }
    }

    private func ntfsItem(_ item: FSItem) -> NTFSItem? { item as? NTFSItem }

    private func coreType(of inode: OpaquePointer) -> FSItem.ItemType {
        var a = ntfs_attr()
        if ntfs_getattr(inode, &a) == 0 { return FSItem.ItemType(coreType: a.type) }
        return .file
    }

    /// EROFS guard for every mutating operation.
    private var writable: Bool { !isReadOnly }

    /// A read-only look at what replaying the journal would do, run only when
    /// the journal is why the volume is read-only. Writes nothing: the replay
    /// engine's changes go to an in-memory overlay. It is the only thing we can
    /// honestly offer for this case -- replay itself stays refused because the
    /// log layout it depends on has never been checked against a journal
    /// Windows wrote (docs/LOGFILE.md).
    private func journalSummary(_ info: ntfs_volume_info) -> String {
        guard !info.logfile_clean else { return "" }
        var analysis = ntfs_logfile_analysis()
        guard ntfs_logfile_analyse(device, &analysis) >= 0 else { return "" }
        func text(_ field: inout some Any) -> String {
            withUnsafeBytes(of: &field) { raw in
                String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
            }
        }
        let state = text(&analysis.state), message = text(&analysis.message)
        log.notice("\(self.bsdName, privacy: .public): journal \(state, privacy: .public) v\(analysis.log_version_major).\(analysis.log_version_minor) clean=\(analysis.clean) supported=\(analysis.supported) chkdsk=\(analysis.needs_chkdsk) records=\(analysis.records_analyzed) redo=\(analysis.records_redone) undo=\(analysis.records_undone)")
        return "Journal: \(state), version \(analysis.log_version_major).\(analysis.log_version_minor). \(message)"
    }

    private func publishStatus(_ info: ntfs_volume_info) {
        let label = withUnsafePointer(to: info.label) { p in
            p.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
        }
        MountStatus.publish(MountStatus(
            bsdName: bsdName, label: label.isEmpty ? coreLabel : label,
            readOnly: isReadOnly, roReason: Int(roReason),
            roReasonText: isReadOnly ? MountStatus.reasonText(Int(roReason)) : "",
            dirty: info.dirty, hibernated: info.hibernated, logfileClean: info.logfile_clean,
            coreVersion: String(cString: ntfs_core_version()), mountedAt: Date(),
            journalSummary: journalSummary(info)))
    }
}

// MARK: - FSVolume.PathConfOperations

extension NTFSVolume: FSVolume.PathConfOperations {
    var maximumLinkCount: Int { 1023 }                 // NTFS hard-link limit per file
    var maximumNameLength: Int { 255 }                 // UTF-16 units; the core enforces
    var restrictsOwnershipChanges: Bool { true }       // noowners
    var truncatesLongNames: Bool { false }
    var maximumXattrSize: Int { 64 * 1024 }            // ADS as xattr; keep Finder-sized
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
        c.supportsHiddenFiles = true                   // UF_HIDDEN <-> FILE_ATTR_HIDDEN
        c.supportsFastStatFS = true
        c.supportsOpenDenyModes = false
        c.doesNotSupportSettingFilePermissions = true  // noowners semantics (PORTING.md §6)
        c.doesNotSupportImmutableFiles = true          // UF_IMMUTABLE not representable
        c.doesNotSupportRootTimes = false
        c.doesNotSupportVolumeSizes = false
        c.supportsDocumentID = false
        c.supportsJournal = false                      // $LogFile replay is phase 4; not a live journal for FSKit
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
        s.fileSystemSubType = 0
        return s
    }

    /// Activation: allocate the in-memory state (= mount the core) and hand
    /// back the root item. FSKit calls this before mount.
    func activate(options taskOptions: FSTaskOptions, replyHandler reply: @escaping (FSItem?, Error?) -> Void) {
        stateLock.lock(); defer { stateLock.unlock() }
        if vol != nil, let root {                       // re-activation after a failed mount attempt
            reply(root, nil); return
        }
        var opts = self.options
        opts.apply(taskOptions: taskOptions)             // -o ro,showsystem,... (FSActivateOptionSyntax)
        self.options = opts
        if opts.kernelReadOnly { _ = ntfs_bdev_fskit_set_read_only(device, true) }
        var copts = opts.cOptions
        var v: OpaquePointer?
        let rc = ntfs_mount(device, &copts, &v)
        guard rc == 0, let v else {
            reply(nil, posixError(rc, "mount \(bsdName)"))
            return
        }
        var info = ntfs_volume_info()
        if ntfs_volume_get_info(v, &info) == 0 {
            isReadOnly = info.read_only || opts.readOnly
            roReason = info.read_only ? info.ro_reason : (opts.readOnly ? Int32(NTFS_RO_REQUESTED.rawValue) : 0)
        } else {
            isReadOnly = opts.readOnly
            roReason = opts.readOnly ? Int32(NTFS_RO_REQUESTED.rawValue) : 0
        }
        var r: OpaquePointer?
        let rrc = ntfs_volume_root(v, &r)
        guard rrc == 0, let r else {
            _ = ntfs_unmount(v)
            reply(nil, posixError(rrc, "root \(bsdName)"))
            return
        }
        vol = v
        let item = self.item(adopting: r, parent: .parentOfRoot, type: .directory)
        ntfs_inode_ref(r)   // root holds its own reference in addition to the items table entry
        root = item
        if isReadOnly {
            log.notice("\(self.bsdName, privacy: .public): mounted read-only (\(MountStatus.reasonText(Int(self.roReason)), privacy: .public))")
        } else {
            log.info("\(self.bsdName, privacy: .public): mounted read/write")
        }
        publishStatus(info)
        reply(item, nil)
    }

    /// Mount signal. The core is already mounted (activate); the only thing an
    /// option can still change is a late read-only request, enforced in software.
    func mount(options taskOptions: FSTaskOptions, replyHandler reply: @escaping (Error?) -> Void) {
        guard vol != nil else { reply(fsError(ENXIO)); return }
        var opts = self.options
        opts.apply(taskOptions: taskOptions)
        if opts.readOnly && !isReadOnly {
            log.notice("\(self.bsdName, privacy: .public): read-only requested at mount; enforcing")
            isReadOnly = true
            roReason = Int32(NTFS_RO_REQUESTED.rawValue)
            _ = ntfs_bdev_fskit_set_read_only(device, true)
            var info = ntfs_volume_info()
            if let vol, ntfs_volume_get_info(vol, &info) == 0 { publishStatus(info) }
        }
        self.options = opts
        reply(nil)
    }

    /// "Clear and flush all cached state": everything dirty goes to the device.
    /// Items are still referenced by FSKit here (reclaims follow), so the core
    /// stays mounted until deactivate.
    func unmount(replyHandler reply: @escaping () -> Void) {
        if let vol, writable {
            let rc = ntfs_volume_sync(vol)
            if rc != 0 { log.error("unmount: sync rc \(rc)") }
        }
        reply()
    }

    func deactivate(options: FSDeactivateOptions, replyHandler reply: @escaping (Error?) -> Void) {
        stateLock.lock(); defer { stateLock.unlock() }
        let leftover = itemsLock.withLock { items.count }
        if leftover > 1 { log.notice("deactivate: \(leftover) items not reclaimed; dropping") }
        dropAllItems()
        if let v = vol {
            let rc = ntfs_unmount(v)
            vol = nil
            MountStatus.remove(bsdName: bsdName)
            if rc != 0 {
                log.error("ntfs_unmount: rc \(rc)")
                reply(posixError(rc, "unmount")); return
            }
        }
        reply(nil)
    }

    func synchronize(flags: FSSyncFlags, replyHandler reply: @escaping (Error?) -> Void) {
        guard let vol else { reply(nil); return }
        guard writable else { reply(nil); return }
        let rc = ntfs_volume_sync(vol)
        reply(rc == 0 ? nil : posixError(rc, "sync"))
    }

    func getAttributes(_ desired: FSItem.GetAttributesRequest, of item: FSItem,
                       replyHandler reply: @escaping (FSItem.Attributes?, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(nil, fsError(EINVAL)); return }
        var a = ntfs_attr()
        let rc = ntfs_getattr(it.inode, &a)
        guard rc == 0 else { reply(nil, posixError(rc, "getattr")); return }
        let out = FSItem.Attributes()
        fillAttributes(out, from: a, fileID: it.id, parentID: it.parentID)
        reply(out, nil)
    }

    func setAttributes(_ req: FSItem.SetAttributesRequest, on item: FSItem,
                       replyHandler reply: @escaping (FSItem.Attributes?, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(nil, fsError(EINVAL)); return }
        var consumed: FSItem.Attribute = []

        // Size: truncate/extend (files only; "ignore attempts to set the size
        // of directories or symbolic links; don't produce an error").
        if req.isValid(.size) {
            if it.itemType == .file {
                guard writable else { reply(nil, fsError(EROFS)); return }
                let rc = ntfs_truncate(it.inode, req.size)
                guard rc == 0 else { reply(nil, posixError(rc, "truncate")); return }
                it.dirty = true
            }
            consumed.insert(.size)
        }

        // Times and flags in one core call.
        var a = ntfs_attr()
        var valid: UInt32 = 0
        if req.isValid(.accessTime) { a.atime = ntfs_timespec(req.accessTime); valid |= NTFS_SETATTR_ATIME.rawValue; consumed.insert(.accessTime) }
        if req.isValid(.modifyTime) { a.mtime = ntfs_timespec(req.modifyTime); valid |= NTFS_SETATTR_MTIME.rawValue; consumed.insert(.modifyTime) }
        if req.isValid(.changeTime) { a.ctime = ntfs_timespec(req.changeTime); valid |= NTFS_SETATTR_CTIME.rawValue; consumed.insert(.changeTime) }
        if req.isValid(.birthTime) { a.crtime = ntfs_timespec(req.birthTime); valid |= NTFS_SETATTR_CRTIME.rawValue; consumed.insert(.birthTime) }
        if req.isValid(.flags) {
            var cur = ntfs_attr()
            let grc = ntfs_getattr(it.inode, &cur)
            guard grc == 0 else { reply(nil, posixError(grc, "getattr")); return }
            let want = fileAttributes(cur.file_attributes, applyingBSDFlags: req.flags)
            if want != cur.file_attributes {
                a.file_attributes = want
                valid |= NTFS_SETATTR_FLAGS.rawValue
            }
            consumed.insert(.flags)   // bits we cannot represent are silently dropped
        }
        if valid != 0 {
            guard writable else { reply(nil, fsError(EROFS)); return }
            let rc = ntfs_setattr(it.inode, &a, valid)
            guard rc == 0 else { reply(nil, posixError(rc, "setattr")); return }
        }

        // Mode: the volume declares doesNotSupportSettingFilePermissions, so this
        // rarely arrives. Try, but a refusal by the core is "not supported by the
        // on-disk format": don't fail, just don't consume.
        if req.isValid(.mode) {
            if writable {
                var m = ntfs_attr(); m.mode = req.mode
                let rc = ntfs_setattr(it.inode, &m, NTFS_SETATTR_MODE.rawValue)
                if rc == 0 { consumed.insert(.mode) }
                else if hostErrno(rc) != ENOTSUP && hostErrno(rc) != EINVAL && hostErrno(rc) != EPERM {
                    reply(nil, posixError(rc, "chmod")); return
                }
            }
        }
        // uid/gid: noowners; accept silently so chown does not fail (kernel then
        // sees the mounting user in the reply and stops).
        if req.isValid(.uid) { consumed.insert(.uid) }
        if req.isValid(.gid) { consumed.insert(.gid) }

        req.consumedAttributes = consumed
        var now = ntfs_attr()
        let rc = ntfs_getattr(it.inode, &now)
        guard rc == 0 else { reply(nil, posixError(rc, "getattr")); return }
        let out = FSItem.Attributes()
        fillAttributes(out, from: now, fileID: it.id, parentID: it.parentID)
        reply(out, nil)
    }

    func lookupItem(named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        guard let dir = ntfsItem(directory) else { reply(nil, nil, fsError(EINVAL)); return }
        guard let cname = name.string else { reply(nil, nil, fsError(EILSEQ)); return }
        // Dot entries: the kernel normally resolves them itself; be safe.
        if cname == "." { reply(dir, name, nil); return }
        if cname == ".." {
            if dir.isRoot { reply(dir, name, nil); return }
            guard let vol else { reply(nil, nil, fsError(ENXIO)); return }
            var pino: OpaquePointer?
            let rc = ntfs_inode_get(vol, NTFSItem.inode(forIdentifier: dir.parentID), &pino)
            guard rc == 0, let pino else { reply(nil, nil, posixError(rc, "lookup ..")); return }
            let grand: FSItem.Identifier = (dir.parentID == .rootDirectory) ? .parentOfRoot : .invalid
            let parent = itemsLock.withLock { items[ntfs_inode_number(pino)] }
            if let parent { ntfs_inode_put(pino); reply(parent, name, nil); return }
            reply(item(adopting: pino, parent: grand, type: .directory), name, nil)
            return
        }
        var ino: OpaquePointer?
        let rc = ntfs_lookup(dir.inode, cname, &ino)
        guard rc == 0, let ino else { reply(nil, nil, posixError(rc, "lookup")); return }
        let it = item(adopting: ino, parent: dir.id, type: coreType(of: ino))
        reply(it, name, nil)
    }

    func reclaimItem(_ item: FSItem, replyHandler reply: @escaping (Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(nil); return }
        let live = itemsLock.withLock { () -> Bool in
            if items[it.inodeNumber] === it { items.removeValue(forKey: it.inodeNumber); return true }
            return false
        }
        if live { ntfs_inode_put(it.inode) }   // else already dropped by deactivate
        reply(nil)
    }

    func readSymbolicLink(_ item: FSItem, replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(nil, fsError(EINVAL)); return }
        var buf = [CChar](repeating: 0, count: Int(PATH_MAX) + 1)
        var len = 0
        let rc = buf.withUnsafeMutableBufferPointer { p in
            ntfs_readlink(it.inode, p.baseAddress, p.count, &len)
        }
        guard rc == 0 else { reply(nil, posixError(rc, "readlink")); return }
        let data = buf.withUnsafeBytes { Data($0.prefix(min(len, buf.count))) }
        reply(FSFileName(data: data), nil)
    }

    /// Attributes FSKit sends along with create: mode is taken by the create
    /// call; the rest (flags, times) is applied afterwards.
    private func applyCreateAttributes(_ attributes: FSItem.SetAttributesRequest, to it: NTFSItem, modeConsumed: Bool) {
        var consumed: FSItem.Attribute = modeConsumed ? [.mode] : []
        var a = ntfs_attr()
        var valid: UInt32 = 0
        if attributes.isValid(.accessTime) { a.atime = ntfs_timespec(attributes.accessTime); valid |= NTFS_SETATTR_ATIME.rawValue; consumed.insert(.accessTime) }
        if attributes.isValid(.modifyTime) { a.mtime = ntfs_timespec(attributes.modifyTime); valid |= NTFS_SETATTR_MTIME.rawValue; consumed.insert(.modifyTime) }
        if attributes.isValid(.birthTime) { a.crtime = ntfs_timespec(attributes.birthTime); valid |= NTFS_SETATTR_CRTIME.rawValue; consumed.insert(.birthTime) }
        if attributes.isValid(.flags) {
            var cur = ntfs_attr()
            if ntfs_getattr(it.inode, &cur) == 0 {
                a.file_attributes = fileAttributes(cur.file_attributes, applyingBSDFlags: attributes.flags)
                valid |= NTFS_SETATTR_FLAGS.rawValue
                consumed.insert(.flags)
            }
        }
        if valid != 0 {
            let rc = ntfs_setattr(it.inode, &a, valid)
            if rc != 0 { log.info("create: post-attributes rc \(rc), ignored") }
        }
        if attributes.isValid(.uid) { consumed.insert(.uid) }
        if attributes.isValid(.gid) { consumed.insert(.gid) }
        attributes.consumedAttributes = consumed
    }

    func createItem(named name: FSFileName, type: FSItem.ItemType, inDirectory directory: FSItem,
                    attributes: FSItem.SetAttributesRequest,
                    replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        guard let dir = ntfsItem(directory) else { reply(nil, nil, fsError(EINVAL)); return }
        guard let cname = name.string else { reply(nil, nil, fsError(EILSEQ)); return }
        guard writable else { reply(nil, nil, fsError(EROFS)); return }
        let mode: UInt32 = attributes.isValid(.mode) ? (attributes.mode & 0o7777) : (type == .directory ? 0o755 : 0o644)
        var ino: OpaquePointer?
        let rc: Int32
        switch type {
        case .file: rc = ntfs_create(dir.inode, cname, mode, &ino)
        case .directory: rc = ntfs_mkdir(dir.inode, cname, mode, &ino)
        default: reply(nil, nil, fsError(ENOTSUP)); return   // FIFOs, sockets, devices: not on NTFS
        }
        guard rc == 0, let ino else { reply(nil, nil, posixError(rc, "create")); return }
        dir.dirVersion &+= 1
        let it = item(adopting: ino, parent: dir.id, type: type)
        applyCreateAttributes(attributes, to: it, modeConsumed: attributes.isValid(.mode))
        reply(it, name, nil)
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem,
                            attributes: FSItem.SetAttributesRequest, linkContents contents: FSFileName,
                            replyHandler reply: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        guard let dir = ntfsItem(directory) else { reply(nil, nil, fsError(EINVAL)); return }
        guard let cname = name.string, let target = contents.string else { reply(nil, nil, fsError(EILSEQ)); return }
        guard writable else { reply(nil, nil, fsError(EROFS)); return }
        var ino: OpaquePointer?
        let rc = ntfs_symlink(dir.inode, cname, target, &ino)
        guard rc == 0, let ino else { reply(nil, nil, posixError(rc, "symlink")); return }
        dir.dirVersion &+= 1
        let it = item(adopting: ino, parent: dir.id, type: .symlink)
        applyCreateAttributes(attributes, to: it, modeConsumed: attributes.isValid(.mode))
        reply(it, name, nil)
    }

    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        guard let it = ntfsItem(item), let dir = ntfsItem(directory) else { reply(nil, fsError(EINVAL)); return }
        guard let cname = name.string else { reply(nil, fsError(EILSEQ)); return }
        guard writable else { reply(nil, fsError(EROFS)); return }
        if it.itemType == .directory { reply(nil, fsError(EPERM)); return }
        let rc = ntfs_link(it.inode, dir.inode, cname)
        guard rc == 0 else { reply(nil, posixError(rc, "link")); return }
        dir.dirVersion &+= 1
        reply(name, nil)
    }

    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem,
                    replyHandler reply: @escaping (Error?) -> Void) {
        guard let it = ntfsItem(item), let dir = ntfsItem(directory) else { reply(fsError(EINVAL)); return }
        guard let cname = name.string else { reply(fsError(EILSEQ)); return }
        guard writable else { reply(fsError(EROFS)); return }
        let rc = it.itemType == .directory ? ntfs_rmdir(dir.inode, cname) : ntfs_unlink(dir.inode, cname)
        guard rc == 0 else { reply(posixError(rc, "remove")); return }
        dir.dirVersion &+= 1
        reply(nil)
    }

    /// POSIX rename through the core, which replaces an existing target itself
    /// (file over file, empty dir over dir; ENOTEMPTY / EISDIR / ENOTDIR /
    /// EINVAL for a directory into itself come back from the core). overItem,
    /// if any, keeps its FSItem until FSKit reclaims it; the core frees the
    /// inode when the last reference goes.
    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName,
                    to destinationName: FSFileName, inDirectory destinationDirectory: FSItem,
                    overItem: FSItem?, replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        guard let it = ntfsItem(item), let src = ntfsItem(sourceDirectory), let dst = ntfsItem(destinationDirectory) else {
            reply(nil, fsError(EINVAL)); return
        }
        guard let sname = sourceName.string, let dname = destinationName.string else { reply(nil, fsError(EILSEQ)); return }
        guard writable else { reply(nil, fsError(EROFS)); return }
        if let overItem, let over = ntfsItem(overItem) {
            // Type compatibility is the core's business, but a directory over a
            // file (or vice versa) is a cheap early EISDIR/ENOTDIR here.
            if it.itemType == .directory && over.itemType != .directory { reply(nil, fsError(ENOTDIR)); return }
            if it.itemType != .directory && over.itemType == .directory { reply(nil, fsError(EISDIR)); return }
        }
        let rc = ntfs_rename(src.inode, sname, dst.inode, dname)
        guard rc == 0 else { reply(nil, posixError(rc, "rename")); return }
        it.parentID = dst.id
        src.dirVersion &+= 1
        if dst !== src { dst.dirVersion &+= 1 }
        reply(destinationName, nil)
    }

    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie,
                            verifier: FSDirectoryVerifier, attributes: FSItem.GetAttributesRequest?,
                            packer: FSDirectoryEntryPacker,
                            replyHandler reply: @escaping (FSDirectoryVerifier, Error?) -> Void) {
        guard let dir = ntfsItem(directory) else { reply(verifier, fsError(EINVAL)); return }
        let currentVerifier = FSDirectoryVerifier(rawValue: max(dir.dirVersion, 1))
        let wantAttr = attributes != nil

        // Lag-by-one packing: an entry is packed when the *next* one arrives,
        // because its nextCookie is the next entry's position. When the packer
        // refuses, the callback returns nonzero, the core leaves that entry
        // unconsumed, and FSKit resumes from the last packed entry's
        // nextCookie, which is exactly the refused entry's position.
        final class State {
            let packer: FSDirectoryEntryPacker
            let vol: OpaquePointer?
            let wantAttr: Bool
            let dirID: FSItem.Identifier
            let dirIsRoot: Bool
            var pending: (name: Data, type: FSItem.ItemType, id: FSItem.Identifier, attr: ntfs_attr?, cookie: UInt64)?
            var stopped = false
            init(packer: FSDirectoryEntryPacker, vol: OpaquePointer?, wantAttr: Bool, dirID: FSItem.Identifier, dirIsRoot: Bool) {
                self.packer = packer; self.vol = vol; self.wantAttr = wantAttr; self.dirID = dirID; self.dirIsRoot = dirIsRoot
            }
            func flush(nextCookie: UInt64) -> Bool {
                guard let p = pending else { return true }
                var attrs: FSItem.Attributes?
                if let a = p.attr {
                    attrs = FSItem.Attributes()
                    fillAttributes(attrs!, from: a, fileID: p.id, parentID: dirID)
                }
                let ok = packer.packEntry(name: FSFileName(data: p.name), itemType: p.type, itemID: p.id,
                                          nextCookie: FSDirectoryCookie(rawValue: nextCookie), attributes: attrs)
                if !ok { stopped = true }
                pending = nil
                return ok
            }
        }
        let state = State(packer: packer, vol: vol, wantAttr: wantAttr, dirID: dir.id, dirIsRoot: dir.isRoot)
        var coreCookie = cookie.rawValue
        var eof = false
        let ctx = Unmanaged.passUnretained(state).toOpaque()
        let rc = ntfs_readdir(dir.inode, &coreCookie, wantAttr, { entp, ctxp in
            guard let entp, let ctxp else { return 1 }
            let st = Unmanaged<State>.fromOpaque(ctxp).takeUnretainedValue()
            let e = entp.pointee
            // Pack the previous entry now that this one's position is known.
            if !st.flush(nextCookie: e.cookie) { return 1 }
            let isDot = (e.name_len == 1 && e.name[0] == 0x2e) || (e.name_len == 2 && e.name[0] == 0x2e && e.name[1] == 0x2e)
            if isDot && st.wantAttr { return 0 }          // FSKit: no dot entries with attributes
            var id = NTFSItem.identifier(forInode: e.inode_no)
            if isDot && st.dirIsRoot { id = .rootDirectory } // root: "." and ".." identical
            var attr: ntfs_attr? = e.has_attr ? e.attr : nil
            if st.wantAttr && attr == nil, let vol = st.vol {
                var ni: OpaquePointer?
                if ntfs_inode_get(vol, e.inode_no, &ni) == 0, let ni {
                    var a = ntfs_attr()
                    if ntfs_getattr(ni, &a) == 0 { attr = a }
                    ntfs_inode_put(ni)
                }
            }
            if st.wantAttr && attr == nil { return 0 }      // cannot describe it; skip rather than lie
            st.pending = (Data(bytes: e.name, count: e.name_len), FSItem.ItemType(coreType: e.type), id, attr, e.cookie)
            return 0
        }, ctx, &eof)
        if rc < 0 && !state.stopped {
            let e = hostErrno(rc)
            reply(currentVerifier, e == EINVAL && cookie.rawValue != 0 ? invalidCookieError() : posixError(rc, "readdir"))
            return
        }
        if !state.stopped {
            // Last entry: its nextCookie is the core's resume position (EOF).
            _ = state.flush(nextCookie: coreCookie)
        }
        reply(currentVerifier, nil)
    }
}

// MARK: - Open/close, read/write

extension NTFSVolume: FSVolume.OpenCloseOperations {
    func openItem(_ item: FSItem, modes: FSVolume.OpenModes, replyHandler reply: @escaping (Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(fsError(EINVAL)); return }
        if modes.contains(.write) && !writable { reply(fsError(EROFS)); return }
        it.openCount += 1
        reply(nil)
    }

    /// `modes` are the modes that remain open after this close. When nothing
    /// stays open for writing and the file was written, flush it (write-back is
    /// bounded to 1 s anyway, PORTING.md §6; this makes close() the barrier
    /// applications expect on removable media).
    func closeItem(_ item: FSItem, modes: FSVolume.OpenModes, replyHandler reply: @escaping (Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(nil); return }
        it.openCount = max(0, it.openCount - 1)
        if it.dirty && !modes.contains(.write) && writable {
            let rc = ntfs_fsync(it.inode, true)
            if rc != 0 { reply(posixError(rc, "fsync on close")); return }
            it.dirty = false
        }
        reply(nil)
    }
}

extension NTFSVolume: FSVolume.ReadWriteOperations {
    /// FSKit's buffer is handed to the core directly (no copy). Short reads at
    /// EOF and reads past EOF (0 bytes) are not errors.
    func read(from item: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer,
              replyHandler reply: @escaping (Int, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(0, fsError(EINVAL)); return }
        guard offset >= 0 else { reply(0, fsError(EINVAL)); return }
        if length == 0 { reply(0, nil); return }
        let n = buffer.withUnsafeMutableBytes { raw -> Int in
            guard let base = raw.baseAddress else { return 0 }
            return ntfs_read(it.inode, base, min(length, raw.count), UInt64(offset))
        }
        if n < 0 { reply(0, posixError(n, "read")) } else { reply(n, nil) }
    }

    /// Data's storage is passed straight to the core. The ABI returns bytes
    /// transferred; a short write without an error is retried for the rest.
    func write(contents: Data, to item: FSItem, at offset: off_t,
               replyHandler reply: @escaping (Int, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(0, fsError(EINVAL)); return }
        guard offset >= 0 else { reply(0, fsError(EINVAL)); return }
        guard writable else { reply(0, fsError(EROFS)); return }
        if contents.isEmpty { reply(0, nil); return }
        var done = 0
        var err: Int = 0
        contents.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return }
            while done < raw.count {
                let n = ntfs_write(it.inode, base + done, raw.count - done, UInt64(offset) + UInt64(done))
                if n < 0 { err = n; break }
                if n == 0 { err = -Int(ENOSPC); break }
                done += n
            }
        }
        if done > 0 { it.dirty = true }
        if err != 0 { reply(done, posixError(err, "write")) } else { reply(done, nil) }
    }
}

// MARK: - Xattrs (stored as alternate data streams by the core)

extension NTFSVolume: FSVolume.XattrOperations {
    func getXattr(named name: FSFileName, of item: FSItem, replyHandler reply: @escaping (Data?, Error?) -> Void) {
        guard let it = ntfsItem(item), let cname = name.string else { reply(nil, fsError(EINVAL)); return }
        var len = 0
        var rc = ntfs_getxattr(it.inode, cname, nil, 0, &len)
        guard rc == 0 else { reply(nil, xattrError(rc, "getxattr")); return }
        var data = Data(count: len)
        if len > 0 {
            rc = data.withUnsafeMutableBytes { ntfs_getxattr(it.inode, cname, $0.baseAddress, $0.count, &len) }
            guard rc == 0 else { reply(nil, xattrError(rc, "getxattr")); return }
            if len < data.count { data.count = len }
        }
        reply(data, nil)
    }

    func setXattr(named name: FSFileName, to value: Data?, on item: FSItem, policy: FSVolume.SetXattrPolicy,
                  replyHandler reply: @escaping (Error?) -> Void) {
        guard let it = ntfsItem(item), let cname = name.string else { reply(fsError(EINVAL)); return }
        guard writable else { reply(fsError(EROFS)); return }
        let rc: Int32
        switch policy {
        case .delete:
            rc = ntfs_removexattr(it.inode, cname)
        default:
            // Linux flag values (the core is built with linux/xattr.h), see bridging header.
            var flags: Int32 = 0
            if policy == .mustCreate { flags = Int32(NTFS_XATTR_CREATE_FLAG) }
            if policy == .mustReplace { flags = Int32(NTFS_XATTR_REPLACE_FLAG) }
            let v = value ?? Data()
            if v.isEmpty {
                var dummy: UInt8 = 0
                rc = ntfs_setxattr(it.inode, cname, &dummy, 0, flags)
            } else {
                rc = v.withUnsafeBytes { ntfs_setxattr(it.inode, cname, $0.baseAddress, $0.count, flags) }
            }
        }
        reply(rc == 0 ? nil : xattrError(rc, "setxattr"))
    }

    func listXattrs(of item: FSItem, replyHandler reply: @escaping ([FSFileName]?, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(nil, fsError(EINVAL)); return }
        var len = 0
        var rc = ntfs_listxattr(it.inode, nil, 0, &len)
        guard rc == 0 else { reply(nil, xattrError(rc, "listxattr")); return }
        if len == 0 { reply([], nil); return }
        var buf = [UInt8](repeating: 0, count: len)
        rc = buf.withUnsafeMutableBufferPointer { p in
            p.baseAddress!.withMemoryRebound(to: CChar.self, capacity: p.count) { ntfs_listxattr(it.inode, $0, p.count, &len) }
        }
        guard rc == 0 else { reply(nil, xattrError(rc, "listxattr")); return }
        var names: [FSFileName] = []
        var start = 0
        for i in 0..<min(len, buf.count) where buf[i] == 0 {
            if i > start { names.append(FSFileName(data: Data(buf[start..<i]))) }
            start = i + 1
        }
        reply(names, nil)
    }
}

// MARK: - Item deactivation, preallocate, volume rename

extension NTFSVolume: FSVolume.ItemDeactivation {
    var itemDeactivationPolicy: FSVolume.ItemDeactivationOptions { [.forRemovedItems] }
    func deactivateItem(_ item: FSItem, replyHandler reply: @escaping (Error?) -> Void) {
        // VNOP_INACTIVE equivalent: nothing to free early; the core reference
        // is dropped in reclaimItem and the core frees an unlinked inode then.
        reply(nil)
    }
}

extension NTFSVolume: FSVolume.PreallocateOperations {
    func preallocateSpace(for item: FSItem, at offset: off_t, length: Int, flags: FSVolume.PreallocateFlags,
                          replyHandler reply: @escaping (Int, Error?) -> Void) {
        guard let it = ntfsItem(item) else { reply(0, fsError(EINVAL)); return }
        guard writable else { reply(0, fsError(EROFS)); return }
        guard offset >= 0, length >= 0 else { reply(0, fsError(EINVAL)); return }
        var start = UInt64(offset)
        if flags.contains(.fromEOF) {
            var a = ntfs_attr()
            let grc = ntfs_getattr(it.inode, &a)
            guard grc == 0 else { reply(0, posixError(grc, "getattr")); return }
            start = a.size &+ start
        }
        // keep_size: F_PREALLOCATE never changes the logical size.
        let rc = ntfs_fallocate(it.inode, start, UInt64(length), true)
        guard rc == 0 else { reply(0, posixError(rc, "preallocate")); return }
        it.dirty = true
        reply(length, nil)
    }
}

extension NTFSVolume: FSVolume.RenameOperations {
    func setVolumeName(_ name: FSFileName, replyHandler reply: @escaping (FSFileName?, Error?) -> Void) {
        guard let vol, let s = name.string else { reply(nil, fsError(EINVAL)); return }
        guard writable else { reply(nil, fsError(EROFS)); return }
        let rc = ntfs_volume_set_label(vol, s)
        guard rc == 0 else { reply(nil, posixError(rc, "setlabel")); return }
        self.name = name
        var info = ntfs_volume_info()
        if ntfs_volume_get_info(vol, &info) == 0 { publishStatus(info) }
        reply(name, nil)
    }
}
