// SPDX-License-Identifier: GPL-2.0
//
// Mount options: defaults shared with the host app through the app group,
// overridable per mount by `-o` task options.

import Foundation
import FSKit

enum SharedDefaults {
    static let suite = "group.org.ntfsmac.NTFS"
    static let readOnly = "readOnly"
    static let showHidden = "showHidden"
    static let showSystem = "showSystem"
    static let allowWindowsIllegalNames = "allowWindowsIllegalNames"
    static let discard = "discard"
    static let caseSensitive = "caseSensitive"
}

struct MountOptions {
    var readOnly = false
    var hideHidden = false
    var showSystem = false
    var allowWindowsIllegalNames = false
    var discard = false
    var caseSensitive = false

    static func fromDefaults() -> MountOptions {
        var o = MountOptions()
        guard let d = UserDefaults(suiteName: SharedDefaults.suite) else { return o }
        o.readOnly = d.bool(forKey: SharedDefaults.readOnly)
        o.hideHidden = d.object(forKey: SharedDefaults.showHidden) != nil && !d.bool(forKey: SharedDefaults.showHidden)
        o.showSystem = d.bool(forKey: SharedDefaults.showSystem)
        o.allowWindowsIllegalNames = d.bool(forKey: SharedDefaults.allowWindowsIllegalNames)
        o.discard = d.bool(forKey: SharedDefaults.discard)
        o.caseSensitive = d.bool(forKey: SharedDefaults.caseSensitive)
        return o
    }

    /// Applies `mount -o a,b,c` style options from FSTaskOptions.
    mutating func apply(taskOptions: FSTaskOptions) {
        let args = taskOptions.taskOptions
        var i = 0
        while i < args.count {
            if args[i] == "-o", i + 1 < args.count {
                for opt in args[i + 1].split(separator: ",") {
                    switch opt {
                    case "ro", "rdonly": readOnly = true
                    case "rw": readOnly = false
                    case "hidehidden": hideHidden = true
                    case "showsystem": showSystem = true
                    case "allowillegal": allowWindowsIllegalNames = true
                    case "discard": discard = true
                    case "casesensitive": caseSensitive = true
                    default: log.info("ignoring mount option \(String(opt), privacy: .public)")
                    }
                }
                i += 2
            } else {
                i += 1
            }
        }
    }

    var cFlags: UInt32 {
        var f: UInt32 = NTFS_MOUNT_RDONLY_FALLBACK.rawValue
        if readOnly { f |= NTFS_MOUNT_RDONLY.rawValue }
        if hideHidden { f |= NTFS_MOUNT_HIDE_HIDDEN.rawValue }
        if showSystem { f |= NTFS_MOUNT_SHOW_SYSTEM.rawValue }
        if allowWindowsIllegalNames { f |= NTFS_MOUNT_ALLOW_WINDOWS_ILLEGAL.rawValue }
        if discard { f |= NTFS_MOUNT_DISCARD.rawValue }
        if caseSensitive { f |= NTFS_MOUNT_CASE_SENSITIVE.rawValue }
        return f
    }

    var cOptions: ntfs_mount_options {
        var o = ntfs_mount_options()
        o.flags = cFlags
        o.uid = getuid()
        o.gid = getgid()
        o.fmask = 0o022
        o.dmask = 0o022
        return o
    }
}
