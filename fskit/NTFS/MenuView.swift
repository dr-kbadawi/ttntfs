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
            if monitor.volumes.isEmpty && monitor.staleStatus.isEmpty {
                Text("No NTFS volumes mounted")
                    .foregroundStyle(.secondary)
                    .padding(.vertical, 4)
            } else {
                ForEach(monitor.volumes) { v in
                    VolumeRow(volume: v) { monitor.unmount(v) }
                }
                ForEach(monitor.staleStatus, id: \.bsdName) { s in
                    HStack(alignment: .top) {
                        Image(systemName: "externaldrive.badge.questionmark")
                        VStack(alignment: .leading) {
                            Text("\(s.label.isEmpty ? "NTFS" : s.label) (\(s.bsdName))").font(.body)
                            Text(s.readOnly ? s.roReasonText : "Activated by the extension; not listed by the system yet.")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    }
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
        .frame(width: 340)
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
        HStack(alignment: .top) {
            Image(systemName: volume.readOnly ? "externaldrive.badge.exclamationmark" : "externaldrive.fill")
                .foregroundStyle(volume.readOnly ? .orange : .primary)
            VStack(alignment: .leading, spacing: 2) {
                Text(volume.name).font(.body)
                Text("\(volume.device) · \(volume.readOnly ? "read-only" : "read/write")")
                    .font(.caption).foregroundStyle(.secondary)
                if volume.readOnly && !volume.reason.isEmpty {
                    Text(volume.reason)
                        .font(.caption)
                        .foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer()
            Button("Eject") { unmount() }.controlSize(.small)
        }
    }
}
