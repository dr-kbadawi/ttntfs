// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
//
// "Open at login" via SMAppService.mainApp. Registering puts the app in
// System Settings → General → Login Items & Extensions → Open at Login, where
// the user can also switch it off; `.requiresApproval` is how that comes back.
//
// The driver itself does not need the app: fskit_agent launches the extension
// on demand when a volume is probed or mounted. Starting at login only means
// the menu-bar status and the volume list are there from the start.

import Foundation
import ServiceManagement

@MainActor
final class LoginItem: ObservableObject {
    static let shared = LoginItem()

    /// Set the first time we register, so that switching it off afterwards sticks.
    private static let didRegisterOnceKey = "didRegisterLoginItemOnce"

    @Published private(set) var isEnabled = false
    /// The user turned it off in System Settings; `register()` cannot override that.
    @Published private(set) var requiresApproval = false
    @Published private(set) var lastError: String?

    private init() { refresh() }

    func refresh() {
        let status = SMAppService.mainApp.status
        isEnabled = status == .enabled
        requiresApproval = status == .requiresApproval
    }

    /// Start at login by default, once. A user who switches it off is not overruled
    /// on the next launch.
    func registerOnFirstRun() {
        let store = SharedSettings.store
        guard !store.bool(forKey: Self.didRegisterOnceKey) else { refresh(); return }
        store.set(true, forKey: Self.didRegisterOnceKey)
        setEnabled(true)
    }

    func setEnabled(_ on: Bool) {
        do {
            switch (on, SMAppService.mainApp.status) {
            case (true, .enabled), (false, .notRegistered), (false, .notFound):
                break                                   // already in the wanted state
            case (true, _):
                try SMAppService.mainApp.register()
            case (false, _):
                try SMAppService.mainApp.unregister()
            }
            lastError = nil
        } catch {
            lastError = error.localizedDescription
            NSLog("login item %@ failed: %@", on ? "register" : "unregister",
                  error.localizedDescription)
        }
        refresh()
    }
}
