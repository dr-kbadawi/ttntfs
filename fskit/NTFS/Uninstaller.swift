// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
//
// Removing the app by dragging it to the Trash is not enough: the module stays
// in fskit_agent's enabled list, the bundle stays registered with
// LaunchServices and PlugInKit — so System Settings keeps listing a file system
// extension that no longer exists — and the Login Item record is orphaned with
// nothing able to remove it, because only the app itself can call
// SMAppService.unregister(). So the app has to be able to remove itself.
//
// Everything here is the user's own state in their own home; nothing needs an
// administrator. The mirror of this is ModuleEnabler: what enabling wrote,
// uninstalling takes back.

import Foundation
import AppKit

@MainActor
final class Uninstaller: ObservableObject {
    enum State: Equatable {
        case idle
        case working(String)
        case failed(String)
    }

    @Published private(set) var state: State = .idle

    private let enabler: ModuleEnabler
    init(enabler: ModuleEnabler) { self.enabler = enabler }

    /// Returns only on failure; on success the app has been moved to the Trash
    /// and is terminating.
    func run() async {
        state = .working("Disabling the file system extension…")
        if let problem = await enabler.disable() {
            state = .failed("Could not disable the extension: \(problem). Eject your NTFS volumes and try again.")
            return
        }

        state = .working("Removing the login item…")
        LoginItem.shared.setEnabled(false)

        state = .working("Unregistering…")
        Self.unregisterFromLaunchServices()

        state = .working("Deleting settings…")
        Self.removeSupportFiles()

        state = .working("Moving to the Trash…")
        do {
            try await Self.trashSelf()
        } catch {
            state = .failed("Removed everything else, but could not move the app to the Trash: "
                            + "\(error.localizedDescription). Drag it there yourself.")
            return
        }
        NSApp.terminate(nil)
    }

    // MARK: Steps

    /// LaunchServices keeps a record per *copy* of the bundle it has seen. The
    /// record for a deleted bundle is dropped eventually, but not promptly, and
    /// until then the extension is still listed. `lsregister -u` is the only way
    /// to do this; there is no public API.
    private static func unregisterFromLaunchServices() {
        let bundle = Bundle.main.bundleURL
        let lsregister = "/System/Library/Frameworks/CoreServices.framework/Frameworks/"
            + "LaunchServices.framework/Support/lsregister"
        guard FileManager.default.isExecutableFile(atPath: lsregister) else { return }
        let task = Process()
        task.executableURL = URL(filePath: lsregister)
        task.arguments = ["-u", bundle.path]
        try? task.run()
        task.waitUntilExit()
    }

    private static func removeSupportFiles() {
        let home = FileManager.default.homeDirectoryForCurrentUser
        let appID = Bundle.main.bundleIdentifier ?? "ch.techtag.ntfs"
        let paths = [
            "Library/Group Containers/\(MountStatus.appGroup)",
            "Library/Containers/\(appID)",                       // from when the app was sandboxed
            "Library/Preferences/\(appID).plist",
            "Library/Caches/\(appID)",
            "Library/Saved Application State/\(appID).savedState",
            "Library/HTTPStorages/\(appID)",
        ]
        for path in paths {
            try? FileManager.default.removeItem(at: home.appending(path: path))
        }
    }

    /// Moving a running app to the Trash is fine: the bundle stays open through
    /// its inode until the process exits.
    private static func trashSelf() async throws {
        let bundle = Bundle.main.bundleURL
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            NSWorkspace.shared.recycle([bundle]) { _, error in
                if let error { c.resume(throwing: error) } else { c.resume() }
            }
        }
    }
}
