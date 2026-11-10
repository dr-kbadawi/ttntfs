// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
//
// Turns the FSKit module on, from the app.
//
// There are two paths, chosen by what the OS allows rather than by version
// alone.
//
// Up to macOS 26 the switch in System Settings → General → Login Items &
// Extensions → File System Extensions cannot enable a third-party module
// (verified on 26.3 and 26.6.2): its host, LoginItems.appex, talks to fskitd as
// an unentitled FSClient and fskitd answers EPERM. Asking it to enable one of
// Apple's own modules fails the same way, so this is not about our signature.
//
// What actually decides the matter there is fskit_agent's list, which it loads
// once at start and never re-reads:
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
// macOS 27 closes that path: the plist can no longer be read or written by an
// app without Apple's private entitlement, so the only supported route is to
// open System Settings and wait for FSClient to report the module enabled. The
// same fallback is taken on 26 when the read fails with a permission error,
// since Apple could extend the protection to a 26.x update.
//
// Neither path is documented, so nothing here assumes success: the result is
// read back from FSClient and reported honestly.

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
        /// The user has to finish enablement in System Settings. The string is
        /// the instruction to show, not an error.
        case needsSettings(String)
        case failed(String)
    }

    @Published private(set) var outcome: Outcome = .idle

    static let moduleID = "ch.techtag.ntfs.extension"

    /// macOS 27 no longer lets an app read or write fskit_agent's allowlist
    /// without Apple's private entitlement. The supported switch is in System
    /// Settings; the app can only open it and wait for FSKit to report the
    /// module as enabled.
    static var usesSystemSettingsOnly: Bool {
        ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27
    }

    static let systemSettingsHint = "This version of macOS enables third-party file systems only in System Settings → General → Login Items & Extensions → File System Extensions. Turn the extension on there; this app will notice when it is enabled."

    /// True while a poll loop is waiting for the user to finish in System
    /// Settings, which is not `.working` but still must not be re-entered.
    private var isWaitingForSettings = false

    private static var listURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appending(path: "Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist")
    }

    // MARK: Enable

    func enable() async {
        // A second press while enabling, or while waiting in System Settings,
        // would start a second poll loop.
        guard outcome != .working, !isWaitingForSettings else { return }
        outcome = .working

        if Self.usesSystemSettingsOnly {
            await requireSystemSettings()
            return
        }

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
            // A permission error means Apple has protected the list on this
            // build too, whatever the version says; the Settings route is then
            // the only one left.
            if Self.isPermissionDenied(error) {
                await requireSystemSettings()
                return
            }
            outcome = .failed("Could not update the FSKit module list: \(error.localizedDescription)")
            return
        }

        let killed = Self.restartAgent()

        // Read the result back rather than trusting the write.
        if await Self.waitUntilEnabled(timeout: .seconds(15)) {
            // Enabling changes nothing for disks that are already mounted:
            // Disk Arbitration only picks a module when it probes, and it
            // probes on mount. DiskInventory shows what can be handed over.
            outcome = .succeeded
        } else if killed == 0 {
            outcome = .failed("The module list was updated but fskit_agent was not running to restart. Log out and back in, then re-check.")
        } else {
            outcome = .failed("The module list was updated but FSKit still reports the extension as disabled.")
        }
    }

    /// Removes the module from fskit_agent's list and restarts the agent, the
    /// mirror of `enable()`. Used by the uninstaller; leaving the entry behind
    /// would keep the module listed in System Settings after the app is gone.
    func disable() async -> String? {
        if Self.usesSystemSettingsOnly {
            return await switchOffInSystemSettings()
        }
        if Self.moduleIsServingAVolume() {
            return "a volume this driver is serving is still mounted; eject it and try again"
        }
        do {
            try Self.removeFromEnabledList()
        } catch {
            // Same protection as the 27 path: if the app may not edit the list,
            // the Settings switch is the only way left.
            if Self.isPermissionDenied(error) {
                return await switchOffInSystemSettings()
            }
            return "could not update the FSKit module list: \(error.localizedDescription)"
        }
        Self.restartAgent()
        return nil
    }

    /// The one route left when the app may not edit the allowlist. Returns nil
    /// if the extension is already off — refusing there would leave the
    /// uninstaller stuck on a machine where the user has already switched it
    /// off — and otherwise opens Settings and explains what to do.
    private func switchOffInSystemSettings() async -> String? {
        guard await Self.isEnabled() else { return nil }
        ExtensionStatus.openSettings()
        return "this macOS build only turns the extension off in System Settings — switch it off there, then press Try Again"
    }

    private static func removeFromEnabledList() throws {
        let url = listURL
        guard FileManager.default.fileExists(atPath: url.path) else { return }
        var format = PropertyListSerialization.PropertyListFormat.binary
        let data = try Data(contentsOf: url)
        let plist = try PropertyListSerialization.propertyList(from: data, options: [], format: &format)
        guard var modules = plist as? [String], modules.contains(moduleID) else { return }
        modules.removeAll { $0 == moduleID }
        let out = try PropertyListSerialization.data(fromPropertyList: modules, format: format, options: 0)
        try out.write(to: url, options: .atomic)
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

    /// The macOS 27 route: open System Settings, then wait for the user to flip
    /// the switch there and for FSClient to report it.
    private func requireSystemSettings() async {
        isWaitingForSettings = true
        defer { isWaitingForSettings = false }

        ExtensionStatus.openSettings()
        outcome = .needsSettings(Self.systemSettingsHint)

        // Polling is cheap but not free — each round asks the FSKit service —
        // and the user has to find the pane and click, so two seconds is often
        // enough resolution.
        if await Self.waitUntilEnabled(timeout: .seconds(300), interval: .seconds(2)) {
            outcome = .succeeded
        }
    }

    /// Whether the failure is the OS refusing access to the list, which means
    /// the Settings route is the only one left.
    private static func isPermissionDenied(_ error: Error) -> Bool {
        let nsError = error as NSError
        switch nsError.domain {
        case NSPOSIXErrorDomain:
            return nsError.code == EPERM || nsError.code == EACCES
        case NSCocoaErrorDomain:
            return nsError.code == NSFileReadNoPermissionError
                || nsError.code == NSFileWriteNoPermissionError
        default:
            return false
        }
    }

    private static func waitUntilEnabled(timeout: Duration, interval: Duration = .milliseconds(500)) async -> Bool {
        let deadline = ContinuousClock.now.advanced(by: timeout)
        while ContinuousClock.now < deadline {
            if await isEnabled() { return true }
            try? await Task.sleep(for: interval)
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

    /// Whether one of our extension processes holds this specific device open,
    /// i.e. whether this volume is served by us rather than another driver.
    static func deviceIsServedByModule(_ device: String) -> Bool {
        let name = (device as NSString).lastPathComponent          // diskNsM
        return pids(named: "NTFSExtension").contains { pid in
            openDevicePaths(pid).contains { $0.hasSuffix("/" + name) || $0.hasSuffix("/r" + name) }
        }
    }

    private static func holdsBlockDevice(_ pid: pid_t) -> Bool {
        !openDevicePaths(pid).isEmpty
    }

    /// The /dev entries this process currently holds open.
    private static func openDevicePaths(_ pid: pid_t) -> [String] {
        let size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nil, 0)
        guard size > 0 else { return [] }
        var fds = [proc_fdinfo](repeating: proc_fdinfo(), count: Int(size) / MemoryLayout<proc_fdinfo>.size)
        let written = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, &fds, size)
        guard written > 0 else { return [] }

        var paths: [String] = []
        for entry in fds.prefix(Int(written) / MemoryLayout<proc_fdinfo>.size)
        where entry.proc_fdtype == UInt32(PROX_FDTYPE_VNODE) {
            var info = vnode_fdinfowithpath()
            let got = proc_pidfdinfo(pid, entry.proc_fd, PROC_PIDFDVNODEPATHINFO, &info,
                                     Int32(MemoryLayout<vnode_fdinfowithpath>.size))
            guard got > 0 else { continue }
            let path = withUnsafeBytes(of: &info.pvip.vip_path) { raw in
                String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
            }
            if path.hasPrefix("/dev/disk") || path.hasPrefix("/dev/rdisk") { paths.append(path) }
        }
        return paths
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
