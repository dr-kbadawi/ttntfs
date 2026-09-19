// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
//
// Confirmation for a destructive action, from a menu-bar panel.
//
// SwiftUI's .confirmationDialog does not work here: presenting it closes the
// MenuBarExtra panel that owns it, the panel's dismissal does not reset the
// binding, and reopening the menu shows the dialog again. Pressing Cancel
// therefore made the whole menu vanish and come back still asking.
//
// An NSAlert has no such tie to the panel. It also gives the long explanations
// these actions need room to say, and lets Cancel be the default button, which
// for something that can damage a file system is the right default.

import AppKit

@MainActor
enum Confirm {
    /// Returns true only if the user chose to go ahead. Cancel is the default,
    /// so return and escape both decline.
    static func destructive(title: String, message: String, proceed: String) -> Bool {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.alertStyle = .critical
        alert.messageText = title
        alert.informativeText = message
        alert.addButton(withTitle: "Cancel")          // first button: the default
        alert.addButton(withTitle: proceed)
        if alert.buttons.count > 1 {
            alert.buttons[1].hasDestructiveAction = true
        }
        return alert.runModal() == .alertSecondButtonReturn
    }

    /// For an action that is safe but still worth confirming. The proceed button
    /// is the default here, because declining is not the cautious choice when
    /// nothing is at risk -- it just leaves the volume read-only for no reason.
    static func standard(title: String, message: String, proceed: String) -> Bool {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.messageText = title
        alert.informativeText = message
        alert.addButton(withTitle: proceed)            // first button: the default
        alert.addButton(withTitle: "Cancel")
        return alert.runModal() == .alertFirstButtonReturn
    }
}
