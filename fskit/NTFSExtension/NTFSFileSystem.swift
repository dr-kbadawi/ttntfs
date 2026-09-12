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

        var info = ntfs_volume_info()
        let rc = ntfs_probe(dev, &info)
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
        log.info("probe \(block.bsdName, privacy: .public): NTFS '\(name, privacy: .public)' v\(info.major_ver).\(info.minor_ver) dirty=\(info.dirty) hib=\(info.hibernated) log=\(info.logfile_clean)")
        // "usableButLimited": recognized, but we will only offer read-only access.
        if info.dirty || info.hibernated || !info.logfile_clean || !block.isWritable {
            reply(.usableButLimited(name: name.isEmpty ? "NTFS" : name, containerID: containerID), nil)
        } else {
            reply(.usable(name: name.isEmpty ? "NTFS" : name, containerID: containerID), nil)
        }
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
        var info = ntfs_volume_info()
        let rc = ntfs_probe(dev, &info)
        if rc < 0 {
            ntfs_bdev_fskit_close(dev)
            containerStatus = .notReady(status: posixError(rc, "load: probe"))
            reply(nil, posixError(rc, "load \(block.bsdName)"))
            return
        }
        let name = NTFSFileSystem.label(from: info)
        var mountOptions = MountOptions.fromDefaults()
        mountOptions.apply(taskOptions: options)
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
