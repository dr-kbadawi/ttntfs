// SPDX-License-Identifier: GPL-2.0

import SwiftUI

struct MenuView: View {
    @EnvironmentObject var monitor: VolumeMonitor
    @EnvironmentObject var status: ExtensionStatus
    @EnvironmentObject var enabler: ModuleEnabler
    @EnvironmentObject var inventory: DiskInventory
    @Environment(\.openSettings) private var openSettings

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            header
            Divider()
            // Every NTFS partition, mounted or not, so nothing is hidden: a
            // partition macOS declined to mount, a recovery partition, or a
            // volume another driver took are all actionable from here.
            if inventory.partitions.isEmpty {
                Text("No NTFS volumes or partitions found")
                    .foregroundStyle(.secondary)
                    .padding(.vertical, 4)
            } else {
                ForEach(inventory.partitions) { partition in
                    PartitionRow(
                        partition: partition,
                        reason: partition.readOnlyReason.isEmpty
                            ? (monitor.volumes.first { $0.bsdName == partition.bsdName }?.reason ?? "")
                            : partition.readOnlyReason,
                        mount: { Task { await inventory.mount(partition); monitor.refresh() } },
                        eject: { Task { await inventory.unmount(partition); monitor.refresh() } },
                        handOver: {
                            Task {
                                await enabler.remountAll()
                                inventory.refresh(); monitor.refresh(); status.refresh()
                            }
                        })
                }
            }
            if let note = inventory.note {
                Text(note).font(.caption).foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Divider()
            HStack {
                Button("Settings…") { showSettings() }
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }
            }
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)
        }
        .padding(12)
        .frame(width: 340)
        .task { monitor.start(); inventory.start(); status.refresh(); enabler.refreshRemountable() }
    }

    /// `openSettings()` alone does nothing useful in a menu-bar-only app: with
    /// LSUIElement the process is an accessory, so it is never activated and the
    /// window opens unfocused behind everything, or not visibly at all. Activate
    /// first, then bring the window forward once SwiftUI has created it.
    private func showSettings() {
        NSApp.activate(ignoringOtherApps: true)
        openSettings()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
            guard let window = NSApp.windows.first(where: {
                $0.isVisible && ($0.identifier?.rawValue.contains("Settings") == true
                                 || $0.styleMask.contains(.titled) && $0.level == .normal)
            }) else { return }
            window.makeKeyAndOrderFront(nil)
            window.orderFrontRegardless()
        }
    }

    @ViewBuilder private var header: some View {
        switch status.state {
        case .unknown:
            Label("Checking extension…", systemImage: "hourglass").foregroundStyle(.secondary)
        case .enabled:
            VStack(alignment: .leading, spacing: 6) {
                Label("File system extension enabled", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green)
                handoverOffer
            }
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
            Text("Enabled.").font(.caption).foregroundStyle(.secondary)
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

    /// Enabling does nothing to a disk that is already mounted — Disk
    /// Arbitration only picks a module when it probes, and it probes on mount.
    /// So this offer has to live outside the enable flow: the header flips to
    /// "enabled" the moment enabling succeeds, and anything shown only in the
    /// disabled branch would vanish with it.
    @ViewBuilder private var handoverOffer: some View {
        if let note = enabler.remountNote {
            Text(note).font(.caption).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        if !enabler.remountable.isEmpty {
            Text("\(enabler.remountable.map(\.name).formatted(.list(type: .and))) "
                 + "\(enabler.remountable.count == 1 ? "is" : "are") mounted by the system. "
                 + "Remount to hand over to this driver.")
                .font(.caption).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Button(enabler.remountable.count == 1 ? "Remount Volume" : "Remount Volumes") {
                Task { await enabler.remountAll(); monitor.start(); status.refresh() }
            }
            .controlSize(.small)
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

struct PartitionRow: View {
    let partition: Partition
    /// Why it is read-only, from the extension's own status file, when it knows.
    let reason: String
    let mount: () -> Void
    let eject: () -> Void
    let handOver: () -> Void

    var body: some View {
        HStack(alignment: .top) {
            Image(systemName: icon)
                .foregroundStyle(iconColour)
            VStack(alignment: .leading, spacing: 2) {
                Text(partition.displayName).font(.body)
                Text(subtitle).font(.caption).foregroundStyle(.secondary)
                if !detail.isEmpty {
                    Text(detail)
                        .font(.caption)
                        .foregroundStyle(partition.servedByOurDriver ? Color.secondary : Color.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer()
            action
        }
    }

    @ViewBuilder private var action: some View {
        if !partition.isMounted {
            Button("Mount", action: mount).controlSize(.small)
        } else if partition.heldByAnotherDriver {
            VStack(alignment: .trailing, spacing: 4) {
                Button("Use This Driver", action: handOver).controlSize(.small)
                Button("Eject", action: eject).controlSize(.small)
            }
        } else {
            Button("Eject", action: eject).controlSize(.small)
        }
    }

    private var icon: String {
        if !partition.isMounted { return "externaldrive.badge.questionmark" }
        if partition.heldByAnotherDriver { return "externaldrive.badge.exclamationmark" }
        return partition.mountedReadOnly ? "externaldrive.badge.exclamationmark" : "externaldrive.fill"
    }

    private var iconColour: Color {
        if !partition.isMounted { return .secondary }
        if partition.heldByAnotherDriver || partition.mountedReadOnly { return .orange }
        return .primary
    }

    private var subtitle: String {
        var parts = [partition.bsdName, partition.sizeDescription]
        if partition.isMounted {
            parts.append(partition.mountedReadOnly ? "read-only" : "read/write")
        } else {
            parts.append("not mounted")
        }
        return parts.joined(separator: " · ")
    }

    private var detail: String {
        if !partition.isMounted { return "" }
        if partition.heldByAnotherDriver { return "Mounted by the system, not by this driver." }
        return partition.mountedReadOnly ? reason : ""
    }
}
