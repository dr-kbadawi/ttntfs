// SPDX-License-Identifier: GPL-2.0

import SwiftUI

struct MenuView: View {
    @EnvironmentObject var monitor: VolumeMonitor
    @EnvironmentObject var status: ExtensionStatus
    @Environment(\.openSettings) private var openSettings

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            header
            Divider()
            if monitor.volumes.isEmpty {
                Text("No NTFS volumes mounted")
                    .foregroundStyle(.secondary)
                    .padding(.vertical, 4)
            } else {
                ForEach(monitor.volumes) { v in
                    VolumeRow(volume: v) { monitor.unmount(v) }
                }
            }
            Divider()
            HStack {
                Button("Settings…") { openSettings() }
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }
            }
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)
        }
        .padding(12)
        .frame(width: 320)
        .task { monitor.start(); status.refresh() }
    }

    @ViewBuilder private var header: some View {
        switch status.state {
        case .unknown:
            Label("Checking extension…", systemImage: "hourglass").foregroundStyle(.secondary)
        case .enabled:
            Label("File system extension enabled", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
        case .disabled, .notInstalled:
            VStack(alignment: .leading, spacing: 6) {
                Label(status.state == .disabled ? "Extension is disabled" : "Extension not registered",
                      systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
                Text("Enable it under System Settings → General → Login Items & Extensions → File System Extensions, then plug in an NTFS drive.")
                    .font(.caption).foregroundStyle(.secondary)
                HStack {
                    Button("Open System Settings") { status.openSettings() }
                    Button("Re-check") { status.refresh() }
                }
                .controlSize(.small)
            }
        }
    }
}

struct VolumeRow: View {
    let volume: MountedVolume
    let unmount: () -> Void

    var body: some View {
        HStack {
            Image(systemName: "externaldrive.fill")
            VStack(alignment: .leading) {
                Text(volume.name).font(.body)
                Text("\(volume.device) · \(volume.readOnly ? "read-only" : "read/write")")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            Button("Eject") { unmount() }.controlSize(.small)
        }
    }
}
