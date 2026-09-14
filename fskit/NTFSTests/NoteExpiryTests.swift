// SPDX-License-Identifier: GPL-2.0
//
// When a note the menu is showing stops being true.
//
// The bug these pin down: an eject failure ("disk6s1 could not be ejected,
// something is still using it") stayed on screen after the disk was unplugged,
// and then attached itself to the NEXT disk. macOS recycles BSD names -- in one
// session disk6 was three different physical disks -- so matching a note to a
// partition by name alone hands a message about one piece of hardware to
// another. Partition.attachmentID (the IOKit registry entry ID, new on every
// attach) is what tells them apart.

import XCTest

final class NoteExpiryTests: XCTestCase {

    private func partition(_ bsdName: String, attachment: UInt64) -> Partition {
        Partition(bsdName: bsdName, content: "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7",
                  size: 512 << 20, mediaWritable: true, attachmentID: attachment)
    }

    // MARK: per-volume notes

    func testNoteSurvivesWhileTheSameAttachmentIsStillThere() {
        let disks = [partition("disk6s1", attachment: 4242)]
        XCTAssertFalse(DiskInventory.noteHasExpired(bsdName: "disk6s1", attachmentID: 4242, in: disks))
    }

    func testNoteExpiresWhenTheDiskIsGone() {
        XCTAssertTrue(DiskInventory.noteHasExpired(bsdName: "disk6s1", attachmentID: 4242, in: []))
        XCTAssertTrue(DiskInventory.noteHasExpired(bsdName: "disk6s1", attachmentID: 4242,
                                                   in: [partition("disk7s1", attachment: 4242)]))
    }

    /// The bug itself: same BSD name, different hardware. Without the
    /// attachmentID comparison this returns false and the old message is shown
    /// against a disk it was never about.
    func testNoteExpiresWhenADifferentAttachmentWearsTheSameName() {
        let replacement = [partition("disk6s1", attachment: 9999)]
        XCTAssertTrue(DiskInventory.noteHasExpired(bsdName: "disk6s1", attachmentID: 4242, in: replacement))
    }

    /// The same disk unplugged and plugged back in is a new attachment, so a
    /// note made before the cable was pulled does not apply to it either.
    func testNoteExpiresAcrossAReattachOfTheSameDisk() {
        XCTAssertTrue(DiskInventory.noteHasExpired(bsdName: "disk6s1", attachmentID: 4242,
                                                   in: [partition("disk6s1", attachment: 4243)]))
    }

    /// An attachmentID of 0 means none was known when the note was made. There
    /// is then nothing to compare, so the note is kept as long as the name is
    /// present -- lenient, exactly as the code has always been.
    func testUnknownAttachmentIDOnTheNoteIsLenient() {
        XCTAssertFalse(DiskInventory.noteHasExpired(bsdName: "disk6s1", attachmentID: 0,
                                                    in: [partition("disk6s1", attachment: 4242)]))
        XCTAssertFalse(DiskInventory.noteHasExpired(bsdName: "disk6s1", attachmentID: 0,
                                                    in: [partition("disk6s1", attachment: 0)]))
    }

    /// Leniency stops at the disk vanishing: an unknown attachment is no reason
    /// to keep talking about a disk that is not there.
    func testUnknownAttachmentIDStillExpiresWhenTheDiskIsGone() {
        XCTAssertTrue(DiskInventory.noteHasExpired(bsdName: "disk6s1", attachmentID: 0, in: []))
    }

    /// The other direction: the note knows an attachment, the partition now
    /// reports none. They are not the same thing, so the note goes.
    func testKnownNoteAgainstAPartitionWithNoAttachmentIDExpires() {
        XCTAssertTrue(DiskInventory.noteHasExpired(bsdName: "disk6s1", attachmentID: 4242,
                                                   in: [partition("disk6s1", attachment: 0)]))
    }

    func testOtherPartitionsOnTheListDoNotConfuseTheMatch() {
        let disks = [partition("disk4s2", attachment: 11),
                     partition("disk6s1", attachment: 4242),
                     partition("disk6s2", attachment: 4243)]
        XCTAssertFalse(DiskInventory.noteHasExpired(bsdName: "disk6s1", attachmentID: 4242, in: disks))
        XCTAssertTrue(DiskInventory.noteHasExpired(bsdName: "disk6s1", attachmentID: 4243, in: disks))
    }

    // MARK: whole-disk notes

    /// The eject note is about disk6; the partitions listed are disk6s1 and
    /// disk6s2. Matching has to go through wholeDisk or the note never matches
    /// anything and disappears on the next poll, two seconds later.
    func testDiskNoteMatchesThroughThePartitionsOnThatDisk() {
        let disks = [partition("disk6s1", attachment: 4242), partition("disk6s2", attachment: 4243)]
        XCTAssertFalse(DiskInventory.diskNoteHasExpired(wholeDisk: "disk6", attachmentID: 4242, in: disks))
    }

    func testDiskNoteExpiresWhenTheDiskIsGone() {
        XCTAssertTrue(DiskInventory.diskNoteHasExpired(wholeDisk: "disk6", attachmentID: 4242, in: []))
        XCTAssertTrue(DiskInventory.diskNoteHasExpired(wholeDisk: "disk6", attachmentID: 4242,
                                                       in: [partition("disk7s1", attachment: 4242)]))
    }

    /// The reported bug, as it actually happened: an eject failure for disk6,
    /// disk6 unplugged, a different disk arrives and is also called disk6.
    func testDiskNoteExpiresWhenADifferentDiskTakesTheName() {
        let replacement = [partition("disk6s1", attachment: 9999)]
        XCTAssertTrue(DiskInventory.diskNoteHasExpired(wholeDisk: "disk6", attachmentID: 4242, in: replacement))
    }

    /// "Safe to unplug" is shown with an attachment taken from the disk's first
    /// partition, so a whole-disk note carries a per-partition ID. It must match
    /// that partition, not be defeated by the disk having several.
    func testDiskNoteMatchesWhicheverPartitionCarriesTheAttachment() {
        let disks = [partition("disk6s1", attachment: 4242), partition("disk6s2", attachment: 4243)]
        XCTAssertTrue(DiskInventory.diskNoteHasExpired(wholeDisk: "disk6", attachmentID: 4243, in: disks),
                      "matching uses the first partition of the disk, which is how the note was made")
    }

    func testDiskNoteWithUnknownAttachmentIDIsLenient() {
        XCTAssertFalse(DiskInventory.diskNoteHasExpired(wholeDisk: "disk6", attachmentID: 0,
                                                        in: [partition("disk6s1", attachment: 4242)]))
        XCTAssertTrue(DiskInventory.diskNoteHasExpired(wholeDisk: "disk6", attachmentID: 0, in: []))
    }

    /// A whole-disk volume (an attached image, or a disk with no partition
    /// table) is its own wholeDisk.
    func testWholeDiskVolumeMatchesItself() {
        XCTAssertFalse(DiskInventory.diskNoteHasExpired(wholeDisk: "disk8", attachmentID: 77,
                                                        in: [partition("disk8", attachment: 77)]))
        XCTAssertTrue(DiskInventory.diskNoteHasExpired(wholeDisk: "disk8", attachmentID: 77,
                                                       in: [partition("disk8", attachment: 78)]))
    }

    /// disk6 and disk60 are different disks; a prefix match would confuse them.
    func testDiskNamesAreNotPrefixMatched() {
        XCTAssertTrue(DiskInventory.diskNoteHasExpired(wholeDisk: "disk6", attachmentID: 4242,
                                                       in: [partition("disk60s1", attachment: 4242)]))
    }
}
