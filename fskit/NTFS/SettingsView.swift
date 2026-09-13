// SPDX-License-Identifier: GPL-2.0

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
                Toggle("TRIM freed space (SSDs)", isOn: $discard)
            }
            Section {
                Text("Changes apply to volumes mounted from now on.")
                    .font(.caption).foregroundStyle(.secondary)
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
