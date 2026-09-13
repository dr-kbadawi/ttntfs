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
                // Grouped by physical disk, because that is the unit you
                // unplug: each group carries the eject that makes the whole
                // device safe to remove, rather than one partition of it.
                ForEach(diskGroups, id: \.disk) { group in
                    if diskGroups.count > 1 || group.partitions.count > 1 {
                        DiskHeader(wholeDisk: group.disk,
                                   volumeCount: group.partitions.count,
                                   note: inventory.noteForDisk?.wholeDisk == group.disk
                                       ? inventory.noteForDisk?.text : nil,
                                   busy: inventory.busy,
                                   eject: { Task { await inventory.ejectDisk(group.disk); monitor.refresh() } })
                    }
                    ForEach(group.partitions) { partition in
                    PartitionRow(
                        partition: partition,
                        reason: partition.readOnlyReason.isEmpty
                            ? (monitor.volumes.first { $0.bsdName == partition.bsdName }?.reason ?? "")
                            : partition.readOnlyReason,
                        mount: { Task { await inventory.mount(partition); monitor.refresh() } },
                        eject: { Task { await inventory.unmount(partition); monitor.refresh() } },
                        discardHibernation: { confirmDiscard(partition) },
                        replayJournal: { confirmReplay(partition) },
                        note: inventory.note?.bsdName == partition.bsdName
                            ? inventory.note?.text : nil,
                        busy: inventory.busy,
                        handOver: {
                            Task { await inventory.handOver(partition); monitor.refresh() }
                        })
                    }
                }
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
        .task { monitor.start(); inventory.start(); status.refresh() }
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

    private func confirmReplay(_ partition: Partition) {
        guard Confirm.destructive(
            title: "Replay the journal on \(partition.displayName)?",
            message: "This finishes the writes Windows left unfinished, using a journal format "
                   + "this driver has worked out rather than one it has verified against Windows. "
                   + "If that reading is wrong it can damage the file system, and the damage may "
                   + "not be obvious straight away.\n\n"
                   + "Back up anything you cannot lose first. The safe alternative is to plug the "
                   + "disk into Windows and eject it properly.",
            proceed: "Replay and Mount Read/Write") else { return }
        Task { await inventory.replayJournal(partition); monitor.refresh() }
    }

    private func confirmDiscard(_ partition: Partition) {
        guard Confirm.destructive(
            title: "Discard the saved Windows session on \(partition.displayName)?",
            message: "Windows saved a suspended session on this volume with Fast Startup. "
                   + "Anything left open in Windows will be lost and Windows will start fresh "
                   + "next time.\n\nYour files are not affected.",
            proceed: "Discard and Mount Read/Write") else { return }
        Task { await inventory.discardHibernation(partition); monitor.refresh() }
    }

    /// Partitions grouped by the physical disk they live on, in a stable order.
    private var diskGroups: [(disk: String, partitions: [Partition])] {
        var order: [String] = []
        var byDisk: [String: [Partition]] = [:]
        for partition in inventory.partitions {
            if byDisk[partition.wholeDisk] == nil { order.append(partition.wholeDisk) }
            byDisk[partition.wholeDisk, default: []].append(partition)
        }
        return order.map { ($0, byDisk[$0] ?? []) }
    }

    @ViewBuilder private var header: some View {
        switch status.state {
        case .unknown:
            Label("Checking extension…", systemImage: "hourglass").foregroundStyle(.secondary)
        case .enabled:
            VStack(alignment: .leading, spacing: 6) {
                Label("File system extension enabled", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green)
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

/// One physical disk: the unit that gets unplugged, and the only level at which
/// "safe to remove" means anything.
struct DiskHeader: View {
    let wholeDisk: String
    let volumeCount: Int
    let note: String?
    let busy: Bool
    let eject: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Image(systemName: "externaldrive.connected.to.line.below")
                    .foregroundStyle(.secondary)
                Text(wholeDisk).font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Eject Disk", action: eject).controlSize(.small).disabled(busy)
            }
            if let note {
                Text(note).font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.top, 2)
    }
}

struct PartitionRow: View {
    let partition: Partition
    /// Why it is read-only, from the extension's own status file, when it knows.
    let reason: String
    let mount: () -> Void
    let eject: () -> Void
    let discardHibernation: () -> Void
    let replayJournal: () -> Void
    /// The result of the last action on *this* volume, shown in its own row.
    let note: String?
    let busy: Bool
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
                // Read-only analysis of the journal: what a replay would do.
                // Shown because it is the only thing we can offer for an
                // unclean journal, and it explains the read-only state
                // concretely rather than just naming it.
                if !partition.journalSummary.isEmpty {
                    Text(partition.journalSummary)
                        .font(.caption2).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if let note {
                    Text(note).font(.caption).foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer()
            action
        }
    }

    @ViewBuilder private var action: some View {
        actionButtons.disabled(busy)
    }

    @ViewBuilder private var actionButtons: some View {
        if !partition.isMounted {
            Button("Mount", action: mount).controlSize(.small)
        } else if partition.heldByAnotherDriver {
            VStack(alignment: .trailing, spacing: 4) {
                Button("Use This Driver", action: handOver).controlSize(.small)
                Button("Eject", action: eject).controlSize(.small)
            }
        } else if partition.mountedReadOnly && partition.servedByOurDriver
                    && partition.journalBlocked {
            VStack(alignment: .trailing, spacing: 4) {
                Button("Replay Journal…", action: replayJournal).controlSize(.small)
                Button("Eject", action: eject).controlSize(.small)
            }
        } else if partition.hibernated && partition.mountedReadOnly {
            VStack(alignment: .trailing, spacing: 4) {
                Button("Discard Session…", action: discardHibernation).controlSize(.small)
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
