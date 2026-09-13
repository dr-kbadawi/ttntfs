// SPDX-License-Identifier: GPL-2.0
//
// Mount status shared between the extension and the host app, without XPC:
// the extension writes one JSON file into the app-group container on
// activate / deactivate / unload, the app polls it (VolumeMonitor). Both
// targets compile this file.
//
//   <group container>/Library/Application Support/mount-status.json
//   { "disk4s1": { ...MountStatus... }, ... }

import Foundation

struct MountStatus: Codable, Equatable {
    static let appGroup = "group.ch.techtag.ntfs"
    static let fileName = "mount-status.json"

    var bsdName: String
    var label: String
    var readOnly: Bool
    var roReason: Int            // enum ntfs_ro_reason
    var roReasonText: String     // human readable, "" when read/write
    var dirty: Bool
    var hibernated: Bool
    var logfileClean: Bool
    var coreVersion: String
    var mountedAt: Date
    /// What a journal replay would do, when the volume mounted read-only
    /// because its journal is unclean. Read-only analysis; nothing was written.
    /// Empty when the journal is clean or the analysis did not run.
    var journalSummary: String = ""

    /// Human text for ntfs_ro_reason (kept here so the app does not need the C header).
    static func reasonText(_ reason: Int, deviceReadOnly: Bool = false) -> String {
        switch reason {
        case 0: return ""
        case 1: return "Mounted read-only: requested in Settings or with -o ro."
        case 2: return "Mounted read-only: the device is write-protected."
        case 3: return "Mounted read-only: the volume is marked dirty. Run chkdsk /f in Windows, or eject it properly there."
        case 4: return "Mounted read-only: Windows is hibernated on this volume (Fast Startup). Shut Windows down fully (shutdown /s /t 0)."
        case 5: return "Mounted read-only: the NTFS journal ($LogFile) is not clean. Eject the volume properly in Windows."
        case 6: return "Mounted read-only: the volume uses a feature this driver cannot write safely."
        case 7: return "Switched to read-only: errors were found while mounted. Check the volume in Windows."
        default: return "Mounted read-only."
        }
    }

    static var fileURL: URL? {
        guard let base = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: appGroup) else { return nil }
        let dir = base.appendingPathComponent("Library/Application Support", isDirectory: true)
        return dir.appendingPathComponent(fileName)
    }

    static func readAll() -> [String: MountStatus] {
        guard let url = fileURL, let data = try? Data(contentsOf: url) else { return [:] }
        let dec = JSONDecoder()
        dec.dateDecodingStrategy = .iso8601
        return (try? dec.decode([String: MountStatus].self, from: data)) ?? [:]
    }

    private static let lock = NSLock()

    /// Read-modify-write. FSKit runs **one extension process per mounted
    /// volume** (verified with two NTFS disks: a probe instance plus one per
    /// volume), so an NSLock alone is not enough — two volumes activating at
    /// the same moment would each read the file, add their own entry, and the
    /// second write would drop the first. The NSLock serialises threads inside
    /// one process; an advisory lock on a sidecar file serialises the
    /// processes.
    static func update(_ body: (inout [String: MountStatus]) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard let url = fileURL else { return }
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                 withIntermediateDirectories: true)
        let fd = open(url.appendingPathExtension("lock").path, O_CREAT | O_RDWR, 0o644)
        if fd >= 0 {
            flock(fd, LOCK_EX)
        }
        defer {
            if fd >= 0 { flock(fd, LOCK_UN); close(fd) }
        }
        var all = readAll()
        body(&all)
        let enc = JSONEncoder()
        enc.dateEncodingStrategy = .iso8601
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        do {
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let data = try enc.encode(all)
            try data.write(to: url, options: .atomic)
        } catch {
            NSLog("mount-status: cannot write %@: %@", url.path, error.localizedDescription)
        }
    }

    static func publish(_ s: MountStatus) { update { $0[s.bsdName] = s } }
    static func remove(bsdName: String) { update { $0.removeValue(forKey: bsdName) } }
}
