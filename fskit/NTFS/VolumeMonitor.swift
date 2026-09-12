// SPDX-License-Identifier: GPL-2.0
//
// Lists mounted NTFS volumes by polling getmntinfo(3), joins them with the
// status file the extension writes into the app group (MountStatus.swift:
// read-only reason, dirty/hibernated state), and unmounts through
// DiskArbitration.

import Foundation
import DiskArbitration

struct MountedVolume: Identifiable, Equatable {
    var id: String { mountPoint }
    let mountPoint: String
    let device: String          // /dev/disk4s1
    let name: String
    let readOnly: Bool          // MNT_RDONLY as the kernel sees it, or the extension's software read-only
    let reason: String          // why read-only ("" when read/write)
    let dirty: Bool
    let hibernated: Bool

    var bsdName: String { (device as NSString).lastPathComponent }
}

@MainActor
final class VolumeMonitor: ObservableObject {
    @Published private(set) var volumes: [MountedVolume] = []
    /// Status entries with no matching mount (activation failed, or a volume
    /// the extension mounted read-only that the kernel has not listed yet).
    @Published private(set) var staleStatus: [MountStatus] = []
    private var timer: Timer?
    private var session: DASession?

    func start() {
        guard timer == nil else { return }
        session = DASessionCreate(kCFAllocatorDefault)
        if let session { DASessionSetDispatchQueue(session, .main) }
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    func refresh() {
        let status = MountStatus.readAll()
        var mounts: UnsafeMutablePointer<statfs>?
        let n = getmntinfo(&mounts, MNT_NOWAIT)
        var found: [MountedVolume] = []
        var seen = Set<String>()
        if n > 0, let mounts {
            for i in 0..<Int(n) {
                var st = mounts[i]
                let type = withUnsafePointer(to: &st.f_fstypename) { p in
                    p.withMemoryRebound(to: CChar.self, capacity: Int(MFSTYPENAMELEN)) { String(cString: $0) }
                }
                guard type == "ntfs" || type == "ntfsx" else { continue }
                let mnt = withUnsafePointer(to: &st.f_mntonname) { p in
                    p.withMemoryRebound(to: CChar.self, capacity: Int(MAXPATHLEN)) { String(cString: $0) }
                }
                let dev = withUnsafePointer(to: &st.f_mntfromname) { p in
                    p.withMemoryRebound(to: CChar.self, capacity: Int(MAXPATHLEN)) { String(cString: $0) }
                }
                let bsd = (dev as NSString).lastPathComponent
                let s = status[bsd]
                seen.insert(bsd)
                let kernelRO = (st.f_flags & UInt32(MNT_RDONLY)) != 0
                let ro = kernelRO || (s?.readOnly ?? false)
                var reason = s?.roReasonText ?? ""
                if ro && reason.isEmpty { reason = kernelRO ? "Mounted read-only by the system." : "Mounted read-only." }
                found.append(MountedVolume(mountPoint: mnt, device: dev,
                                           name: (mnt as NSString).lastPathComponent,
                                           readOnly: ro, reason: reason,
                                           dirty: s?.dirty ?? false, hibernated: s?.hibernated ?? false))
            }
        }
        if found != volumes { volumes = found }
        let stale = status.values.filter { !seen.contains($0.bsdName) }.sorted { $0.bsdName < $1.bsdName }
        if stale != staleStatus { staleStatus = stale }
    }

    func unmount(_ v: MountedVolume) {
        guard let session,
              let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, v.device) else { return }
        DADiskUnmount(disk, DADiskUnmountOptions(kDADiskUnmountOptionDefault), { _, dissenter, _ in
            if let dissenter {
                let status = DADissenterGetStatus(dissenter)
                NSLog("unmount failed: 0x%x", status)
            }
        }, nil)
    }
}
