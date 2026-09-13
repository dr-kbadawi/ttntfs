// SPDX-License-Identifier: GPL-2.0
//
// Host app: a menu-bar item that carries the FSKit extension, shows mounted
// NTFS volumes, and exposes the mount options.

import SwiftUI
import AppKit

/// A MenuBarExtra scene has no launch-time hook of its own, so the one-time
/// login-item registration hangs off the app delegate.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        Task { @MainActor in LoginItem.shared.registerOnFirstRun() }
    }
}

@main
struct NTFSApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var monitor = VolumeMonitor()
    @StateObject private var status = ExtensionStatus()
    @StateObject private var loginItem = LoginItem.shared

    var body: some Scene {
        MenuBarExtra("NTFS", systemImage: "externaldrive") {
            MenuView()
                .environmentObject(monitor)
                .environmentObject(status)
        }
        .menuBarExtraStyle(.window)

        Settings {
            SettingsView()
                .environmentObject(loginItem)
        }
    }
}
