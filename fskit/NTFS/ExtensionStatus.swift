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

    private var order = RefreshOrder()

    func refresh() {
        let mine = order.start()
        Task {
            do {
                let modules = try await FSClient.shared.installedExtensions
                guard order.accept(mine) else { return }
                if let m = modules.first(where: { $0.bundleIdentifier == Self.extensionBundleID }) {
                    state = m.isEnabled ? .enabled : .disabled
                } else {
                    state = .notInstalled
                }
            } catch {
                NSLog("FSClient: %@", error.localizedDescription)
                let next = Self.stateAfterFailedRefresh(state)
                if next != state { state = next }
            }
        }
    }

    /// What a refresh that could not ask FSClient leaves. FSClient fails for a
    /// moment while fskit_agent restarts, and the menu refreshes every time it
    /// opens. An error is no answer, so what is known stays; only with nothing
    /// known yet is "not registered" the best guess.
    nonisolated static func stateAfterFailedRefresh(_ state: State) -> State {
        state == .unknown ? .notInstalled : state
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

/// Refreshes run on their own and can finish out of order: one started just
/// after `fskit_agent` was restarted can answer late. Each refresh takes a
/// number as it starts, and its answer is used only if no newer refresh has
/// answered already.
struct RefreshOrder {
    private var started = 0
    private var answered = 0

    mutating func start() -> Int {
        started += 1
        return started
    }

    /// Whether the answer of the refresh that took `ticket` is still news.
    mutating func accept(_ ticket: Int) -> Bool {
        guard ticket > answered else { return false }
        answered = ticket
        return true
    }
}
