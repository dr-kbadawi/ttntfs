// SPDX-License-Identifier: GPL-2.0
//
// Keys shared with the extension via the app group. Keep in sync with
// NTFSExtension/Options.swift (SharedDefaults).

import Foundation

enum SharedSettings {
    static let suite = "group.ch.techtag.ntfs"
    static let store = UserDefaults(suiteName: suite) ?? .standard
    static let readOnly = "readOnly"
    static let showHidden = "showHidden"
    static let showSystem = "showSystem"
    static let allowWindowsIllegalNames = "allowWindowsIllegalNames"
    static let discard = "discard"
    static let caseSensitive = "caseSensitive"
}
