// SPDX-License-Identifier: GPL-2.0
//
// Mount options: defaults shared with the host app through the app group
// (SharedSettings.swift in the app writes them), overridable per mount by
// the task options FSKit hands us:
//   loadResource:  `--rdonly` (kernel asked for a read-only mount), `-f`
//   activate:      `-o a,b,c` (FSActivateOptionSyntax shortOptions "o:")
// Every toggle maps onto one bit of ntfs_mount_options.flags.

import Foundation
import FSKit

enum SharedDefaults {
    static let suite = "group.ch.techtag.ntfs"
    static let readOnly = "readOnly"
    static let showHidden = "showHidden"
    static let showSystem = "showSystem"
    static let allowWindowsIllegalNames = "allowWindowsIllegalNames"
    static let discard = "discard"
    static let caseSensitive = "caseSensitive"
    /// One-shot requests, written by the app: BSD names whose saved Windows
    /// hibernation image the user has agreed to discard. Consumed at mount.
    static let pendingHibernationDiscard = "pendingHibernationDiscard"
}

struct MountOptions {
    var readOnly = false          // Settings toggle or -o ro
    var kernelReadOnly = false    // --rdonly from FSKit / mount -r: the kernel already has MNT_RDONLY
    var force = false             // -f
    var hideHidden = false
    var showSystem = false
    var allowWindowsIllegalNames = false
    var discard = false
    var caseSensitive = false
    /// Discard the saved Windows session so the volume can mount read-write.
    /// Never a stored preference: only ever set for one mount, by explicit
    /// user consent, because it destroys whatever Windows had suspended.
    var discardHibernation = false

    static func fromDefaults() -> MountOptions {
        var o = MountOptions()
        guard let d = UserDefaults(suiteName: SharedDefaults.suite) else { return o }
        o.readOnly = d.bool(forKey: SharedDefaults.readOnly)
        // SettingsView's default for showHidden is true; the key is absent until toggled.
        o.hideHidden = d.object(forKey: SharedDefaults.showHidden) != nil && !d.bool(forKey: SharedDefaults.showHidden)
        o.showSystem = d.bool(forKey: SharedDefaults.showSystem)
        o.allowWindowsIllegalNames = d.bool(forKey: SharedDefaults.allowWindowsIllegalNames)
        o.discard = d.bool(forKey: SharedDefaults.discard)
        o.caseSensitive = d.bool(forKey: SharedDefaults.caseSensitive)
        return o
    }

    /// Takes the one-shot hibernation-discard request for this device, if the
    /// user made one. Removing it here means a failed or repeated mount cannot
    /// silently discard a session the user only agreed to once.
    static func takeHibernationDiscardRequest(for bsdName: String) -> Bool {
        guard let d = UserDefaults(suiteName: SharedDefaults.suite) else { return false }
        let pending = d.stringArray(forKey: SharedDefaults.pendingHibernationDiscard) ?? []
        guard pending.contains(bsdName) else { return false }
        d.set(pending.filter { $0 != bsdName }, forKey: SharedDefaults.pendingHibernationDiscard)
        return true
    }

    /// Applies FSTaskOptions from loadResource / activate / mount. Idempotent.
    mutating func apply(taskOptions: FSTaskOptions) {
        let args = taskOptions.taskOptions
        var i = 0
        while i < args.count {
            let a = args[i]
            switch a {
            case "--rdonly", "-r", "rdonly":
                kernelReadOnly = true
                readOnly = true
            case "-f", "--force":
                force = true
            case "-o":
                if i + 1 < args.count { applyList(args[i + 1]); i += 1 }
            default:
                if a.hasPrefix("-o") && a.count > 2 {
                    applyList(String(a.dropFirst(2)))
                } else if !a.hasPrefix("-") {
                    applyList(a)     // bare "ro,showsystem" (mount passes -o's argument alone on some paths)
                } else {
                    log.info("ignoring task option \(a, privacy: .public)")
                }
            }
            i += 1
        }
    }

    private mutating func applyList(_ list: String) {
        for opt in list.split(separator: ",") {
            switch opt.lowercased() {
            case "ro", "rdonly": readOnly = true
            case "rw": readOnly = false
            case "hidehidden", "nohidden": hideHidden = true
            case "showhidden": hideHidden = false
            case "showsystem", "show_sys_files": showSystem = true
            case "allowillegal", "windows_names_off": allowWindowsIllegalNames = true
            case "discard": discard = true
            case "nodiscard": discard = false
            case "casesensitive", "case_sensitive": caseSensitive = true
            case "force": force = true
            case "": break
            default: log.info("ignoring mount option \(String(opt), privacy: .public)")
            }
        }
    }

    var cFlags: UInt32 {
        // Always ask for the read-only fallback: PORTING.md §6 says a dirty /
        // hibernated volume mounts read-only with an explanation, never fails.
        var f: UInt32 = NTFS_MOUNT_RDONLY_FALLBACK.rawValue
        if readOnly { f |= NTFS_MOUNT_RDONLY.rawValue }
        if hideHidden { f |= NTFS_MOUNT_HIDE_HIDDEN.rawValue }
        if showSystem { f |= NTFS_MOUNT_SHOW_SYSTEM.rawValue }
        if allowWindowsIllegalNames { f |= NTFS_MOUNT_ALLOW_WINDOWS_ILLEGAL.rawValue }
        if discard { f |= NTFS_MOUNT_DISCARD.rawValue }
        if caseSensitive { f |= NTFS_MOUNT_CASE_SENSITIVE.rawValue }
        if discardHibernation { f |= NTFS_MOUNT_DISCARD_HIBERNATION.rawValue }
        return f
    }

    var cOptions: ntfs_mount_options {
        var o = ntfs_mount_options()
        o.flags = cFlags
        // noowners: every file belongs to the user fskitd runs the module as
        // (the console user); the volume reports restrictsOwnershipChanges.
        o.uid = getuid()
        o.gid = getgid()
        o.fmask = 0o022
        o.dmask = 0o022
        return o
    }
}
