// SPDX-License-Identifier: GPL-2.0
//
// The update check (NTFS/UpdateCheck.swift): which tags count as newer, when a
// check is due, how GitHub's answer is read, and that the request keeps the
// promises README.md makes about the app's only network request -- no cookies,
// no cache, no credentials, nothing but the app's name and version.
// UpdateCheckStateTests then drives the check itself with a fake clock, store
// and GitHub: nothing sent while off, at most one request a day, and what the
// user is shown.

import XCTest

final class UpdateCheckTests: XCTestCase {

    // MARK: versions

    func testTagsAreReadAsNumbers() {
        XCTAssertEqual(LatestRelease.components("v0.4.2"), [0, 4, 2])
        XCTAssertEqual(LatestRelease.components("0.4.2"), [0, 4, 2])
        XCTAssertEqual(LatestRelease.components("v1"), [1])
        XCTAssertEqual(LatestRelease.components("v0.04.10"), [0, 4, 10])
        XCTAssertEqual(LatestRelease.components("1.2.3.4"), [1, 2, 3, 4])
    }

    /// Anything else never produces a notice, whatever GitHub returns.
    func testAnythingButAPlainVersionIsRefused() {
        for tag in ["", "v", "?", "v0.5.0-rc1", "v0..4", "v0.4.", ".4.2", "V0.4.2",
                    "v 0.4.2", "v0.4.2\n", "v0.4.2 ", "1.2.3.4.5", "v1234567890.0",
                    "v٠.٤.٢", "v0.4.2/../../x", "latest"] {
            XCTAssertNil(LatestRelease.components(tag), "accepted \(tag.debugDescription)")
        }
    }

    func testNewerIsNumericNotAlphabetical() {
        XCTAssertTrue(LatestRelease.isNewer("v0.4.3", than: "0.4.2"))
        XCTAssertTrue(LatestRelease.isNewer("v0.4.10", than: "0.4.9"))
        XCTAssertTrue(LatestRelease.isNewer("v0.5", than: "0.4.9"))
        XCTAssertTrue(LatestRelease.isNewer("v1.0.0", than: "0.99.99"))
        XCTAssertTrue(LatestRelease.isNewer("v0.4.2.1", than: "0.4.2"))
    }

    func testSameOrOlderIsNotNewer() {
        XCTAssertFalse(LatestRelease.isNewer("v0.4.2", than: "0.4.2"))
        XCTAssertFalse(LatestRelease.isNewer("v0.4", than: "0.4.0"))
        XCTAssertFalse(LatestRelease.isNewer("v0.4.1", than: "0.4.2"))
        XCTAssertFalse(LatestRelease.isNewer("v0.4.9", than: "0.4.10"))
    }

    /// A tag or a bundle version that cannot be read -- BuildInfo shows "?"
    /// for a missing one -- must not claim an update.
    func testUnreadableVersionsAreNeverNewer() {
        XCTAssertFalse(LatestRelease.isNewer("v0.5.0-rc1", than: "0.4.2"))
        XCTAssertFalse(LatestRelease.isNewer("v0.4.3", than: "?"))
        XCTAssertFalse(LatestRelease.isNewer("v0.4.3", than: ""))
    }

    // MARK: when

    private let now = Date(timeIntervalSince1970: 1_795_000_000)

    func testFirstCheckIsDueAtOnce() {
        XCTAssertTrue(LatestRelease.isDue(lastCheck: nil, now: now))
    }

    func testOneCheckADay() {
        XCTAssertFalse(LatestRelease.isDue(lastCheck: now, now: now))
        XCTAssertFalse(LatestRelease.isDue(lastCheck: now.addingTimeInterval(-3_600), now: now))
        XCTAssertFalse(LatestRelease.isDue(lastCheck: now.addingTimeInterval(-86_399), now: now))
        XCTAssertTrue(LatestRelease.isDue(lastCheck: now.addingTimeInterval(-86_400), now: now))
        XCTAssertTrue(LatestRelease.isDue(lastCheck: now.addingTimeInterval(-30 * 86_400), now: now))
    }

    /// A clock set back puts the last check in the future; that must not bring
    /// a second request the same day.
    func testClockSetBackDoesNotMakeACheckDue() {
        XCTAssertFalse(LatestRelease.isDue(lastCheck: now.addingTimeInterval(2 * 3_600), now: now))
        XCTAssertFalse(LatestRelease.isDue(lastCheck: now.addingTimeInterval(86_400), now: now))
    }

    /// A clock that was set far ahead and then corrected must not silence the
    /// check until the wrong date comes round.
    func testLastCheckFarInTheFutureIsDue() {
        XCTAssertTrue(LatestRelease.isDue(lastCheck: now.addingTimeInterval(86_401), now: now))
        XCTAssertTrue(LatestRelease.isDue(lastCheck: now.addingTimeInterval(365 * 86_400), now: now))
    }

    // MARK: GitHub's answer

    func testTagIsReadFromTheAnswer() {
        let json = """
        {"url": "https://api.github.com/repos/dr-kbadawi/ttntfs/releases/1",
         "html_url": "https://github.com/dr-kbadawi/ttntfs/releases/tag/v0.4.3",
         "tag_name": "v0.4.3", "name": "0.4.3", "draft": false, "prerelease": false,
         "assets": [{"name": "TT-NTFS-Native.dmg", "size": 1600000}], "body": "Notes"}
        """
        XCTAssertEqual(LatestRelease.tag(fromResponse: Data(json.utf8)), "v0.4.3")
        XCTAssertEqual(LatestRelease.tag(fromResponse: Data(#"{"tag_name": "v0.5"}"#.utf8)), "v0.5")
    }

    func testDraftsPreReleasesAndJunkAreIgnored() {
        let answers = [
            #"{"tag_name": "v0.5.0", "draft": true, "prerelease": false}"#,
            #"{"tag_name": "v0.5.0", "draft": false, "prerelease": true}"#,
            #"{"tag_name": "v0.5.0-beta", "draft": false, "prerelease": false}"#,
            #"{"tag_name": 5}"#,
            #"{"message": "API rate limit exceeded"}"#,
            #"{"message": "Not Found", "status": "404"}"#,
            "<html>",
            "",
        ]
        for answer in answers {
            XCTAssertNil(LatestRelease.tag(fromResponse: Data(answer.utf8)), answer)
        }
    }

    // MARK: the request

    func testRequestCarriesNoCookiesCacheOrCredentials() {
        let config = LatestRelease.sessionConfiguration(appVersion: "0.4.2")
        XCTAssertNil(config.httpCookieStorage)
        XCTAssertFalse(config.httpShouldSetCookies)
        XCTAssertEqual(config.httpCookieAcceptPolicy, .never)
        XCTAssertNil(config.urlCache)
        XCTAssertNil(config.urlCredentialStorage)
        XCTAssertEqual(config.requestCachePolicy, .reloadIgnoringLocalCacheData)
    }

    func testRequestNamesTheAppAndNothingElse() {
        let headers = LatestRelease.sessionConfiguration(appVersion: "0.4.2").httpAdditionalHeaders
        XCTAssertEqual(headers?["User-Agent"] as? String, "TT-NTFS-Native/0.4.2")
        XCTAssertEqual(headers?["Accept-Language"] as? String, "en")
        XCTAssertEqual(headers?.count, 2)

        let request = LatestRelease.request()
        XCTAssertEqual(request.url?.absoluteString,
                       "https://api.github.com/repos/dr-kbadawi/ttntfs/releases/latest")
        XCTAssertEqual(request.httpMethod, "GET")
        XCTAssertNil(request.httpBody)
        XCTAssertEqual(request.allHTTPHeaderFields, ["Accept": "application/vnd.github+json"])
    }

    /// The button opens a page built from a validated tag, never an address
    /// taken from the answer.
    func testReleasePageIsBuiltFromTheTag() {
        XCTAssertEqual(LatestRelease.page(for: "v0.4.3")?.absoluteString,
                       "https://github.com/dr-kbadawi/ttntfs/releases/tag/v0.4.3")
        XCTAssertNil(LatestRelease.page(for: "v0.4.3/../../../evil"))
        XCTAssertNil(LatestRelease.page(for: "https://example.com"))
        XCTAssertEqual(LatestRelease.displayVersion("v0.4.3"), "0.4.3")
        XCTAssertEqual(LatestRelease.displayVersion("0.4.3"), "0.4.3")
    }
}

// MARK: - The check itself

private final class MemoryStore: UpdateCheckStore {
    var values: [String: Any] = [:]
    func object(forKey defaultName: String) -> Any? { values[defaultName] }
    func set(_ value: Any?, forKey defaultName: String) { values[defaultName] = value }
}

/// Stands in for api.github.com: records each request and gives a fixed
/// answer, held back on request so a test can act while one is under way.
private final class FakeGitHub {
    var answer: String? = "v0.4.3"
    private(set) var requests: [String] = []    // the app version each one named
    var holdNextAnswer = false
    private var held: CheckedContinuation<Void, Never>?

    @MainActor func fetch(_ appVersion: String) async -> String? {
        requests.append(appVersion)
        if holdNextAnswer {
            holdNextAnswer = false
            await withCheckedContinuation { held = $0 }
        }
        return answer
    }

    @MainActor func releaseHeldAnswer() {
        held?.resume()
        held = nil
    }
}

@MainActor
final class UpdateCheckStateTests: XCTestCase {
    private let store = MemoryStore()
    private let github = FakeGitHub()
    private var now = Date(timeIntervalSince1970: 1_795_000_000)
    private var homebrew = false

    private func makeCheck(version: String? = "0.4.2") -> UpdateCheck {
        UpdateCheck(UpdateCheck.Environment(
            store: store,
            now: { [unowned self] in self.now },
            runningVersion: version,
            fetchTag: { [github] in await github.fetch($0) },
            installedWithHomebrew: { [unowned self] in self.homebrew }))
    }

    /// Lets the check's task run until the fake has received the request.
    private func waitForRequest() async {
        while github.requests.isEmpty { await Task.yield() }
    }

    func testFirstCheckAsksOnceAndShowsTheNewerVersion() async {
        let check = makeCheck()
        await check.checkIfDue()?.value
        XCTAssertEqual(github.requests, ["0.4.2"])
        XCTAssertEqual(check.available, UpdateCheck.Available(
            version: "0.4.3",
            page: URL(string: "https://github.com/dr-kbadawi/ttntfs/releases/tag/v0.4.3")!,
            viaHomebrew: false))
        XCTAssertEqual(store.values[UpdateCheck.lastCheckKey] as? Date, now)
    }

    func testAtMostOneRequestADay() async {
        let check = makeCheck()
        await check.checkIfDue()?.value
        now += 23 * 3_600
        let tooSoon = check.checkIfDue()
        XCTAssertNil(tooSoon)
        now += 3_600
        await check.checkIfDue()?.value
        XCTAssertEqual(github.requests.count, 2)
    }

    /// The stamp goes on before the request, so a failure does not turn into
    /// a retry every hour.
    func testAFailedCheckAlsoWaitsADay() async {
        github.answer = nil
        let check = makeCheck()
        await check.checkIfDue()?.value
        now += 3_600
        let retry = check.checkIfDue()
        XCTAssertNil(retry)
        XCTAssertEqual(github.requests.count, 1)
        XCTAssertNil(check.available)
    }

    func testOffMeansNoRequestAtAll() {
        store.values[UpdateCheck.enabledKey] = false
        let check = makeCheck()
        XCTAssertFalse(check.isEnabled)
        let first = check.checkIfDue()
        now += 30 * 86_400
        let later = check.checkIfDue()
        XCTAssertNil(first)
        XCTAssertNil(later)
        XCTAssertTrue(github.requests.isEmpty)
    }

    func testSwitchingOffMidRequestKeepsNothing() async {
        github.holdNextAnswer = true
        let check = makeCheck()
        let task = check.checkIfDue()
        await waitForRequest()
        check.setEnabled(false)
        github.releaseHeldAnswer()
        await task?.value
        XCTAssertNil(store.values[UpdateCheck.latestTagKey])
        XCTAssertNil(check.available)
        XCTAssertEqual(store.values[UpdateCheck.enabledKey] as? Bool, false)
    }

    func testSwitchingBackOnTheSameDayDoesNotAskAgain() async {
        let check = makeCheck()
        await check.checkIfDue()?.value
        check.setEnabled(false)
        XCTAssertNil(check.available)
        check.setEnabled(true)
        XCTAssertEqual(check.available?.version, "0.4.3")      // from the stored answer
        XCTAssertEqual(github.requests.count, 1)
    }

    func testRelaunchTheSameDayShowsTheNoticeWithoutAsking() async {
        await makeCheck().checkIfDue()?.value
        now += 3_600
        let relaunched = makeCheck()
        let request = relaunched.checkIfDue()
        XCTAssertNil(request)
        XCTAssertEqual(relaunched.available?.version, "0.4.3")
        XCTAssertEqual(github.requests.count, 1)
    }

    func testNoticeGoesOnceThisCopyIsThatVersion() async {
        await makeCheck().checkIfDue()?.value
        XCTAssertNil(makeCheck(version: "0.4.3").available)
        XCTAssertNil(makeCheck(version: "0.5.0").available)
    }

    /// A plain Xcode build carries a placeholder version, and would announce
    /// the release it was built from.
    func testABuildThatCannotSayItsVersionNeitherAsksNorTells() async {
        await makeCheck().checkIfDue()?.value           // an installed copy stored v0.4.3
        now += 2 * 86_400
        let unstamped = makeCheck(version: nil)
        let request = unstamped.checkIfDue()
        XCTAssertNil(request)
        XCTAssertNil(unstamped.available)
        XCTAssertEqual(github.requests.count, 1)
    }

    func testHomebrewInstallIsToldToUseBrew() async {
        homebrew = true
        let check = makeCheck()
        await check.checkIfDue()?.value
        XCTAssertEqual(check.available?.viaHomebrew, true)
    }

    func testClockSetBackDoesNotBringASecondRequest() async {
        let check = makeCheck()
        await check.checkIfDue()?.value
        now -= 2 * 3_600
        let again = check.checkIfDue()
        XCTAssertNil(again)
        XCTAssertEqual(github.requests.count, 1)
    }

    /// What the uninstaller relies on: after stop() nothing is asked, and an
    /// answer still on its way is not written back into deleted settings.
    func testAfterStopNothingIsAskedOrWritten() async {
        github.holdNextAnswer = true
        let check = makeCheck()
        let task = check.checkIfDue()
        await waitForRequest()
        check.stop()
        github.releaseHeldAnswer()
        await task?.value
        XCTAssertNil(store.values[UpdateCheck.latestTagKey])
        now += 2 * 86_400
        let after = check.checkIfDue()
        XCTAssertNil(after)
        XCTAssertEqual(github.requests.count, 1)
    }
}
