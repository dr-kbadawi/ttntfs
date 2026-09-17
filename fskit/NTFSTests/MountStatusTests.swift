// SPDX-License-Identifier: GPL-2.0
//
// MountStatus is the only channel between the extension and the app: one JSON
// file in the app-group container. The app believes what it says -- a volume
// our driver is refusing writes to still looks read/write to the kernel -- so
// a round-trip that loses a field makes the app lie about a mounted disk.

import XCTest

final class MountStatusTests: XCTestCase {

    private func sample(bsdName: String = "disk6s1") -> MountStatus {
        MountStatus(bsdName: bsdName,
                    label: "Windows Data",
                    readOnly: true,
                    roReason: 5,
                    roReasonText: MountStatus.reasonText(5),
                    dirty: true,
                    hibernated: true,
                    logfileClean: false,
                    coreVersion: "ntfs 7.1-port",
                    mountedAt: Date(timeIntervalSince1970: 1_757_000_000),
                    journalSummary: "12 transactions would be replayed")
    }

    // MARK: reasonText

    /// Every ntfs_ro_reason the core can report must produce its own sentence.
    /// Two reasons sharing one message means the menu tells the user to do the
    /// wrong thing: chkdsk does not clear a hibernation image.
    func testEveryReasonHasItsOwnNonEmptyMessage() {
        var seen: [String: Int] = [:]
        for reason in 1...7 {                       // NTFS_RO_REQUESTED ... NTFS_RO_ERRORS
            let text = MountStatus.reasonText(reason)
            XCTAssertFalse(text.isEmpty, "reason \(reason) has no message")
            if let other = seen[text] {
                XCTFail("reasons \(other) and \(reason) share a message: \(text)")
            }
            seen[text] = reason
        }
    }

    /// NTFS_RO_NONE is not a reason at all: the volume is read/write and the
    /// app shows nothing. An explanatory sentence here would be a false claim.
    func testNoReasonIsEmpty() {
        XCTAssertEqual(MountStatus.reasonText(0), "")
    }

    /// A newer core could add a reason this build has never heard of. It must
    /// still say something true rather than crash or claim read/write.
    func testUnknownReasonFallsBackWithoutCrashing() {
        for reason in [8, 99, -1, Int.max, Int.min] {
            let text = MountStatus.reasonText(reason)
            XCTAssertFalse(text.isEmpty, "unknown reason \(reason) produced no message")
        }
    }

    /// NTFS_RO_DEVICE has its own message, so a write-protected device is
    /// already covered by its own reason code rather than by qualifying another
    /// one. reasonText() used to take a deviceReadOnly flag that no caller
    /// passed and that changed nothing; it was removed on 2026-09-14 rather
    /// than left for someone to assume it worked.
    func testTheDeviceReasonHasItsOwnMessage() {
        let device = MountStatus.reasonText(2)          // NTFS_RO_DEVICE
        XCTAssertFalse(device.isEmpty)
        for reason in [1, 3, 4, 5, 6, 7] {
            XCTAssertNotEqual(MountStatus.reasonText(reason), device,
                              "reason \(reason) reuses the write-protected-device wording")
        }
    }

    // MARK: Codable

    func testRoundTripPreservesEveryField() throws {
        let original = sample()
        let enc = JSONEncoder()
        enc.dateEncodingStrategy = .iso8601
        let dec = JSONDecoder()
        dec.dateDecodingStrategy = .iso8601
        let back = try dec.decode(MountStatus.self, from: enc.encode(original))
        XCTAssertEqual(back, original)
    }

    func testDictionaryRoundTripMatchesWhatUpdateWrites() throws {
        let all = ["disk6s1": sample(), "disk7s2": sample(bsdName: "disk7s2")]
        let enc = JSONEncoder()
        enc.dateEncodingStrategy = .iso8601
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        let dec = JSONDecoder()
        dec.dateDecodingStrategy = .iso8601
        XCTAssertEqual(try dec.decode([String: MountStatus].self, from: enc.encode(all)), all)
    }

    /// A status file written before journalSummary existed must still decode.
    ///
    /// It did not until 2026-09-14. journalSummary carries a default, which
    /// reads as if the field were optional on the wire, but Swift's synthesised
    /// decoder ignores property defaults and throws on a missing key -- and
    /// because readAll() decodes the whole dictionary in one go, one entry from
    /// an older build made the app show NO status for ANY volume until every
    /// one of them was remounted. This test was written expecting the opposite
    /// and failed, which is how the bug was found. MountStatus now decodes by
    /// hand; every field added from here on needs decodeIfPresent.
    func testAFileFromBeforeJournalSummaryExistedStillDecodes() throws {
        let json = """
        {"disk6s1":{"bsdName":"disk6s1","label":"NTFS","readOnly":false,"roReason":0,
        "roReasonText":"","dirty":false,"hibernated":false,"logfileClean":true,
        "coreVersion":"x","mountedAt":"2026-09-13T10:00:00Z"}}
        """
        let all = MountStatus.readAll(from: try write(json))
        XCTAssertEqual(all["disk6s1"]?.bsdName, "disk6s1")
        XCTAssertEqual(all["disk6s1"]?.journalSummary, "", "the default must be used")
    }

    /// A genuinely required field missing is still an error: leniency is for
    /// fields added later, not for a truncated or foreign file.
    func testAFileMissingARequiredFieldDoesNotDecode() throws {
        let json = """
        {"disk6s1":{"bsdName":"disk6s1","label":"NTFS","readOnly":false,
        "roReasonText":"","dirty":false,"hibernated":false,"logfileClean":true,
        "coreVersion":"x","mountedAt":"2026-09-13T10:00:00Z"}}
        """
        XCTAssertTrue(MountStatus.readAll(from: try write(json)).isEmpty)
    }

    /// The same file with the field present decodes, which is what a running
    /// extension writes: the encoder always emits every field.
    func testAFileWithJournalSummaryPresentDecodes() throws {
        let json = """
        {"disk6s1":{"bsdName":"disk6s1","label":"NTFS","readOnly":false,"roReason":0,
        "roReasonText":"","dirty":false,"hibernated":false,"logfileClean":true,
        "coreVersion":"x","mountedAt":"2026-09-13T10:00:00Z","journalSummary":""}}
        """
        let all = MountStatus.readAll(from: try write(json))
        XCTAssertEqual(all["disk6s1"]?.bsdName, "disk6s1")
        XCTAssertEqual(all["disk6s1"]?.journalSummary, "")
    }

    // MARK: readAll

    /// The status file is written by whichever extension process mounted a
    /// volume, and read by the app at startup and every poll. Nothing about it
    /// is guaranteed: it may not exist yet, or may be a partial write. Every
    /// one of those must read as "nothing is mounted", never as a throw.
    func testMissingFileReadsAsEmpty() {
        let missing = FileManager.default.temporaryDirectory
            .appendingPathComponent("ttntfs-tests-absent-\(UUID().uuidString).json")
        XCTAssertTrue(MountStatus.readAll(from: missing).isEmpty)
    }

    /// fileURL is nil when the app-group container cannot be resolved, and
    /// readAll() hands that straight through.
    func testNilURLReadsAsEmpty() {
        XCTAssertTrue(MountStatus.readAll(from: nil).isEmpty)
    }

    func testCorruptFilesReadAsEmpty() throws {
        for junk in ["", "{", "not json at all", "[]", "null",
                     #"{"disk6s1":{"bsdName":"disk6s1"}}"#,               // fields missing
                     #"{"disk6s1":{"bsdName":42}}"#,                      // wrong type
                     #"{"disk6s1":"a string"}"#] {
            XCTAssertTrue(MountStatus.readAll(from: try write(junk)).isEmpty,
                          "junk file decoded to something: \(junk)")
        }
    }

    /// A date the encoder never writes in ISO 8601 form. The decoder is pinned
    /// to .iso8601, so this is a whole-file failure, not one bad entry.
    func testAFileWithAnUnexpectedDateFormatReadsAsEmpty() throws {
        let json = #"{"disk6s1":{"bsdName":"disk6s1","label":"","readOnly":false,"roReason":0,"roReasonText":"","dirty":false,"hibernated":false,"logfileClean":true,"coreVersion":"x","mountedAt":770000000}}"#
        XCTAssertTrue(MountStatus.readAll(from: try write(json)).isEmpty)
    }

    func testAWellFormedFileReadsBack() throws {
        let enc = JSONEncoder()
        enc.dateEncodingStrategy = .iso8601
        let all = ["disk6s1": sample()]
        let url = try write(String(decoding: try enc.encode(all), as: UTF8.self))
        XCTAssertEqual(MountStatus.readAll(from: url), all)
    }

    /// The real readAll() reads the app-group container path, which these tests
    /// deliberately do not touch. All it is asked to prove is that it answers
    /// without throwing whatever is (or is not) on this machine.
    ///
    /// Skipped when there is no container. `fileURL` calls
    /// `containerURL(forSecurityApplicationGroupIdentifier:)`, which needs the
    /// App Group entitlement the test bundle does not carry; on a machine where
    /// the container has never been created -- a fresh CI runner -- that call
    /// goes to containermanagerd and was reported hanging there on 2026-09-14
    /// under CODE_SIGNING_ALLOWED=NO. It did not reproduce afterwards, so this
    /// is a guard rather than a fix: the value of the assertion is not worth a
    /// suite that can hang.
    func testReadAllOnTheRealPathDoesNotCrash() throws {
        try XCTSkipIf(MountStatus.fileURL == nil,
                      "no app-group container for this bundle; nothing to read")
        _ = MountStatus.readAll()
    }

    /*
     * journalPendingOps decides whether the user sees "Close Journal" with a
     * reassurance or "Replay Journal" with a data-loss warning. Wrong in one
     * direction shows a frightening warning for a harmless Fast Startup
     * shutdown; wrong in the other hides a real risk. Both are pinned.
     */
    func testJournalPendingOpsDecidesTheWarning() throws {
        let head = """
        {"disk9s1":{"bsdName":"disk9s1","label":"NTFS","readOnly":true,"roReason":5,
        "roReasonText":"x","dirty":false,"hibernated":false,"logfileClean":false,
        "coreVersion":"t","mountedAt":"2026-09-17T10:00:00Z"
        """

        // absent (a status file predating the field) -> unknown, treated as risky
        let older = try write(head + "}}")
        XCTAssertEqual(MountStatus.readAll(from: older)["disk9s1"]?.journalPendingOps, -1,
                       "a file without the field must read as unknown, never as zero")

        // zero -> the Fast Startup case: nothing to replay, safe wording
        let zero = try write(head + ",\"journalPendingOps\":0}}")
        XCTAssertEqual(MountStatus.readAll(from: zero)["disk9s1"]?.journalPendingOps, 0)

        // positive -> genuine pending work, keep the warning
        let some = try write(head + ",\"journalPendingOps\":161}}")
        XCTAssertEqual(MountStatus.readAll(from: some)["disk9s1"]?.journalPendingOps, 161)
    }

    // MARK: helpers

    private var scratch: [URL] = []

    /// A status file of our own, so nothing here can read or disturb the one a
    /// running extension shares with the app.
    private func write(_ contents: String) throws -> URL {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("ttntfs-tests-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        scratch.append(dir)
        let url = dir.appendingPathComponent(MountStatus.fileName)
        try contents.write(to: url, atomically: true, encoding: .utf8)
        return url
    }

    override func tearDownWithError() throws {
        for dir in scratch { try? FileManager.default.removeItem(at: dir) }
        scratch = []
    }
}
