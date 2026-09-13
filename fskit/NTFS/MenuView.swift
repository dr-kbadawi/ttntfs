// SPDX-License-Identifier: GPL-2.0

import SwiftUI

struct MenuView: View {
    @EnvironmentObject var monitor: VolumeMonitor
    @EnvironmentObject var status: ExtensionStatus
    @EnvironmentObject var enabler: ModuleEnabler
    @EnvironmentObject var inventory: DiskInventory
    @State private var discardTarget: Partition?
    @State private var replayTarget: Partition?
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
                        discardHibernation: { discardTarget = partition },
                        replayJournal: { replayTarget = partition },
                        busy: inventory.busy,
                        handOver: {
                            Task { await inventory.handOver(partition); monitor.refresh() }
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
        .confirmationDialog("Replay the journal on \(replayTarget?.displayName ?? "")?",
                            isPresented: Binding(get: { replayTarget != nil },
                                                 set: { if !$0 { replayTarget = nil } })) {
            Button("Replay and Mount Read/Write", role: .destructive) {
                if let target = replayTarget {
                    Task { await inventory.replayJournal(target); monitor.refresh() }
                }
                replayTarget = nil
            }
            Button("Cancel", role: .cancel) { replayTarget = nil }
        } message: {
            Text("This finishes the writes Windows left unfinished, using a journal format "
                 + "this driver has worked out rather than one it has verified against Windows. "
                 + "If that reading is wrong it can damage the file system, and the damage may "
                 + "not be obvious straight away. Back up anything you cannot lose first. "
                 + "The safe alternative is to plug the disk into Windows and eject it properly.")
        }
        .confirmationDialog("Discard the saved Windows session on \(discardTarget?.displayName ?? "")?",
                            isPresented: Binding(get: { discardTarget != nil },
                                                 set: { if !$0 { discardTarget = nil } })) {
            Button("Discard and Mount Read/Write", role: .destructive) {
                if let target = discardTarget {
                    Task { await inventory.discardHibernation(target); monitor.refresh() }
                }
                discardTarget = nil
            }
            Button("Cancel", role: .cancel) { discardTarget = nil }
        } message: {
            Text("Windows saved a suspended session on this volume with Fast Startup. "
                 + "Anything left open in Windows will be lost and Windows will start fresh next time. "
                 + "Your files are not affected.")
        }
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
    /// The offer therefore lives outside the enable flow, and comes from the
    /// inventory rather than a separate list that could disagree with it.
    @ViewBuilder private var handoverOffer: some View {
        let waiting = inventory.partitions.filter(\.heldByAnotherDriver)
        if !waiting.isEmpty {
            Text("\(waiting.map(\.displayName).formatted(.list(type: .and))) "
                 + "\(waiting.count == 1 ? "is" : "are") mounted by the system. "
                 + "Hand over to use this driver.")
                .font(.caption).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Button(waiting.count == 1 ? "Use This Driver" : "Use This Driver for All") {
                Task { await inventory.handOverAll(); monitor.refresh(); status.refresh() }
            }
            .controlSize(.small)
            .disabled(inventory.busy)
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
    let discardHibernation: () -> Void
    let replayJournal: () -> Void
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
