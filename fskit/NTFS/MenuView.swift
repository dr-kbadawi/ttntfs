// SPDX-License-Identifier: GPL-2.0

import SwiftUI

struct MenuView: View {
    @EnvironmentObject var monitor: VolumeMonitor
    @EnvironmentObject var status: ExtensionStatus
    @EnvironmentObject var enabler: ModuleEnabler
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
                enableFooter
            }
        }
    }

    /// The System Settings switch cannot turn a third-party FSKit module on
    /// (macOS 26.3 and 26.6.2), so we do it here instead. See ModuleEnabler.
    @ViewBuilder private var enableFooter: some View {
        switch enabler.outcome {
        case .working:
            HStack(spacing: 6) {
                ProgressView().controlSize(.small)
                Text("Enabling…").font(.caption).foregroundStyle(.secondary)
            }
        case .succeeded:
            VStack(alignment: .leading, spacing: 6) {
                if let note = enabler.remountNote {
                    Text(note).font(.caption).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if enabler.remountable.isEmpty {
                    Text("Enabled. Plug in an NTFS drive.")
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    // Enabling changes nothing for disks that are already
                    // mounted: Disk Arbitration only picks a module when it
                    // probes, and it probes on mount.
                    Text("\(enabler.remountable.map(\.name).formatted(.list(type: .and))) "
                         + "\(enabler.remountable.count == 1 ? "is" : "are") still mounted by the system. "
                         + "Remount to hand over to this driver.")
                        .font(.caption).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    Button(enabler.remountable.count == 1 ? "Remount Volume" : "Remount Volumes") {
                        Task { await enabler.remountAll(); monitor.start() }
                    }
                    .controlSize(.small)
                }
            }
        case .blocked(let why), .failed(let why):
            VStack(alignment: .leading, spacing: 6) {
                Text(why)
                    .font(.caption).foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
                enableButtons(title: "Try Again")
            }
        case .idle:
            VStack(alignment: .leading, spacing: 6) {
                Text("Turn it on here — the switch in System Settings cannot enable third-party file system extensions on this version of macOS.")
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                enableButtons(title: "Enable Extension")
            }
        }
    }

    private func enableButtons(title: String) -> some View {
        HStack {
            Button(title) {
                Task {
                    await enabler.enable()
                    status.refresh()
                }
            }
            Button("Re-check") { status.refresh() }
        }
        .controlSize(.small)
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
