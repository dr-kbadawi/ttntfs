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
        if rc == -ENXIO {
            log.info("probe \(block.bsdName, privacy: .public): not NTFS")
            reply(.notRecognized, nil)
            return
        }
        if rc < 0 {
            log.error("probe \(block.bsdName, privacy: .public): rc \(rc)")
            reply(nil, posixError(rc))
            return
        }
        let name = withUnsafePointer(to: info.label) { p in
            p.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
        }
        let containerID = FSContainerIdentifier(uuid: NTFSFileSystem.uuid(fromSerial: info.serial))
        log.info("probe \(block.bsdName, privacy: .public): NTFS '\(name, privacy: .public)' dirty=\(info.dirty) hib=\(info.hibernated)")
        if info.dirty || info.hibernated || !info.logfile_clean {
            reply(.usableButLimited(name: name.isEmpty ? "NTFS" : name, containerID: containerID), nil)
        } else {
            reply(.usable(name: name.isEmpty ? "NTFS" : name, containerID: containerID), nil)
        }
    }

    // MARK: Load / unload

    func loadResource(resource: FSResource, options: FSTaskOptions,
                      replyHandler reply: @escaping (FSVolume?, Error?) -> Void) {
        guard let block = resource as? FSBlockDeviceResource else {
            reply(nil, fs_errorForPOSIXError(ENOTSUP))
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
            reply(nil, posixError(rc, "load: probe"))
            return
        }
        let name = withUnsafePointer(to: info.label) { p in
            p.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
        }
        var mountOptions = MountOptions.fromDefaults()
        mountOptions.apply(taskOptions: options)

        let volumeID = FSVolume.Identifier(uuid: NTFSFileSystem.uuid(fromSerial: info.serial))
        let volume = NTFSVolume(volumeID: volumeID,
                                volumeName: FSFileName(string: name.isEmpty ? "NTFS" : name),
                                device: dev, options: mountOptions)
        lock.withLock { volumes[block.bsdName] = volume }
        containerStatus = .ready
        log.info("load \(block.bsdName, privacy: .public): volume '\(name, privacy: .public)'")
        reply(volume, nil)
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions,
                        replyHandler reply: @escaping (Error?) -> Void) {
        if let block = resource as? FSBlockDeviceResource {
            let vol = lock.withLock { volumes.removeValue(forKey: block.bsdName) }
            vol?.releaseDevice()
            log.info("unload \(block.bsdName, privacy: .public)")
        }
        containerStatus = .notReady(status: fs_errorForPOSIXError(ENXIO))
        reply(nil)
    }

    func didFinishLoading() {
        log.debug("didFinishLoading")
    }

    // MARK: Check / format (FSManageableResourceMaintenanceOperations)

    /// The system runs a check before mounting block-device file systems. Until
    /// the core has a real fsck this validates the boot sector and reports the
    /// journal/dirty state; `-y` cannot repair anything yet.
    func startCheck(task: FSTask, options: FSTaskOptions) throws -> Progress {
        let progress = Progress(totalUnitCount: 1)
        // FSKit hands the resource to check via loadResource first; the volume
        // for it is the most recently loaded one.
        let volume = lock.withLock { volumes.values.first }
        DispatchQueue.global().async {
            defer { progress.completedUnitCount = 1 }
            guard let volume else {
                task.logMessage("ntfsx: no volume loaded")
                task.didComplete(error: fs_errorForPOSIXError(ENOENT))
                return
            }
            var info = ntfs_volume_info()
            let rc = ntfs_probe(volume.device, &info)
            if rc < 0 {
                task.logMessage("ntfsx: not a valid NTFS volume (\(rc))")
                task.didComplete(error: posixError(rc))
                return
            }
            if info.hibernated {
                task.logMessage("ntfsx: volume is hibernated (Windows Fast Startup); will mount read-only")
            } else if info.dirty || !info.logfile_clean {
                task.logMessage("ntfsx: volume was not cleanly unmounted; will mount read-only until checked by Windows")
            } else {
                task.logMessage("ntfsx: volume appears clean")
            }
            task.didComplete(error: nil)
        }
        return progress
    }

    func startFormat(task: FSTask, options: FSTaskOptions) throws -> Progress {
        task.logMessage("ntfsx: formatting is not supported yet")
        throw fs_errorForPOSIXError(ENOTSUP)
    }

    // MARK: Helpers

    /// Deterministic UUID from the 64-bit NTFS volume serial.
    static func uuid(fromSerial serial: UInt64) -> UUID {
        var bytes = [UInt8](repeating: 0, count: 16)
        let ns: [UInt8] = Array("ntfsx".utf8)              // namespace tag
        for (i, b) in ns.enumerated() { bytes[i] = b }
        for i in 0..<8 { bytes[8 + i] = UInt8((serial >> (8 * UInt64(7 - i))) & 0xff) }
        bytes[6] = (bytes[6] & 0x0f) | 0x40  // version 4-ish, keeps it a valid UUID
        bytes[8] = (bytes[8] & 0x3f) | 0x80
        return UUID(uuid: (bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                           bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]))
    }
}
