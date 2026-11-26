// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH

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
            UpdateNotice()      // empty unless a newer version is out (UpdateCheck.swift)
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
                                       ? inventory.noteForDisk : nil,
                                   busy: inventory.busy,
                                   hasMountedVolumes: inventory.disksWithMountedVolumes.contains(group.disk),
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
                        mountReadWriteAnyway: { confirmClearDirty(partition) },
                        note: inventory.note?.bsdName == partition.bsdName
                            ? inventory.note : nil,
                        busy: inventory.busy,
                        handOver: {
                            Task { await inventory.handOver(partition); monitor.refresh() }
                        })
                    }
                }
            }
            if showStarLine {
                StarLineRow(retire: { inventory.retireStarLine() })
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
        // Nothing promises that `.task` runs again each time the panel reopens,
        // and the extension can be switched off in System Settings while the
        // app runs (macOS 27 allows it), so read its state whenever one of the
        // app's windows becomes key, as the menu panel does when it opens.
        .onReceive(NotificationCenter.default.publisher(for: NSWindow.didBecomeKeyNotification)) { _ in
            status.refresh()
        }
        .onChange(of: status.state) { _, new in
            if new == .enabled { enabler.settle() }
        }
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

    /*
     * Two situations wear the same "journal is not clean" label, and only one of
     * them is dangerous.
     *
     * Windows leaves the log OPEN whenever it does not dismount a volume, and a
     * Fast Startup shutdown -- the default -- does not dismount a removable one.
     * The result reads as dirty while the transaction table is empty and there
     * is nothing whatever to replay: measured 2026-09-17, scenario B2, where the
     * file written before the shutdown was present and intact. Showing that user
     * a warning about possible file system damage is simply wrong.
     *
     * The dangerous case is a journal carrying real pending work, where we apply
     * a record format reverse-engineered rather than documented.
     *
     * journalPendingOps tells them apart: redo + undo + transactions to roll
     * back. -1 means an older status file that predates the field, and is
     * treated as the dangerous case.
     */
    private func confirmReplay(_ partition: Partition) {
        let pending = partition.journalPendingOps

        if pending == 0 {
            guard Confirm.standard(
                title: "Finish the shutdown Windows didn't complete on \(partition.displayName)?",
                message: "Windows left this volume's journal open instead of closing it, which "
                       + "happens on a normal shutdown when Fast Startup is on. Nothing is wrong "
                       + "with the disk and there are no unfinished writes to apply.\n\n"
                       + "This closes the journal so the volume can be written to. It changes two "
                       + "pages of bookkeeping and touches none of your files.",
                proceed: "Close the Journal and Mount Read/Write") else { return }
        } else {
            let count = pending > 0 ? "\(pending) unfinished operation\(pending == 1 ? "" : "s")"
                                    : "unfinished writes"
            guard Confirm.destructive(
                title: "Replay the journal on \(partition.displayName)?",
                message: "Windows left \(count) in this volume's journal. Applying them uses a "
                       + "journal format this driver has worked out rather than one documented by "
                       + "Microsoft. If that reading is wrong it can damage the file system, and "
                       + "the damage may not be obvious straight away.\n\n"
                       + "Back up anything you cannot lose first. The safe alternative is to plug "
                       + "the disk into Windows and eject it properly.",
                proceed: "Replay and Mount Read/Write") else { return }
        }
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

    /// The dirty mark asks for chkdsk, so this is the one confirmation that has
    /// to say both what is at risk and what the safe path is.
    private func confirmClearDirty(_ partition: Partition) {
        guard Confirm.destructive(
            title: "Mount \(partition.displayName) read/write without a check?",
            message: "This disk is marked as needing a check: Windows flagged it, or a session "
                   + "on this Mac did not finish. Writing to it before chkdsk runs can make any "
                   + "damage that is already there worse.\n\n"
                   + "This clears the mark. The Mac will then mount the disk read/write, and "
                   + "Windows will no longer check it.\n\n"
                   + "The safe way is to run chkdsk /f on it in Windows first.",
            proceed: "Clear the Mark and Mount Read/Write") else { return }
        Task { await inventory.mountReadWriteAnyway(partition); monitor.refresh() }
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

    /// The star line is for someone whose disks are working: not while the
    /// extension is off, and not while a volume or a note shows a problem.
    private var showStarLine: Bool {
        guard inventory.starLineDue, status.state == .enabled else { return false }
        let troubled = inventory.partitions.contains {
            $0.isMounted && ($0.mountedReadOnly || $0.heldByAnotherDriver)
        }
        return !troubled && inventory.note?.kind != .problem && inventory.noteForDisk?.kind != .problem
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

    /// On macOS 26 the System Settings switch cannot turn a third-party FSKit
    /// module on (26.3 and 26.6.2), so we do it here. On 27 we try the same,
    /// which needs Full Disk Access there, and fall back to that switch, which
    /// works on 27. See ModuleEnabler.
    @ViewBuilder private var enableFooter: some View {
        switch enabler.outcome {
        case .working:
            HStack(spacing: 6) {
                ProgressView().controlSize(.small)
                Text("Enabling…").font(.caption).foregroundStyle(.secondary)
            }
        case .needsSettings(let why):
            VStack(alignment: .leading, spacing: 6) {
                Text(why)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                // Already waiting for the user; a second `enable()` would only
                // be refused, so this reopens the pane instead.
                HStack {
                    Button("Open System Settings") { ExtensionStatus.openSettings() }
                    Button("Re-check") { status.refresh() }
                }
                .controlSize(.small)
            }
        case .blocked(let why), .failed(let why):
            VStack(alignment: .leading, spacing: 6) {
                Text(why)
                    .font(.caption).foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
                enableButtons(title: "Try Again")
            }
        // This footer shows only while the extension is off, so a success
        // here is one the status has not caught up with yet, or one undone
        // since in System Settings. Either way the button is what is needed.
        case .idle, .succeeded:
            VStack(alignment: .leading, spacing: 6) {
                Text(ModuleEnabler.settingsSwitchWorks
                     ? "Turn it on here, or in System Settings under Login Items & Extensions → By Category → File System Extensions, then press Re-check."
                     : "Turn it on here — the switch in System Settings cannot enable third-party file system extensions on this version of macOS.")
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
    let note: DiskInventory.DiskNote?
    let busy: Bool
    /// Nothing is mounted on this disk, so there is nothing left to eject. The
    /// device usually stays enumerated after an eject -- that is the enclosure,
    /// not an unfinished job -- so the button has to go by what is mounted.
    let hasMountedVolumes: Bool
    let eject: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Image(systemName: "externaldrive.connected.to.line.below")
                    .foregroundStyle(.secondary)
                Text(wholeDisk).font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Eject Disk", action: eject)
                    .controlSize(.small)
                    .disabled(busy || !hasMountedVolumes)
            }
            if let note {
                Label(note.text, systemImage: note.kind == .good
                        ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                    .font(.caption)
                    .foregroundStyle(note.kind == .good ? Color.green : Color.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.top, 2)
    }
}

struct PartitionRow: View {
    let partition: Partition

    /*
     * "Replay" is the wrong word for the common case. Windows leaves the log
     * open on any shutdown that does not dismount the volume -- which is what
     * Fast Startup does, by default, to a removable disk -- and then there is
     * nothing to replay at all, only a journal to close. Calling that "Replay
     * Journal" invites the user to expect a repair and to fear a risk, and
     * neither is true. See confirmReplay().
     */
    private var replayButtonTitle: String {
        partition.journalPendingOps == 0 ? "Close Journal…" : "Replay Journal…"
    }
    /// Why it is read-only, from the extension's own status file, when it knows.
    let reason: String
    let mount: () -> Void
    let eject: () -> Void
    let discardHibernation: () -> Void
    let replayJournal: () -> Void
    let mountReadWriteAnyway: () -> Void
    /// The result of the last action on *this* volume, shown in its own row.
    let note: DiskInventory.Note?
    let busy: Bool
    let handOver: () -> Void

    var body: some View {
        HStack(alignment: .top) {
            Image(systemName: icon)
                .foregroundStyle(iconColour)
            VStack(alignment: .leading, spacing: 2) {
                Text(partition.displayName).font(.body)
                HStack(spacing: 5) {
                    Text(subtitle).font(.caption).foregroundStyle(.secondary)
                    accessBadge
                }
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
                    Text(note.text)
                        .font(.caption)
                        .foregroundStyle(note.kind == .good ? Color.green : Color.orange)
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
                Button(replayButtonTitle, action: replayJournal).controlSize(.small)
                Button("Eject", action: eject).controlSize(.small)
            }
        } else if partition.hibernated && partition.mountedReadOnly {
            VStack(alignment: .trailing, spacing: 4) {
                Button("Discard Session…", action: discardHibernation).controlSize(.small)
                Button("Eject", action: eject).controlSize(.small)
            }
        } else if partition.dirtyBlocked && partition.servedByOurDriver {
            VStack(alignment: .trailing, spacing: 4) {
                Button("Mount R/W Anyway", action: mountReadWriteAnyway).controlSize(.small)
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

    /// Device and size only. Whether a volume is writable is the thing a user
    /// is actually looking for, so it is a badge rather than the third item in
    /// a grey list where it reads like more metadata.
    private var subtitle: String {
        "\(partition.bsdName) · \(partition.sizeDescription)"
    }

    private var accessBadge: some View {
        Text(accessText)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 5)
            .padding(.vertical, 1)
            .background(accessColour.opacity(0.15), in: Capsule())
            .foregroundStyle(accessColour)
    }

    private var accessText: String {
        guard partition.isMounted else { return "not mounted" }
        return partition.mountedReadOnly ? "read-only" : "read/write"
    }

    private var accessColour: Color {
        guard partition.isMounted else { return .secondary }
        return partition.mountedReadOnly ? .orange : .green
    }

    private var detail: String {
        if !partition.isMounted { return "" }
        if partition.heldByAnotherDriver { return "Mounted by the system, not by this driver." }
        return partition.mountedReadOnly ? reason : ""
    }
}

/// The one-time request for a GitHub star; when it appears is StarLine's
/// business (DiskInventory.swift). Following the link or pressing the cross
/// retires it for good.
struct StarLineRow: View {
    let retire: () -> Void

    private var message: AttributedString {
        var link = AttributedString("A star on GitHub")
        link.link = StarLine.repository
        // The site says "helps other Mac users find it"; that wraps here.
        return AttributedString("Working? ") + link + AttributedString(" helps others find it.")
    }

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Image(systemName: "star")
                .accessibilityHidden(true)
            Text(message)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 4)
            Button(action: retire) {
                Image(systemName: "xmark")
                    .padding(2)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("Don't show this again")
            .accessibilityLabel("Don't show this again")
        }
        .font(.caption)
        .foregroundStyle(.secondary)
        // Open the page in the browser as any link would, and retire the line
        // on the way: a click is as final as the cross.
        .environment(\.openURL, OpenURLAction { url in
            NSWorkspace.shared.open(url)
            retire()
            return .handled
        })
    }
}
