// SPDX-License-Identifier: GPL-2.0
//
// Turns the FSKit module on without System Settings.
//
// The switch in System Settings → General → Login Items & Extensions → File
// System Extensions cannot enable a third-party module on macOS 26 (verified on
// 26.3 and 26.6.2): its host, LoginItems.appex, talks to fskitd as an
// unentitled FSClient and fskitd answers EPERM. Asking it to enable one of
// Apple's own modules fails the same way, so this is not about our signature.
//
// What actually decides the matter is fskit_agent's list, which it loads once at
// start and never re-reads:
//
//     ~/Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist
//
// So: add the bundle ID, then restart the agent. SIGTERM is ignored and
// `launchctl kickstart` is refused, but SIGKILL works and launchd respawns the
// agent on the next FSKit request. The PlugInKit election (`pluginkit -e use`)
// is *not* needed — verified 2026-09-13 by setting it to `ignore` and watching
// FSKit still report the module enabled and Disk Arbitration still mount with it.
//
// All of this is per-user state under the user's own home, which is why it
// belongs in the app rather than an installer, and why it needs no admin
// password. It does require the app to be non-sandboxed: the list lives in an
// Apple-restricted app group a sandboxed process cannot touch, and signalling
// another of the user's processes is likewise out of reach under the sandbox.
//
// It is also undocumented, so nothing here assumes success: the result is read
// back from FSClient and reported honestly.

import Foundation
import FSKit
import Darwin

@MainActor
final class ModuleEnabler: ObservableObject {
    enum Outcome: Equatable {
        case idle
        case working
        case succeeded
        /// Refused on purpose, with the reason to show the user.
        case blocked(String)
        case failed(String)
    }

    @Published private(set) var outcome: Outcome = .idle

    static let moduleID = "ch.techtag.ntfs.extension"

    private static var listURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appending(path: "Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist")
    }

    // MARK: Enable

    func enable() async {
        outcome = .working

        // Restarting the agent drops volumes *our* module is serving. Volumes
        // mounted by Apple's ntfs driver survive it — verified 2026-09-13 on
        // 26.6.2 — which matters because the common case is a user plugging in
        // an NTFS disk, macOS mounting it read-only, and only then installing
        // this app. Refusing there would be wrong, so the test is narrow.
        if Self.moduleIsServingAVolume() {
            outcome = .blocked("Eject the NTFS volume this driver is serving first — enabling restarts a system service that would drop it.")
            return
        }

        do {
            try Self.addToEnabledList()
        } catch {
            outcome = .failed("Could not update the FSKit module list: \(error.localizedDescription)")
            return
        }

        let killed = Self.restartAgent()

        // Read the result back rather than trusting the write.
        if await Self.waitUntilEnabled(timeout: .seconds(15)) {
            outcome = .succeeded
        } else if killed == 0 {
            outcome = .failed("The module list was updated but fskit_agent was not running to restart. Log out and back in, then re-check.")
        } else {
            outcome = .failed("The module list was updated but FSKit still reports the extension as disabled.")
        }
    }

    // MARK: Steps

    /// Adds our bundle ID to fskit_agent's enabled list, preserving the file's format.
    private static func addToEnabledList() throws {
        let url = listURL
        var format = PropertyListSerialization.PropertyListFormat.binary
        var modules: [String] = []

        if FileManager.default.fileExists(atPath: url.path) {
            let data = try Data(contentsOf: url)
            let plist = try PropertyListSerialization.propertyList(from: data, options: [], format: &format)
            modules = plist as? [String] ?? []
        } else {
            // The list appears the first time System Settings shows the pane. If it
            // is absent, creating it with the Apple defaults plus ours is correct:
            // fskit_agent merges it with what is registered.
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
        }

        guard !modules.contains(moduleID) else { return }
        modules.append(moduleID)
        let out = try PropertyListSerialization.data(fromPropertyList: modules, format: format, options: 0)
        try out.write(to: url, options: .atomic)
    }

    /// SIGKILLs every fskit_agent belonging to this user; launchd brings it back
    /// on demand with the list reloaded. Returns how many were signalled.
    @discardableResult
    private static func restartAgent() -> Int {
        let agents = pids(named: "fskit_agent")
        for pid in agents { kill(pid, SIGKILL) }
        return agents.count
    }

    private static func waitUntilEnabled(timeout: Duration) async -> Bool {
        let deadline = ContinuousClock.now.advanced(by: timeout)
        while ContinuousClock.now < deadline {
            if await isEnabled() { return true }
            try? await Task.sleep(for: .milliseconds(500))
        }
        return false
    }

    static func isEnabled() async -> Bool {
        do {
            let modules = try await FSClient.shared.installedExtensions
            return modules.first { $0.bundleIdentifier == moduleID }?.isEnabled ?? false
        } catch {
            return false
        }
    }

    // MARK: System queries

    /// Whether one of our extension processes is currently serving a mounted
    /// volume. The mount table cannot answer this — our mounts look exactly like
    /// Apple's "ntfs ... fskit" — and the module process lingers indefinitely
    /// after an unmount, so neither is usable. What does track it is the block
    /// device: the module holds one descriptor on /dev/diskN while serving and
    /// none when idle.
    static func moduleIsServingAVolume() -> Bool {
        pids(named: "NTFSExtension").contains(where: holdsBlockDevice)
    }

    private static func holdsBlockDevice(_ pid: pid_t) -> Bool {
        let size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nil, 0)
        guard size > 0 else { return false }
        var fds = [proc_fdinfo](repeating: proc_fdinfo(), count: Int(size) / MemoryLayout<proc_fdinfo>.size)
        let written = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, &fds, size)
        guard written > 0 else { return false }

        for entry in fds.prefix(Int(written) / MemoryLayout<proc_fdinfo>.size)
        where entry.proc_fdtype == UInt32(PROX_FDTYPE_VNODE) {
            var info = vnode_fdinfowithpath()
            let got = proc_pidfdinfo(pid, entry.proc_fd, PROC_PIDFDVNODEPATHINFO, &info,
                                     Int32(MemoryLayout<vnode_fdinfowithpath>.size))
            guard got > 0 else { continue }
            let path = withUnsafeBytes(of: &info.pvip.vip_path) { raw in
                String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
            }
            if path.hasPrefix("/dev/disk") || path.hasPrefix("/dev/rdisk") { return true }
        }
        return false
    }

    /// Mount points of every mounted NTFS volume, whoever mounted it. Used only
    /// for reporting, never to decide whether enabling is safe.
    static func mountedNTFSVolumes() -> [String] {
        var buffer: UnsafeMutablePointer<statfs>?
        let count = getmntinfo(&buffer, MNT_NOWAIT)
        guard count > 0, let buffer else { return [] }
        return (0..<Int(count)).compactMap { i in
            var entry = buffer[i]
            let type = withUnsafeBytes(of: &entry.f_fstypename) { raw in
                String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
            }
            guard type == "ntfs" else { return nil }
            return withUnsafeBytes(of: &entry.f_mntonname) { raw in
                String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
            }
        }
    }

    /// PIDs of the current user's processes with this executable name.
    private static func pids(named name: String) -> [pid_t] {
        let capacity = proc_listallpids(nil, 0)
        guard capacity > 0 else { return [] }
        var pids = [pid_t](repeating: 0, count: Int(capacity))
        let written = proc_listallpids(&pids, capacity * Int32(MemoryLayout<pid_t>.size))
        guard written > 0 else { return [] }

        let me = getuid()
        return pids.prefix(Int(written)).filter { pid in
            guard pid > 0 else { return false }
            var info = proc_bsdshortinfo()
            let size = proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &info,
                                   Int32(MemoryLayout<proc_bsdshortinfo>.size))
            guard size == Int32(MemoryLayout<proc_bsdshortinfo>.size), info.pbsi_uid == me else {
                return false
            }
            return withUnsafeBytes(of: &info.pbsi_comm) { raw in
                String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
            } == name
        }
    }
}
