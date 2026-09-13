// SPDX-License-Identifier: GPL-2.0
//
// Host app: a menu-bar item that carries the FSKit extension, shows mounted
// NTFS volumes, and exposes the mount options.

import SwiftUI
import AppKit

/// A MenuBarExtra scene has no launch-time hook of its own, so the one-time
/// login-item registration hangs off the app delegate.
final class AppDelegate: NSObject, NSApplicationDelegate {
    /// Only the app can remove its own Login Item record — Background Task
    /// Management has no per-item command line — so the uninstaller invokes the
    /// binary with this flag before deleting the bundle. Without it the record
    /// is orphaned in System Settings and has to be deleted by hand.
    static let unregisterFlag = "--unregister-login-item"

    func applicationDidFinishLaunching(_ notification: Notification) {
        if CommandLine.arguments.contains(Self.unregisterFlag) {
            Task { @MainActor in
                LoginItem.shared.setEnabled(false)
                NSApp.terminate(nil)
            }
            return
        }
        Task { @MainActor in LoginItem.shared.registerOnFirstRun() }
    }
}

@main
struct NTFSApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var monitor = VolumeMonitor()
    @StateObject private var status = ExtensionStatus()
    @StateObject private var loginItem = LoginItem.shared
    @StateObject private var enabler = ModuleEnabler()
    @StateObject private var uninstaller: Uninstaller
    @StateObject private var inventory = DiskInventory()

    init() {
        let enabler = ModuleEnabler()
        _enabler = StateObject(wrappedValue: enabler)
        _uninstaller = StateObject(wrappedValue: Uninstaller(enabler: enabler))
    }

    var body: some Scene {
        MenuBarExtra("NTFS", systemImage: "externaldrive") {
            MenuView()
                .environmentObject(monitor)
                .environmentObject(status)
                .environmentObject(enabler)
                .environmentObject(inventory)
        }
        .menuBarExtraStyle(.window)

        Settings {
            SettingsView()
                .environmentObject(loginItem)
                .environmentObject(uninstaller)
        }
    }
}
