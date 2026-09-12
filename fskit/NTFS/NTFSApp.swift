// SPDX-License-Identifier: GPL-2.0
//
// Host app: a menu-bar item that carries the FSKit extension, shows mounted
// NTFS volumes, and exposes the mount options.

import SwiftUI

@main
struct NTFSApp: App {
    @StateObject private var monitor = VolumeMonitor()
    @StateObject private var status = ExtensionStatus()

    var body: some Scene {
        MenuBarExtra("NTFS", systemImage: "externaldrive") {
            MenuView()
                .environmentObject(monitor)
                .environmentObject(status)
        }
        .menuBarExtraStyle(.window)

        Settings {
            SettingsView()
        }
    }
}
