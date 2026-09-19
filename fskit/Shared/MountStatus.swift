// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
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
    /*
     * How much work a replay would actually do. -1 means unknown (an older
     * status file, or a clean journal).
     *
     * Zero is the common case and it is not the dangerous one. Fast Startup is
     * on by default in Windows, and a hybrid shutdown does not dismount a
     * removable volume: the log is left open with VOLUME_IS_CLEAN clear, which
     * reads as dirty, while the transaction table is empty and there is nothing
     * to replay. Measured 2026-09-17, scenario B2. The UI needs to tell that
     * apart from a journal carrying genuine pending transactions, because the
     * warning that fits the second is wrong and frightening for the first.
     */
    var journalPendingOps: Int = -1

    /*
     * Decoded by hand because Swift's synthesized decoder ignores property
     * defaults: a key missing from the JSON throws, default or not. That matters
     * here because this file survives upgrades. journalSummary was added after
     * the first builds shipped, so a status file written by an older extension
     * has no such key -- and since readAll() decodes the whole dictionary in one
     * go, one old entry made the decode fail and the app showed NO status for
     * ANY volume until every one of them was remounted. Found by a unit test on
     * 2026-09-14 that was written expecting the opposite.
     *
     * Every field added from here on should be decodeIfPresent with a default,
     * for the same reason.
     */
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        bsdName       = try c.decode(String.self, forKey: .bsdName)
        label         = try c.decode(String.self, forKey: .label)
        readOnly      = try c.decode(Bool.self,   forKey: .readOnly)
        roReason      = try c.decode(Int.self,    forKey: .roReason)
        roReasonText  = try c.decode(String.self, forKey: .roReasonText)
        dirty         = try c.decode(Bool.self,   forKey: .dirty)
        hibernated    = try c.decode(Bool.self,   forKey: .hibernated)
        logfileClean  = try c.decode(Bool.self,   forKey: .logfileClean)
        coreVersion   = try c.decode(String.self, forKey: .coreVersion)
        mountedAt     = try c.decode(Date.self,   forKey: .mountedAt)
        journalSummary = try c.decodeIfPresent(String.self, forKey: .journalSummary) ?? ""
        journalPendingOps = try c.decodeIfPresent(Int.self, forKey: .journalPendingOps) ?? -1
    }

    /// The memberwise initialiser, which the custom `init(from:)` suppresses.
    init(bsdName: String, label: String, readOnly: Bool, roReason: Int,
         roReasonText: String, dirty: Bool, hibernated: Bool, logfileClean: Bool,
         coreVersion: String, mountedAt: Date, journalSummary: String = "",
                journalPendingOps: Int = -1) {
        self.bsdName = bsdName; self.label = label; self.readOnly = readOnly
        self.roReason = roReason; self.roReasonText = roReasonText
        self.dirty = dirty; self.hibernated = hibernated
        self.logfileClean = logfileClean; self.coreVersion = coreVersion
        self.mountedAt = mountedAt; self.journalSummary = journalSummary
        self.journalPendingOps = journalPendingOps
    }

    /// Human text for ntfs_ro_reason (kept here so the app does not need the C header).
    static func reasonText(_ reason: Int) -> String {
        switch reason {
        case 0: return ""
        case 1: return "Mounted read-only: requested in Settings or with -o ro."
        case 2: return "Mounted read-only: the device is write-protected."
        case 3: return "Mounted read-only: the volume is marked dirty. Run chkdsk /f in Windows, or eject it properly there."
        case 4: return "Mounted read-only: Windows is hibernated on this volume (Fast Startup). Shut Windows down fully (shutdown /s /t 0)."
        case 5: return "Mounted read-only: the NTFS journal ($LogFile) is not clean. Eject the volume properly in Windows."
        case 6: return "Mounted read-only: the volume uses a feature this driver cannot write safely."
        // The volume was writable and a write failed. Whatever is wrong is
        // happening now, so the first sentence has to be the action: a disk
        // that has started refusing writes often stops answering reads next.
        case 7: return "Switched to read-only after disk errors. Copy your files off this volume now, then check it in Windows with chkdsk /f."
        default: return "Mounted read-only."
        }
    }

    static var fileURL: URL? {
        guard let base = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: appGroup) else { return nil }
        let dir = base.appendingPathComponent("Library/Application Support", isDirectory: true)
        return dir.appendingPathComponent(fileName)
    }

    static func readAll() -> [String: MountStatus] { readAll(from: fileURL) }

    /// Takes the URL so the tests can point at a file they own; the container
    /// path is fixed and shared with whatever extension process is running.
    /// A missing or unreadable file is not an error and never throws: nothing
    /// has been mounted yet is the normal state, and a half-written file must
    /// leave the app showing no status rather than failing to start.
    static func readAll(from url: URL?) -> [String: MountStatus] {
        guard let url, let data = try? Data(contentsOf: url) else { return [:] }
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
