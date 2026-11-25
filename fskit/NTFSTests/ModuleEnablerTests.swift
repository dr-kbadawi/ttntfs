// SPDX-License-Identifier: GPL-2.0
//
// How the app turns its FSKit module on and off: the edit of fskit_agent's
// list, and when it hands over to the switch in System Settings instead.
//
// What these pin down, from the macOS 27 work (issue #2):
// - macOS 27 refuses the list to an app without Full Disk Access, with Cocoa
//   error 257. That has to read as a permission error on every version, or the
//   app reports "Could not update the FSKit module list" and stops there.
// - On 27 the System Settings switch works, so any failed or ignored edit hands
//   over to it. On 26 it cannot turn a third-party module on, and only a
//   refused list sends the user there.
//   Both from the fix "let the app turn the extension on itself on macOS 27".
// - A list in a shape the app does not know is refused with a plain message,
//   not overwritten: writing our one entry over it would drop every other
//   module. From the same fix, and "say plainly when the module list has an
//   unknown format".
// - A finished attempt's message is cleared once the extension is seen on, so
//   the menu does not keep it after the state changes in System Settings. From
//   "keep the menu's enable state current after a change in System Settings".

import XCTest

final class ModuleEnablerTests: XCTestCase {
    private let ours = "ch.techtag.ntfs.extension"
    private let others = ["com.apple.fskit.exfat", "com.example.fskit.other"]

    // MARK: the list

    func testNoListBecomesAListOfOurs() throws {
        XCTAssertEqual(try ModuleEnabler.listAdding(ours, to: nil), [ours])
    }

    func testAddingKeepsEveryOtherModuleAndAppendsOurs() throws {
        XCTAssertEqual(try ModuleEnabler.listAdding(ours, to: others), others + [ours])
        XCTAssertEqual(try ModuleEnabler.listAdding(ours, to: [String]()), [ours])
    }

    func testAddingWhenAlreadyListedWritesNothing() throws {
        XCTAssertNil(try ModuleEnabler.listAdding(ours, to: others + [ours]))
    }

    /// The list is FSKit's data. In any shape but a list of strings the app
    /// refuses it with a plain message and writes nothing, rather than replace
    /// it with a list of one.
    func testAListInAnUnknownShapeIsRefusedNotReplaced() {
        let shapes: [Any] = [
            ["modules": others],
            [1, 2] as [Any],
            ["com.apple.fskit.exfat", 2] as [Any],
            ours,
        ]
        for shape in shapes {
            XCTAssertThrowsError(try ModuleEnabler.listAdding(ours, to: shape), "\(shape)") { error in
                XCTAssertEqual(error.localizedDescription,
                               "it is not in the format this app knows, so it was left unchanged")
                XCTAssertFalse(ModuleEnabler.isPermissionDenied(error))
            }
        }
    }

    /// The file is parsed by PropertyListSerialization, which hands back
    /// Foundation collections; they must be read the same way.
    func testAParsedListIsReadLikeAnyOther() throws {
        let list = try PropertyListSerialization.data(fromPropertyList: others, format: .binary, options: 0)
        let parsedList = try PropertyListSerialization.propertyList(from: list, options: [], format: nil)
        XCTAssertEqual(try ModuleEnabler.listAdding(ours, to: parsedList), others + [ours])

        let dict = try PropertyListSerialization.data(fromPropertyList: ["modules": others], format: .binary, options: 0)
        let parsedDict = try PropertyListSerialization.propertyList(from: dict, options: [], format: nil)
        XCTAssertThrowsError(try ModuleEnabler.listAdding(ours, to: parsedDict))
        XCTAssertNil(ModuleEnabler.listRemoving(ours, from: parsedDict))
    }

    func testRemovingTakesOutOnlyOurs() {
        XCTAssertEqual(ModuleEnabler.listRemoving(ours, from: [others[0], ours, others[1]]), others)
        XCTAssertEqual(ModuleEnabler.listRemoving(ours, from: [ours, others[0], ours]), [others[0]])
        XCTAssertEqual(ModuleEnabler.listRemoving(ours, from: [ours]), [])
    }

    /// No list, a list without ours, or one in a shape the app does not know:
    /// nothing is written. The uninstaller must not fail over a list it has
    /// nothing to take out of.
    func testRemovingWritesNothingWhenThereIsNothingToRemove() {
        XCTAssertNil(ModuleEnabler.listRemoving(ours, from: nil))
        XCTAssertNil(ModuleEnabler.listRemoving(ours, from: others))
        XCTAssertNil(ModuleEnabler.listRemoving(ours, from: ["modules": [ours]]))
    }

    // MARK: permission errors

    /// What macOS 27 gives an app without Full Disk Access that reads the
    /// list: NSCocoaErrorDomain 257, "couldn't be opened because you don't have
    /// permission to view it". Reproduced with a file the test may not read.
    func testTheErrorForAnUnreadableFileIsAPermissionError() throws {
        try XCTSkipIf(geteuid() == 0, "root reads a file whatever its mode")
        let dir = FileManager.default.temporaryDirectory
            .appending(path: "ModuleEnablerTests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let file = dir.appending(path: "enabledModules.plist")
        try Data().write(to: file)
        try FileManager.default.setAttributes([.posixPermissions: 0], ofItemAtPath: file.path)

        XCTAssertThrowsError(try Data(contentsOf: file)) { error in
            XCTAssertEqual((error as NSError).domain, NSCocoaErrorDomain)
            XCTAssertEqual((error as NSError).code, NSFileReadNoPermissionError)
            XCTAssertTrue(ModuleEnabler.isPermissionDenied(error))
        }
    }

    func testRefusalsAreRecognisedInBothDomains() {
        let refusals: [Error] = [
            CocoaError(.fileReadNoPermission),
            CocoaError(.fileWriteNoPermission),
            NSError(domain: NSPOSIXErrorDomain, code: Int(EPERM)),
            NSError(domain: NSPOSIXErrorDomain, code: Int(EACCES)),
        ]
        for error in refusals {
            XCTAssertTrue(ModuleEnabler.isPermissionDenied(error), "\(error)")
        }
    }

    func testOtherFailuresAreNotRefusals() {
        let failures: [Error] = [
            CocoaError(.propertyListReadCorrupt),
            CocoaError(.fileReadCorruptFile),
            CocoaError(.fileNoSuchFile),
            CocoaError(.fileWriteOutOfSpace),
            NSError(domain: NSPOSIXErrorDomain, code: Int(ENOENT)),
            NSError(domain: NSPOSIXErrorDomain, code: Int(EROFS)),
        ]
        for error in failures {
            XCTAssertFalse(ModuleEnabler.isPermissionDenied(error), "\(error)")
        }
    }

    // MARK: when System Settings takes over

    func testTheSwitchWorksFromMacOS27() {
        XCTAssertFalse(ModuleEnabler.settingsSwitchWorks(onMacOS: 26))
        XCTAssertTrue(ModuleEnabler.settingsSwitchWorks(onMacOS: 27))
        XCTAssertTrue(ModuleEnabler.settingsSwitchWorks(onMacOS: 28))
    }

    /// On 26 the switch cannot turn a third-party module on, so only a refused
    /// list sends the user there; any other failure is reported.
    func testOn26OnlyARefusedListGoesToSystemSettings() {
        XCTAssertTrue(ModuleEnabler.editFailureGoesToSettings(CocoaError(.fileReadNoPermission), onMacOS: 26))
        XCTAssertFalse(ModuleEnabler.editFailureGoesToSettings(CocoaError(.propertyListReadCorrupt), onMacOS: 26))
        XCTAssertFalse(ModuleEnabler.editFailureGoesToSettings(CocoaError(.fileWriteOutOfSpace), onMacOS: 26))
    }

    /// On 27 the switch works, so any failure to edit the list hands over to
    /// it, a list in an unknown shape included.
    func testOn27AnyFailedEditGoesToSystemSettings() {
        let failures: [Error] = [
            CocoaError(.fileReadNoPermission),
            CocoaError(.propertyListReadCorrupt),
            CocoaError(.fileWriteOutOfSpace),
        ]
        for error in failures {
            XCTAssertTrue(ModuleEnabler.editFailureGoesToSettings(error, onMacOS: 27), "\(error)")
        }
    }

    func testOn27AnEditFSKitIgnoresHandsOverToTheSwitch() {
        XCTAssertEqual(ModuleEnabler.afterIgnoredEdit(onMacOS: 27, agentsRestarted: 1), .systemSettings)
        XCTAssertEqual(ModuleEnabler.afterIgnoredEdit(onMacOS: 27, agentsRestarted: 0), .systemSettings)
    }

    /// Before 27 an ignored edit is a failure, and the message says whether
    /// there was an fskit_agent to restart, since what to do next differs.
    func testOn26AnEditFSKitIgnoresIsAFailureThatSaysWhy() {
        guard case .failed(let noAgent) = ModuleEnabler.afterIgnoredEdit(onMacOS: 26, agentsRestarted: 0),
              case .failed(let ignored) = ModuleEnabler.afterIgnoredEdit(onMacOS: 26, agentsRestarted: 2) else {
            return XCTFail("an ignored edit on 26 must end in a failure")
        }
        XCTAssertTrue(noAgent.contains("fskit_agent was not running"), noAgent)
        XCTAssertTrue(ignored.contains("still reports the extension as disabled"), ignored)
    }

    // MARK: settle()

    /// Seeing the extension on clears a finished attempt's result, so the menu
    /// does not keep an old message once the state changes in System Settings.
    func testAFinishedAttemptIsClearedOnceTheExtensionIsOn() {
        XCTAssertEqual(ModuleEnabler.settled(.succeeded), .idle)
        XCTAssertEqual(ModuleEnabler.settled(.failed("x")), .idle)
        XCTAssertEqual(ModuleEnabler.settled(.blocked("x")), .idle)
    }

    /// An attempt still running, or waiting for the user in System Settings,
    /// is left alone: its own poll loop finishes it.
    func testAnAttemptInProgressIsLeftAlone() {
        XCTAssertEqual(ModuleEnabler.settled(.working), .working)
        XCTAssertEqual(ModuleEnabler.settled(.needsSettings("hint")), .needsSettings("hint"))
        XCTAssertEqual(ModuleEnabler.settled(.idle), .idle)
    }
}
