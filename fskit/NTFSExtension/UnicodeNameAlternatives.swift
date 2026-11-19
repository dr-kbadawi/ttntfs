// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH

import Foundation

/// Return NFC then NFD, omitting byte-identical spellings. Swift String equality
/// treats canonically equivalent strings as equal, but NTFS distinguishes them.
func unicodeNameAlternatives(_ name: String) -> [String] {
    let nfc = name.precomposedStringWithCanonicalMapping
    let nfd = name.decomposedStringWithCanonicalMapping
    var alternatives: [String] = []
    if !nfc.utf8.elementsEqual(name.utf8) {
        alternatives.append(nfc)
    }
    if !nfd.utf8.elementsEqual(name.utf8) && !nfd.utf8.elementsEqual(nfc.utf8) {
        alternatives.append(nfd)
    }
    return alternatives
}
