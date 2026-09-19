// SPDX-License-Identifier: GPL-2.0
//
// NTFSMountOptions: the boolean -> ntfs_mount_flags mapping and the -o option list.
// Both are pure, and both sit between the user and a filesystem that will be
// written to, so a flag landing on the wrong bit is not a cosmetic mistake:
// NTFS_MOUNT_DISCARD_HIBERNATION destroys a suspended Windows session.

import XCTest

final class MountOptionsTests: XCTestCase {

    // MARK: cFlags

    /// Every toggle with the bit it is supposed to set. Written out rather than
    /// derived so that the table itself is the statement of intent.
    private static let mapping: [(String, (inout NTFSMountOptions) -> Void, UInt32)] = [
        ("readOnly",                 { $0.readOnly = true },                 NTFS_MOUNT_RDONLY.rawValue),
        ("hideHidden",               { $0.hideHidden = true },               NTFS_MOUNT_HIDE_HIDDEN.rawValue),
        ("showSystem",               { $0.showSystem = true },               NTFS_MOUNT_SHOW_SYSTEM.rawValue),
        ("allowWindowsIllegalNames", { $0.allowWindowsIllegalNames = true }, NTFS_MOUNT_ALLOW_WINDOWS_ILLEGAL.rawValue),
        ("discard",                  { $0.discard = true },                  NTFS_MOUNT_DISCARD.rawValue),
        ("caseSensitive",            { $0.caseSensitive = true },            NTFS_MOUNT_CASE_SENSITIVE.rawValue),
        ("discardHibernation",       { $0.discardHibernation = true },       NTFS_MOUNT_DISCARD_HIBERNATION.rawValue),
        ("replayJournal",            { $0.replayJournal = true },            NTFS_MOUNT_REPLAY_JOURNAL.rawValue),
    ]

    /// The read-only fallback is unconditional: PORTING.md §6 says a dirty or
    /// hibernated volume mounts read-only with an explanation, never fails.
    func testDefaultsAskOnlyForTheReadOnlyFallback() {
        XCTAssertEqual(NTFSMountOptions().cFlags, NTFS_MOUNT_RDONLY_FALLBACK.rawValue)
    }

    func testEachToggleSetsItsOwnBitAndNothingElse() {
        for (name, set, bit) in Self.mapping {
            var o = NTFSMountOptions()
            set(&o)
            XCTAssertEqual(o.cFlags, NTFS_MOUNT_RDONLY_FALLBACK.rawValue | bit,
                           "\(name) does not map to the expected flag bit")
        }
    }

    /// Two toggles sharing a bit would make one silently imply the other.
    func testTheFlagBitsAreDistinctAndSingleBit() {
        var seen: Set<UInt32> = [NTFS_MOUNT_RDONLY_FALLBACK.rawValue]
        for (name, _, bit) in Self.mapping {
            XCTAssertEqual(bit.nonzeroBitCount, 1, "\(name) is not a single bit")
            XCTAssertTrue(seen.insert(bit).inserted, "\(name) shares a bit with another option")
        }
    }

    func testCombinationsOrTogether() {
        var o = NTFSMountOptions()
        o.readOnly = true
        o.showSystem = true
        o.caseSensitive = true
        XCTAssertEqual(o.cFlags,
                       NTFS_MOUNT_RDONLY_FALLBACK.rawValue
                       | NTFS_MOUNT_RDONLY.rawValue
                       | NTFS_MOUNT_SHOW_SYSTEM.rawValue
                       | NTFS_MOUNT_CASE_SENSITIVE.rawValue)
    }

    func testEveryToggleAtOnce() {
        var o = NTFSMountOptions()
        var expected = NTFS_MOUNT_RDONLY_FALLBACK.rawValue
        for (_, set, bit) in Self.mapping {
            set(&o)
            expected |= bit
        }
        XCTAssertEqual(o.cFlags, expected)
    }

    /// kernelReadOnly and force describe what the caller asked for, not what the
    /// core is told: the kernel has already set MNT_RDONLY, and -f is ours.
    /// Neither may leak into the flags word.
    func testKernelReadOnlyAndForceDoNotReachTheCore() {
        var o = NTFSMountOptions()
        o.kernelReadOnly = true
        o.force = true
        XCTAssertEqual(o.cFlags, NTFS_MOUNT_RDONLY_FALLBACK.rawValue)
    }

    /// The two destructive options are never stored preferences, so nothing but
    /// an explicit per-mount request can set them. Guards against a default that
    /// would discard a suspended Windows session on every mount.
    func testDestructiveOptionsAreOffByDefaultAndNotReachableFromAnOptionList() {
        XCTAssertFalse(NTFSMountOptions().discardHibernation)
        XCTAssertFalse(NTFSMountOptions().replayJournal)
        var o = NTFSMountOptions()
        o.applyList("discardhibernation,replayjournal,discard_hibernation,replay_journal")
        XCTAssertFalse(o.discardHibernation)
        XCTAssertFalse(o.replayJournal)
    }

    // MARK: -o option lists

    private func options(_ list: String) -> NTFSMountOptions {
        var o = NTFSMountOptions()
        o.applyList(list)
        return o
    }

    func testReadOnlyAndReadWrite() {
        XCTAssertTrue(options("ro").readOnly)
        XCTAssertTrue(options("rdonly").readOnly)
        XCTAssertFalse(options("rw").readOnly)
        XCTAssertFalse(options("").readOnly)
    }

    func testDiscard() {
        XCTAssertTrue(options("discard").discard)
        XCTAssertFalse(options("nodiscard").discard)
    }

    func testCaseSensitive() {
        XCTAssertTrue(options("casesensitive").caseSensitive)
        XCTAssertTrue(options("case_sensitive").caseSensitive)
    }

    func testHideHidden() {
        XCTAssertTrue(options("hidehidden").hideHidden)
        XCTAssertTrue(options("nohidden").hideHidden)
        XCTAssertFalse(options("showhidden").hideHidden)
    }

    func testShowSystem() {
        XCTAssertTrue(options("showsystem").showSystem)
        XCTAssertTrue(options("show_sys_files").showSystem)
        XCTAssertFalse(options("").showSystem)
    }

    func testAllowIllegalNames() {
        XCTAssertTrue(options("allowillegal").allowWindowsIllegalNames)
        XCTAssertTrue(options("windows_names_off").allowWindowsIllegalNames)
    }

    func testForce() {
        XCTAssertTrue(options("force").force)
    }

    /// mount(8) passes whatever the user typed. An option we do not know is not
    /// a reason to refuse the mount, and must not disturb the ones we do know.
    func testUnknownOptionsAreIgnoredWithoutDisturbingTheRest() {
        let o = options("ro,nodev,noatime,nosuid,showsystem,-1,=,ro=yes")
        XCTAssertTrue(o.readOnly)
        XCTAssertTrue(o.showSystem)
        XCTAssertFalse(o.discard)
        XCTAssertFalse(o.caseSensitive)
        XCTAssertFalse(o.hideHidden)
        XCTAssertFalse(o.allowWindowsIllegalNames)
    }

    /// mount's own convention: the last occurrence wins, so `-o ro,rw` mounts
    /// read/write. This is how the app overrides a stored default.
    func testLaterOptionsWin() {
        XCTAssertFalse(options("ro,rw").readOnly)
        XCTAssertTrue(options("rw,ro").readOnly)
        XCTAssertFalse(options("discard,nodiscard").discard)
        XCTAssertTrue(options("nodiscard,discard").discard)
        XCTAssertFalse(options("hidehidden,showhidden").hideHidden)
        XCTAssertTrue(options("showhidden,hidehidden").hideHidden)
    }

    func testRepeatedOptionIsIdempotent() {
        XCTAssertTrue(options("ro,ro,ro").readOnly)
    }

    func testEmptyAndDegenerateListsChangeNothing() {
        XCTAssertEqual(options("").cFlags, NTFSMountOptions().cFlags)
        XCTAssertEqual(options(",").cFlags, NTFSMountOptions().cFlags)
        XCTAssertEqual(options(",,,").cFlags, NTFSMountOptions().cFlags)
        XCTAssertEqual(options(",ro,,").cFlags,
                       NTFS_MOUNT_RDONLY_FALLBACK.rawValue | NTFS_MOUNT_RDONLY.rawValue)
    }

    func testOptionNamesAreCaseInsensitive() {
        XCTAssertTrue(options("RO").readOnly)
        XCTAssertTrue(options("ShowSystem").showSystem)
        XCTAssertTrue(options("CASE_SENSITIVE").caseSensitive)
    }

    /// applyList is applied repeatedly over one NTFSMountOptions (defaults first,
    /// then loadResource, then activate), so it must accumulate rather than
    /// reset. A second list that says nothing about read-only must not clear it.
    func testApplyingASecondListAccumulates() {
        var o = NTFSMountOptions()
        o.applyList("ro")
        o.applyList("showsystem")
        XCTAssertTrue(o.readOnly)
        XCTAssertTrue(o.showSystem)
    }

    func testAWholeListMapsToTheExpectedFlagsWord() {
        XCTAssertEqual(options("ro,showsystem,discard,casesensitive,nohidden,allowillegal").cFlags,
                       NTFS_MOUNT_RDONLY_FALLBACK.rawValue
                       | NTFS_MOUNT_RDONLY.rawValue
                       | NTFS_MOUNT_SHOW_SYSTEM.rawValue
                       | NTFS_MOUNT_DISCARD.rawValue
                       | NTFS_MOUNT_CASE_SENSITIVE.rawValue
                       | NTFS_MOUNT_HIDE_HIDDEN.rawValue
                       | NTFS_MOUNT_ALLOW_WINDOWS_ILLEGAL.rawValue)
    }
}
