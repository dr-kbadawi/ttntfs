// SPDX-License-Identifier: GPL-2.0
//
// FSUnaryFileSystem: probe / load / unload / check / format.

import Foundation
import FSKit

final class NTFSFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations,
                            FSManageableResourceMaintenanceOperations {
    static let shared = NTFSFileSystem()

    private let lock = NSLock()
    private var volumes: [String: NTFSVolume] = [:]   // by BSD name

    override init() {
        super.init()
        installCoreLogger()
        log.info("NTFS extension starting, core \(String(cString: ntfs_core_version()), privacy: .public)")
    }

    private static func label(from info: ntfs_volume_info) -> String {
        withUnsafePointer(to: info.label) { p in
            p.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
        }
    }

    // MARK: Probe

    func probeResource(resource: FSResource,
                       replyHandler reply: @escaping (FSProbeResult?, Error?) -> Void) {
        guard let block = resource as? FSBlockDeviceResource else {
            reply(.notRecognized, nil)
            return
        }
        guard let dev = ntfs_bdev_fskit_create(block) else {
            reply(nil, posixError(ENOMEM, "probe: bdev"))
            return
        }
        defer { ntfs_bdev_fskit_close(dev) }

        // Light: boot sector plus $Volume, no mount. DA probes twice per mount
        // and the full probe mounts and unmounts, which on a 1 TB disk is about
        // two seconds each. Nothing here needs free space, the hibernation state
        // or the journal state; the mount decides all three for itself.
        var info = ntfs_volume_info()
        let rc = ntfs_probe_light(dev, &info)
        if hostErrno(rc) == ENXIO && rc < 0 {
            log.info("probe \(block.bsdName, privacy: .public): not NTFS")
            reply(.notRecognized, nil)
            return
        }
        if rc < 0 {
            reply(nil, posixError(rc, "probe \(block.bsdName)"))
            return
        }
        let name = NTFSFileSystem.label(from: info)
        let containerID = FSContainerIdentifier(uuid: NTFSFileSystem.uuid(fromSerial: info.serial))
        // Only what the light probe actually read: hibernation and journal state
        // need the mounted volume and are reported by activate().
        log.info("probe \(block.bsdName, privacy: .public): NTFS '\(name, privacy: .public)' v\(info.major_ver).\(info.minor_ver) dirty=\(info.dirty)")
        // Always `.usable`. Disk Arbitration (DASupport.m, DAProbeWithFSKit) maps
        // only FSMatchResultUsable to success; notRecognized is ENOENT and
        // everything else, `usableButLimited` included, is EIO, after which the
        // disk falls through to Apple's read-only ntfs driver. The probe handle
        // DA gives us is always opened read-only, so `block.isWritable` says
        // nothing about the media, and dirty/hibernated volumes are handled by
        // the read-only fallback in NTFSVolume.activate.
        reply(.usable(name: name.isEmpty ? "NTFS" : name, containerID: containerID), nil)
    }

    // MARK: Load / unload

    /// Wraps the device and creates the volume object. The core is mounted in
    /// NTFSVolume.activate (FSKit activates before it mounts). Options here are
    /// FSKit's own: `--rdonly` (kernel wants MNT_RDONLY) and `-f`.
    func loadResource(resource: FSResource, options: FSTaskOptions,
                      replyHandler reply: @escaping (FSVolume?, Error?) -> Void) {
        guard let block = resource as? FSBlockDeviceResource else {
            reply(nil, fsError(ENOTSUP))
            return
        }
        guard let dev = ntfs_bdev_fskit_create(block) else {
            reply(nil, posixError(ENOMEM, "load: bdev"))
            return
        }
        // Light again: this only needs the label and the serial. activate()
        // mounts the volume straight afterwards and learns the rest there.
        var info = ntfs_volume_info()
        let rc = ntfs_probe_light(dev, &info)
        if rc < 0 {
            ntfs_bdev_fskit_close(dev)
            containerStatus = .notReady(status: posixError(rc, "load: probe"))
            reply(nil, posixError(rc, "load \(block.bsdName)"))
            return
        }
        let name = NTFSFileSystem.label(from: info)
        var mountOptions = MountOptions.fromDefaults()
        mountOptions.apply(taskOptions: options)
        // A one-shot request from the app, consumed here so it cannot apply
        // twice. The core still refuses unless the volume is otherwise clean.
        if MountOptions.takeJournalReplayRequest(for: block.bsdName) {
            mountOptions.replayJournal = true
            log.notice("\(block.bsdName, privacy: .public): replaying the journal at the user's request")
        }
        if MountOptions.takeHibernationDiscardRequest(for: block.bsdName) {
            mountOptions.discardHibernation = true
            log.notice("\(block.bsdName, privacy: .public): discarding the saved Windows hibernation image at the user's request")
        }
        if mountOptions.kernelReadOnly { _ = ntfs_bdev_fskit_set_read_only(dev, true) }

        let volumeID = FSVolume.Identifier(uuid: NTFSFileSystem.uuid(fromSerial: info.serial))
        let volume = NTFSVolume(volumeID: volumeID,
                                volumeName: FSFileName(string: name.isEmpty ? "NTFS" : name),
                                device: dev, bsdName: block.bsdName, label: name, options: mountOptions)
        let previous = lock.withLock { volumes.updateValue(volume, forKey: block.bsdName) }
        previous?.releaseDevice()
        containerStatus = .ready
        log.info("load \(block.bsdName, privacy: .public): volume '\(name, privacy: .public)' options \(options.taskOptions, privacy: .public)")
        reply(volume, nil)
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions,
                        replyHandler reply: @escaping (Error?) -> Void) {
        if let block = resource as? FSBlockDeviceResource {
            let vol = lock.withLock { volumes.removeValue(forKey: block.bsdName) }
            vol?.releaseDevice()
            log.info("unload \(block.bsdName, privacy: .public)")
        }
        containerStatus = .notReady(status: fsError(ENXIO))
        reply(nil)
    }

    func didFinishLoading() {
        log.debug("didFinishLoading")
    }

    // MARK: Check / format (FSManageableResourceMaintenanceOperations)

    /// The system runs a check before mounting block-device file systems. Until
    /// the core has a real fsck this validates the boot sector and reports the
    /// journal/dirty state; `-y` cannot repair anything yet. Exit status:
    /// clean -> success; dirty/hibernated -> success with a message (the mount
    /// will be read-only); not NTFS -> error.
    func startCheck(task: FSTask, options: FSTaskOptions) throws -> Progress {
        let progress = Progress(totalUnitCount: 1)
        // FSKit hands the resource to check via loadResource first; a unary
        // file system has exactly one.
        let volume = lock.withLock { volumes.values.first }
        let quick = options.taskOptions.contains("-q")
        DispatchQueue.global().async {
            defer { progress.completedUnitCount = 1 }
            guard let volume else {
                task.logMessage("ttntfs: no volume loaded")
                task.didComplete(error: fsError(ENOENT))
                return
            }
            var info = ntfs_volume_info()
            let rc = ntfs_probe(volume.device, &info)
            if rc < 0 {
                task.logMessage("ttntfs: not a valid NTFS volume (errno \(hostErrno(rc)))")
                task.didComplete(error: posixError(rc, "check"))
                return
            }
            let label = NTFSFileSystem.label(from: info)
            task.logMessage("ttntfs: NTFS \(info.major_ver).\(info.minor_ver) '\(label)', cluster \(info.cluster_size), \(info.total_clusters) clusters")
            if info.hibernated {
                task.logMessage("ttntfs: volume is hibernated (Windows Fast Startup); it will mount read-only")
            } else if info.dirty {
                task.logMessage("ttntfs: volume is marked dirty; it will mount read-only until chkdsk runs in Windows")
            } else if !info.logfile_clean {
                task.logMessage("ttntfs: $LogFile is not clean; it will mount read-only (journal replay is not enabled yet)")
            } else {
                task.logMessage(quick ? "ttntfs: quick check: volume appears clean" : "ttntfs: volume appears clean")
            }
            // Journal analysis: read-only, so it is safe to run on anything and
            // is the only way to see what replay *would* do while replay itself
            // stays refused (docs/LOGFILE.md). Skipped for a quick check.
            if !quick {
                var analysis = ntfs_logfile_analysis()
                let rc = ntfs_logfile_analyse(volume.device, &analysis)
                if rc < 0 {
                    task.logMessage("ttntfs: journal could not be read (errno \(hostErrno(rc)))")
                } else {
                    let state = withUnsafeBytes(of: &analysis.state) { raw in
                        String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
                    }
                    let message = withUnsafeBytes(of: &analysis.message) { raw in
                        String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
                    }
                    task.logMessage("ttntfs: journal \(state) (v\(analysis.log_version_major).\(analysis.log_version_minor))")
                    task.logMessage("ttntfs: \(message)")
                    if !analysis.clean && analysis.supported && !analysis.needs_chkdsk {
                        task.logMessage("ttntfs: this is an analysis only — nothing was written. "
                                        + "Replay is not enabled: the log layout it relies on has not "
                                        + "yet been checked against a journal Windows wrote.")
                    }
                }
            }
            task.didComplete(error: nil)
        }
        return progress
    }

    func startFormat(task: FSTask, options: FSTaskOptions) throws -> Progress {
        task.logMessage("ttntfs: formatting is not supported yet (use mkntfs from tools/)")
        throw fsError(ENOTSUP)
    }

    // MARK: Helpers

    /// Deterministic UUID from the 64-bit NTFS volume serial.
    static func uuid(fromSerial serial: UInt64) -> UUID {
        var bytes = [UInt8](repeating: 0, count: 16)
        let ns: [UInt8] = Array("ttntfs".utf8)              // namespace tag
        for (i, b) in ns.enumerated() { bytes[i] = b }
        for i in 0..<8 { bytes[8 + i] = UInt8((serial >> (8 * UInt64(7 - i))) & 0xff) }
        bytes[6] = (bytes[6] & 0x0f) | 0x40  // version 4-ish, keeps it a valid UUID
        bytes[8] = (bytes[8] & 0x3f) | 0x80
        return UUID(uuid: (bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                           bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]))
    }
}
