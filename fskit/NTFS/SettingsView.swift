// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH

import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var loginItem: LoginItem
    @EnvironmentObject private var uninstaller: Uninstaller
    @State private var confirmRemove = false
    @AppStorage(SharedSettings.readOnly, store: SharedSettings.store) private var readOnly = false
    @AppStorage(SharedSettings.showHidden, store: SharedSettings.store) private var showHidden = true
    @AppStorage(SharedSettings.showSystem, store: SharedSettings.store) private var showSystem = false
    @AppStorage(SharedSettings.allowWindowsIllegalNames, store: SharedSettings.store) private var allowIllegal = false
    @AppStorage(SharedSettings.discard, store: SharedSettings.store) private var discard = false
    @AppStorage(SharedSettings.caseSensitive, store: SharedSettings.store) private var caseSensitive = false
    @AppStorage(SharedSettings.hideDotFiles, store: SharedSettings.store) private var hideDotFiles = false
    @AppStorage(SharedSettings.wslSymlinks, store: SharedSettings.store) private var wslSymlinks = false

    var body: some View {
        Form {
            Section("General") {
                Toggle("Open at login", isOn: Binding(get: { loginItem.isEnabled },
                                                      set: { loginItem.setEnabled($0) }))
                if loginItem.requiresApproval {
                    Text("Turned off in System Settings › General › Login Items & Extensions.")
                        .font(.caption).foregroundStyle(.secondary)
                } else if let error = loginItem.lastError {
                    Text(error).font(.caption).foregroundStyle(.red)
                } else {
                    Text("Only the menu-bar status needs the app; volumes mount without it.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Section("Mounting") {
                Toggle("Mount volumes read-only", isOn: $readOnly)
                Toggle("Show files Windows marks as hidden", isOn: $showHidden)
                Toggle("Show NTFS system files ($MFT, $Bitmap, …)", isOn: $showSystem)
                Toggle("Case-sensitive names", isOn: $caseSensitive)
            }
            Section("Compatibility") {
                Toggle("Allow file names Windows cannot open (: ? * < > | \")", isOn: $allowIllegal)
                Text("Off: such names are rejected so everything written here opens on Windows.")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Hide macOS dot-files from Windows (.DS_Store, ._*, …)", isOn: $hideDotFiles)
                Text("New files whose names start with a dot get the Windows hidden attribute, so "
                     + "Explorer does not show them. Files that already exist are left alone.")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Write symlinks in WSL format", isOn: $wslSymlinks)
                Text("Off: symlinks use Windows' own format, which Explorer, cmd, WSL and Linux all "
                     + "follow. On: the WSL-only format, which Windows itself cannot follow; choose "
                     + "this only for a disk used mainly from WSL.")
                    .font(.caption).foregroundStyle(.secondary)
                /*
                 * Wired all the way to the allocator (NVolDiscard in
                 * lcnalloc.c), which does issue discards -- but the FSKit
                 * block device on macOS 26 has no TRIM/UNMAP primitive
                 * (checked against the 26.2 SDK: the only "trim" in FSKit is
                 * file-level preallocation trimming on close), so the bridge
                 * reports discard_granularity 0 and the core never asks.
                 * Leaving the switch enabled would be a mock. Disabled, with
                 * the reason on it, until Apple adds the primitive; the
                 * setting is still stored so it takes effect the day it can.
                 */
                Toggle("TRIM freed space (SSDs)", isOn: $discard)
                    .disabled(true)
                Text("Not available: macOS's FSKit gives file systems no way to issue TRIM. "
                     + "The setting is kept and will take effect when it does.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section {
                Text("Changes apply to volumes mounted from now on.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            /*
             * GPL-2.0 §1 asks each copy to carry an appropriate copyright
             * notice and §2(c) asks an interactive program to show it along
             * with the no-warranty notice. For a menu-bar app this panel is
             * where that lives. The version and commit come from the bundle,
             * stamped at build time by scripts/version.sh, so a build from an
             * uncommitted tree shows "-dirty" and a bug report can name the
             * exact source it came from.
             */
            Section("About") {
                LabeledContent("TT NTFS Native", value: BuildInfo.versionLine)
                LabeledContent("Build", value: BuildInfo.commit)
                    .font(.caption).foregroundStyle(.secondary)
                Text(BuildInfo.copyright)
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Text("Free software under the GNU General Public License, version 2. " +
                     "It comes with no warranty. The complete source code is available " +
                     "under the same licence.")
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Text("Contains code from the Linux NTFS driver, © Anton Altaparmakov, " +
                     "Tuxera Inc., Richard Russon, Jean-Pierre Andre, LG Electronics Co., Ltd. " +
                     "and others, used under the GPL.")
                    .font(.caption2).foregroundStyle(.tertiary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            // Dragging the app to the Trash would leave the module in FSKit's
            // enabled list, the bundle registered with LaunchServices, and the
            // login item orphaned -- only the app can undo those.
            Section("Remove") {
                switch uninstaller.state {
                case .working(let step):
                    HStack(spacing: 6) {
                        ProgressView().controlSize(.small)
                        Text(step).font(.caption).foregroundStyle(.secondary)
                    }
                case .failed(let why):
                    Text(why).font(.caption).foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                    Button("Try Again") { confirmRemove = true }
                case .idle:
                    Button("Remove TT NTFS Native…") { confirmRemove = true }
                    Text("Turns the extension off, removes its settings and login item, "
                         + "and moves the app to the Trash. Eject your NTFS volumes first.")
                        .font(.caption).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
        .confirmationDialog("Remove TT NTFS Native?", isPresented: $confirmRemove) {
            Button("Remove", role: .destructive) { Task { await uninstaller.run() } }
            Button("Cancel", role: .cancel) { }
        } message: {
            Text("NTFS volumes will stop mounting with this driver. macOS will mount them read-only again.")
        }
        .formStyle(.grouped)
        .frame(width: 440)
        .padding()
    }
}


/// Version, build and copyright as stamped into the bundle at build time.
enum BuildInfo {
    private static func str(_ key: String) -> String {
        (Bundle.main.object(forInfoDictionaryKey: key) as? String) ?? "?"
    }
    /// e.g. "0.4.0 (155)"
    static var versionLine: String {
        "\(str("CFBundleShortVersionString")) (\(str("CFBundleVersion")))"
    }
    /// git describe at build time, e.g. "v0.3-ui-fixes-103-gbcc13ab13-dirty"
    static var commit: String { str("TTGitCommit") }
    static var copyright: String { str("NSHumanReadableCopyright") }
}
