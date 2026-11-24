// SPDX-License-Identifier: GPL-2.0
//
// When the menu's one-time star line appears, and that it stays gone.
//
// The count is made from the inventory's two-second poll, which sees a state,
// not a mount event. These pin down how polls become occasions:
// - a volume that stays mounted counts once, however often it is polled,
//   however its status flickers and however often the app is relaunched;
// - ejecting and mounting again is new again, and a restart forgets what was
//   counted, because attachment IDs are only unique within one boot;
// - new mounts within the hour of the last occasion belong to it, so a dock
//   of disks or the partitions of one disk are one occasion, not one each;
// - once retired, from the menu or from Settings, the line never returns.

import XCTest

final class StarLineTests: XCTestCase {

    private func volume(_ bsdName: String, attachment: UInt64 = 4242, mounted: Bool = true,
                        ours: Bool = true, readOnly: Bool = false) -> Partition {
        var p = Partition(bsdName: bsdName, content: "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7",
                          size: 512 << 20, mediaWritable: true, attachmentID: attachment)
        if mounted { p.mountPoint = "/Volumes/" + bsdName }
        p.servedByOurDriver = mounted && ours
        p.mountedReadOnly = mounted && readOnly
        return p
    }

    /// Runs polls through newMounts, carrying what was counted as poll() does.
    private func fresh(_ polls: [[Partition]]) -> [Int] {
        var counted: Set<String> = []
        return polls.map { poll in
            let step = StarLine.newMounts(poll, counted: counted)
            counted = step.counted
            return step.fresh
        }
    }

    // MARK: what is new

    func testAReadWriteMountIsNewOnceWhileItStaysMounted() {
        let disk = [volume("disk6s2")]
        XCTAssertEqual(fresh([disk, disk, disk]), [1, 0, 0])
    }

    func testReadOnlyUnmountedAndOtherDriversAreNotNew() {
        XCTAssertEqual(fresh([[volume("disk6s2", readOnly: true)],
                              [volume("disk7s1", ours: false)],
                              [volume("disk8s1", mounted: false)]]), [0, 0, 0])
    }

    /// Journal closed or Windows session discarded: the volume was read-only
    /// and is now writable, which is the success the line is waiting for.
    func testAReadOnlyVolumeIsNewWhenItBecomesWritable() {
        XCTAssertEqual(fresh([[volume("disk6s2", readOnly: true)], [volume("disk6s2")]]), [0, 1])
    }

    /// Taken over from Apple's driver with "Use This Driver".
    func testAHandOverIsNewWhenOurDriverHasIt() {
        XCTAssertEqual(fresh([[volume("disk6s2", ours: false)], [volume("disk6s2")]]), [0, 1])
    }

    func testEjectingAndMountingAgainIsNewAgain() {
        XCTAssertEqual(fresh([[volume("disk6s2")],
                              [volume("disk6s2", mounted: false)],
                              [volume("disk6s2")]]), [1, 0, 1])
    }

    func testUnpluggingAndPluggingBackInIsNewAgain() {
        XCTAssertEqual(fresh([[volume("disk6s2", attachment: 1)],
                              [],
                              [volume("disk6s2", attachment: 2)]]), [1, 0, 1])
    }

    /// Read-only comes from a status file and "ours" from a process scan,
    /// both read every poll. A volume that stays mounted is one mount
    /// whatever they say in between.
    func testAFlickeringStatusIsNotNewTwice() {
        XCTAssertEqual(fresh([[volume("disk6s2")],
                              [volume("disk6s2", readOnly: true)],
                              [volume("disk6s2", ours: false)],
                              [volume("disk6s2")]]), [1, 0, 0, 0])
    }

    /// BSD names are recycled: a different disk that comes up as disk6 within
    /// one poll of the last one leaving is a different mount.
    func testADifferentDiskWearingTheSameNameIsNew() {
        XCTAssertEqual(fresh([[volume("disk6s2", attachment: 1)],
                              [volume("disk6s2", attachment: 2)]]), [1, 1])
    }

    func testOnlyTheWritablePartitionOfADiskIsNew() {
        XCTAssertEqual(fresh([[volume("disk6s2"),
                               volume("disk6s3", attachment: 4243, readOnly: true)]]), [1])
    }

    func testASiblingRemountedWhileTheOtherStaysMountedIsNew() {
        let s2 = volume("disk6s2"), s3 = volume("disk6s3", attachment: 4243)
        XCTAssertEqual(fresh([[s2, s3],
                              [s2, volume("disk6s3", attachment: 4243, mounted: false)],
                              [s2, s3]]), [2, 0, 1])
    }

    // MARK: occasions, and what is kept between runs

    private let suite = "ch.techtag.ntfs.tests.StarLine"
    private var store: UserDefaults!
    private let start = Date(timeIntervalSinceReferenceDate: 800_000_000)

    override func setUp() {
        super.setUp()
        store = UserDefaults(suiteName: suite)
        store.removePersistentDomain(forName: suite)
    }

    override func tearDown() {
        store.removePersistentDomain(forName: suite)
        super.tearDown()
    }

    /// One poll, `hours` after the start, in boot `boot`.
    private func poll(_ partitions: [Partition], at hours: Double, boot: String = "boot-1",
                      store: UserDefaults? = nil) -> Bool {
        StarLine.poll(partitions, now: start.addingTimeInterval(hours * 3600),
                      store: store ?? self.store, boot: boot)
    }

    /// A disk plugged in at each of `hours` and unplugged a minute later.
    private func plugIns(at hours: [Double]) -> [Bool] {
        hours.map { hour in
            let due = poll([volume("disk6s2", attachment: UInt64(1000 + hour * 10))], at: hour)
            _ = poll([], at: hour + 1.0 / 60)
            return due
        }
    }

    func testTheLineIsDueOnTheThirdOccasion() {
        XCTAssertEqual(plugIns(at: [0, 2, 4]), [false, false, true])
        XCTAssertTrue(StarLine.isDue(store))
    }

    /// A dock of three disks is one use, not three.
    func testDisksMountedTogetherAreOneOccasion() {
        let dock = [volume("disk6s2", attachment: 1), volume("disk7s1", attachment: 2),
                    volume("disk8s1", attachment: 3)]
        XCTAssertFalse(poll(dock, at: 0))
        XCTAssertEqual(plugIns(at: [2, 4]), [false, true])
    }

    /// FSKit mounts the partitions of a disk one after another, and someone
    /// sorting out a disk may eject and remount it a few times. All of that,
    /// within the hour, is one occasion.
    func testMountsWithinTheHourBelongToTheLastOccasion() {
        let s2 = volume("disk6s2", attachment: 1), s3 = volume("disk6s3", attachment: 2)
        XCTAssertFalse(poll([s2], at: 0))
        XCTAssertFalse(poll([s2, s3], at: 0.001))
        XCTAssertFalse(poll([], at: 0.2))
        XCTAssertFalse(poll([volume("disk6s2", attachment: 3)], at: 0.5))
        XCTAssertEqual(plugIns(at: [2, 4]), [false, true])
    }

    /// Quitting and reopening the app must not count a volume that stayed
    /// mounted: what was counted lives in the store, not in the process.
    func testAVolumeThatStaysMountedCountsOnceAcrossRelaunches() {
        let disk = [volume("disk6s2", attachment: 1)]
        XCTAssertFalse(poll(disk, at: 0))
        XCTAssertFalse(poll(disk, at: 5, store: UserDefaults(suiteName: suite)))     // reopened
        XCTAssertFalse(poll(disk, at: 30, store: UserDefaults(suiteName: suite)))    // and again
        XCTAssertEqual(plugIns(at: [32, 34]), [false, true])
    }

    /// After a restart the disk is mounted afresh, and the attachment ID it
    /// was counted under can come back for a different mount.
    func testARestartForgetsWhatWasCounted() {
        let disk = [volume("disk6s2", attachment: 1)]
        XCTAssertFalse(poll(disk, at: 0, boot: "boot-1"))
        XCTAssertFalse(poll(disk, at: 10, boot: "boot-2"))
        XCTAssertTrue(poll([volume("disk7s1", attachment: 2)], at: 12, boot: "boot-2"))
    }

    // MARK: retired for good

    /// The cross or the link in the menu, once the line was due.
    func testRetiredIsForGood() {
        XCTAssertEqual(plugIns(at: [0, 2, 4]), [false, false, true])
        StarLine.retire(store)
        XCTAssertFalse(StarLine.isDue(store))
        XCTAssertEqual(plugIns(at: [6, 8, 10]), [false, false, false])
    }

    /// The link in Settings › About can retire it before it is ever due.
    func testRetiredBeforeItIsDueItNeverComes() {
        StarLine.retire(store)
        XCTAssertEqual(plugIns(at: [0, 2, 4, 6]), [false, false, false, false])
    }

    /// Once due, it stays due until retired, whatever the disks do.
    func testOnceDueItStaysDue() {
        XCTAssertEqual(plugIns(at: [0, 2, 4]), [false, false, true])
        XCTAssertTrue(poll([], at: 5))
        XCTAssertTrue(poll([volume("disk9s1", attachment: 9)], at: 7))
    }
}
