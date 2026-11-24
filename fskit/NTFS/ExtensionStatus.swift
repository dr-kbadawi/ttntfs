// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
//
// Whether our FSKit module is registered and enabled, via FSClient.

import Foundation
import AppKit
import FSKit

@MainActor
final class ExtensionStatus: ObservableObject {
    enum State { case unknown, notInstalled, disabled, enabled }
    @Published private(set) var state: State = .unknown

    static let extensionBundleID = "ch.techtag.ntfs.extension"

    /// Refreshes run on their own and can finish out of order: one started
    /// just after `fskit_agent` was restarted can answer late. Each answer is
    /// used only if no newer refresh has answered already.
    private var started = 0
    private var answered = 0

    func refresh() {
        started += 1
        let mine = started
        Task {
            do {
                let modules = try await FSClient.shared.installedExtensions
                guard mine > answered else { return }
                answered = mine
                if let m = modules.first(where: { $0.bundleIdentifier == Self.extensionBundleID }) {
                    state = m.isEnabled ? .enabled : .disabled
                } else {
                    state = .notInstalled
                }
            } catch {
                NSLog("FSClient: %@", error.localizedDescription)
                // FSClient fails for a moment while fskit_agent restarts, and
                // the menu refreshes every time it opens. An error is no
                // answer, so keep what is known; only with nothing known yet
                // is "not registered" the best guess.
                if state == .unknown { state = .notInstalled }
            }
        }
    }

    static func openSettings() {
        // Login Items & Extensions pane. On 26 the File System Extensions list is behind its (i)
        // button; on 27 it is under Extensions → By Category (the By App switch does not stay on).
        let urls = [
            "x-apple.systempreferences:com.apple.LoginItems-Settings.extension?ExtensionItems",
            "x-apple.systempreferences:com.apple.LoginItems-Settings.extension",
        ]
        for u in urls {
            if let url = URL(string: u), NSWorkspace.shared.open(url) { return }
        }
    }
}
