// SPDX-License-Identifier: GPL-2.0
//
// Lists mounted NTFS volumes by polling getmntinfo(3) and unmounts through
// DiskArbitration.

import Foundation
import DiskArbitration

struct MountedVolume: Identifiable, Equatable {
    var id: String { mountPoint }
    let mountPoint: String
    let device: String
    let name: String
    let readOnly: Bool
}

@MainActor
final class VolumeMonitor: ObservableObject {
    @Published private(set) var volumes: [MountedVolume] = []
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
        var mounts: UnsafeMutablePointer<statfs>?
        let n = getmntinfo(&mounts, MNT_NOWAIT)
        guard n > 0, let mounts else { volumes = []; return }
        var found: [MountedVolume] = []
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
            found.append(MountedVolume(mountPoint: mnt, device: dev,
                                       name: (mnt as NSString).lastPathComponent,
                                       readOnly: (st.f_flags & UInt32(MNT_RDONLY)) != 0))
        }
        if found != volumes { volumes = found }
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
