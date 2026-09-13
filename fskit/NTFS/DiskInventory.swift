// SPDX-License-Identifier: GPL-2.0
//
// Every NTFS partition on the machine, mounted or not, so the app can show them
// all and let the user decide. `getmntinfo` only knows about volumes that are
// already mounted, which leaves out exactly the ones a user might want to do
// something about — an NTFS partition macOS declined to mount, a Windows
// recovery partition, a volume another driver took.
//
// Partitions come from IOKit (every leaf IOMedia), mount state from
// `getmntinfo`, and "is this one ours" from the extension's open block device.
//
// What counts as an NTFS partition here is the partition *type*, which is all
// that is knowable without reading the disk — and reading a raw device needs
// root, which this app deliberately does not have. So this is a list of
// candidates: mounting one is how you find out. A candidate that turns out not
// to be NTFS simply fails to mount, which is reported rather than hidden.

import Foundation
import IOKit
import DiskArbitration

struct Partition: Identifiable, Equatable {
    let bsdName: String                 // disk6s3
    let content: String                 // partition type: a GPT GUID, or an MBR hint
    let size: UInt64
    let mediaWritable: Bool

    var mountPoint: String?
    var fsType: String?                 // as the kernel reports it, once mounted
    var mountedReadOnly: Bool = false
    var servedByOurDriver: Bool = false

    var id: String { bsdName }
    var device: String { "/dev/" + bsdName }
    var isMounted: Bool { mountPoint != nil }

    /// Display name: the volume name when mounted, otherwise the device.
    var displayName: String {
        if let mountPoint { return (mountPoint as NSString).lastPathComponent }
        return bsdName
    }

    var sizeDescription: String {
        ByteCountFormatter.string(fromByteCount: Int64(size), countStyle: .file)
    }

    /// Partition types that can hold NTFS. Matching the type is the most we can
    /// do without reading the disk; see the note at the top of the file.
    static let ntfsPartitionTypes: Set<String> = [
        "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7",   // GPT Microsoft Basic Data
        "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC",   // GPT Windows Recovery
        "WINDOWS_NTFS",                            // MBR type 0x07, as Apple names it
    ]

    var couldBeNTFS: Bool {
        if let fsType { return fsType == "ntfs" || fsType == "ttntfs" }
        return Self.ntfsPartitionTypes.contains(content.uppercased())
    }

    /// Mounted by something other than us — offer to take it over.
    var heldByAnotherDriver: Bool { isMounted && !servedByOurDriver }
}

@MainActor
final class DiskInventory: ObservableObject {
    @Published private(set) var partitions: [Partition] = []
    @Published private(set) var note: String?

    private var session: DASession?
    private var timer: Timer?

    /// Poll: partitions appear and disappear with cables, and another driver can
    /// take or release a volume without telling us.
    func start() {
        guard timer == nil else { return }
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    init() {
        session = DASessionCreate(kCFAllocatorDefault)
        if let session { DASessionSetDispatchQueue(session, .main) }
    }

    func refresh() {
        // Attach mount state before filtering. A whole-disk volume — an attached
        // image, or a disk formatted without a partition table — carries no
        // partition type at all, so the only thing that identifies it as NTFS is
        // the filesystem the kernel reports once it is mounted.
        var found = Self.leafPartitions()
        let mounts = Self.mountTable()
        for index in found.indices {
            if let m = mounts[found[index].bsdName] {
                found[index].mountPoint = m.mountPoint
                found[index].fsType = m.fsType
                found[index].mountedReadOnly = m.readOnly
            }
            found[index].servedByOurDriver = ModuleEnabler.deviceIsServedByModule(found[index].device)
        }
        // Keep only NTFS-capable partitions; a mounted exFAT volume that merely
        // sits on a Microsoft Basic Data partition is not ours to touch.
        found = found.filter { partition in
            if let fs = partition.fsType { return fs == "ntfs" || fs == "ttntfs" }
            return partition.couldBeNTFS
        }
        found.sort { $0.bsdName.localizedStandardCompare($1.bsdName) == .orderedAscending }
        if found != partitions { partitions = found }
    }

    // MARK: Actions

    func mount(_ partition: Partition) async {
        note = nil
        guard let session, let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, partition.device) else {
            note = "\(partition.displayName): no such device."
            return
        }
        let problem = await withCheckedContinuation { (c: CheckedContinuation<String?, Never>) in
            DADiskMount(disk, nil, DADiskMountOptions(kDADiskMountOptionDefault), { _, dissenter, ctx in
                let box = Unmanaged<Continuation>.fromOpaque(ctx!).takeRetainedValue()
                box.resume(DiskInventory.describe(dissenter))
            }, Unmanaged.passRetained(Continuation(c)).toOpaque())
        }
        if let problem {
            note = "\(partition.displayName) could not be mounted: \(problem). "
                 + "It may not contain NTFS, or may need repair in Windows."
        }
        try? await Task.sleep(for: .milliseconds(600))
        refresh()
    }

    func unmount(_ partition: Partition) async {
        note = nil
        guard let session, let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, partition.device) else { return }
        let problem = await withCheckedContinuation { (c: CheckedContinuation<String?, Never>) in
            DADiskUnmount(disk, DADiskUnmountOptions(kDADiskUnmountOptionDefault), { _, dissenter, ctx in
                let box = Unmanaged<Continuation>.fromOpaque(ctx!).takeRetainedValue()
                box.resume(DiskInventory.describe(dissenter))
            }, Unmanaged.passRetained(Continuation(c)).toOpaque())
        }
        if let problem {
            note = "\(partition.displayName) could not be ejected: \(problem). "
                 + "Something may still be using it."
        }
        try? await Task.sleep(for: .milliseconds(400))
        refresh()
    }

    fileprivate final class Continuation {
        private let c: CheckedContinuation<String?, Never>
        init(_ c: CheckedContinuation<String?, Never>) { self.c = c }
        func resume(_ v: String?) { c.resume(returning: v) }
    }

    fileprivate static func describe(_ dissenter: DADissenter?) -> String? {
        guard let dissenter else { return nil }
        if let reason = DADissenterGetStatusString(dissenter) as String? { return reason }
        return String(format: "error 0x%08X", DADissenterGetStatus(dissenter))
    }

    // MARK: System queries

    /// Every leaf IOMedia, which is every partition plus any whole disk that
    /// carries a filesystem directly.
    private static func leafPartitions() -> [Partition] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOMedia"), &iterator) == KERN_SUCCESS
        else { return [] }
        defer { IOObjectRelease(iterator) }

        var out: [Partition] = []
        while case let service = IOIteratorNext(iterator), service != 0 {
            defer { IOObjectRelease(service) }
            func property<T>(_ key: String, as: T.Type) -> T? {
                IORegistryEntryCreateCFProperty(service, key as CFString, kCFAllocatorDefault, 0)?
                    .takeRetainedValue() as? T
            }
            guard property("Leaf", as: Bool.self) == true,
                  let bsd = property("BSD Name", as: String.self) else { continue }
            out.append(Partition(bsdName: bsd,
                                 content: property("Content", as: String.self) ?? "",
                                 size: property("Size", as: UInt64.self) ?? 0,
                                 mediaWritable: property("Writable", as: Bool.self) ?? false))
        }
        return out
    }

    private struct MountInfo { let mountPoint: String; let fsType: String; let readOnly: Bool }

    private static func mountTable() -> [String: MountInfo] {
        var buffer: UnsafeMutablePointer<statfs>?
        let count = getmntinfo(&buffer, MNT_NOWAIT)
        guard count > 0, let buffer else { return [:] }
        var table: [String: MountInfo] = [:]
        for i in 0..<Int(count) {
            var entry = buffer[i]
            func string(_ field: inout some Any) -> String {
                withUnsafeBytes(of: &field) { raw in
                    String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
                }
            }
            let device = string(&entry.f_mntfromname)
            guard device.hasPrefix("/dev/") else { continue }
            table[(device as NSString).lastPathComponent] =
                MountInfo(mountPoint: string(&entry.f_mntonname),
                          fsType: string(&entry.f_fstypename),
                          readOnly: entry.f_flags & UInt32(MNT_RDONLY) != 0)
        }
        return table
    }
}
