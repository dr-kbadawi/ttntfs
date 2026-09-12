// SPDX-License-Identifier: GPL-2.0
//
// Whether our FSKit module is registered and enabled, via FSClient.

import Foundation
import AppKit
import FSKit

@MainActor
final class ExtensionStatus: ObservableObject {
    enum State { case unknown, notInstalled, disabled, enabled }
    @Published private(set) var state: State = .unknown

    static let extensionBundleID = "org.ntfsmac.NTFS.Extension"

    func refresh() {
        Task {
            do {
                let modules = try await FSClient.shared.installedExtensions
                if let m = modules.first(where: { $0.bundleIdentifier == Self.extensionBundleID }) {
                    state = m.isEnabled ? .enabled : .disabled
                } else {
                    state = .notInstalled
                }
            } catch {
                NSLog("FSClient: %@", error.localizedDescription)
                state = .notInstalled
            }
        }
    }

    func openSettings() {
        // Login Items & Extensions pane; the File System Extensions list is behind its (i) button.
        let urls = [
            "x-apple.systempreferences:com.apple.LoginItems-Settings.extension?ExtensionItems",
            "x-apple.systempreferences:com.apple.LoginItems-Settings.extension",
        ]
        for u in urls {
            if let url = URL(string: u), NSWorkspace.shared.open(url) { return }
        }
    }
}
