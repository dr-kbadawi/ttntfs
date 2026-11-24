// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
//
// The app's only network request, all of it in this file.
//
// At most once a day the app asks GitHub's public API for the latest release of
// this project and compares its tag with this copy's version. When the release
// is newer, the menu-bar panel and Settings → About say so. The request is a GET
// of api.github.com/repos/dr-kbadawi/ttntfs/releases/latest. Its User-Agent is
// the app's name and version, and that is all it says about anything: it goes
// out on an ephemeral session with no cookies, cache or credentials, and carries
// nothing about the user, the Mac or its disks. "Check for updates" in Settings
// turns it off, and then no request is made at all. A failed check is not
// reported; the next one is a day later. A build that scripts/package.sh did not
// stamp does not check: its version is a placeholder.
//
// It only tells. Nothing is downloaded or installed, and that is deliberate:
// the first launch of a new copy re-registers the extension, which stops the
// running one and unmounts every volume it serves (fskit/README.md, step 4).
// So the user has to eject first and choose the moment, and the notice says so.

import Foundation
import AppKit
import SwiftUI

/// The calls the check makes on its settings: UserDefaults in the app, a
/// dictionary in the tests.
protocol UpdateCheckStore: AnyObject {
    func object(forKey defaultName: String) -> Any?
    func set(_ value: Any?, forKey defaultName: String)
}

extension UserDefaults: UpdateCheckStore {}

@MainActor
final class UpdateCheck: ObservableObject {
    static let shared = UpdateCheck(.live)

    /// A release newer than this copy.
    struct Available: Equatable {
        let version: String         // "0.4.3"
        let page: URL               // the release on GitHub: notes and the DMG
        /// Installed with Homebrew, so `brew upgrade` is the way to update; a
        /// copy dragged in from the DMG would leave brew's record behind.
        let viaHomebrew: Bool
    }

    /// What the check takes from outside, passed in so that the tests can drive
    /// it with their own settings, clock and answers (UpdateCheckTests).
    struct Environment {
        var store: UpdateCheckStore
        var now: () -> Date
        /// nil for a build that cannot say which version it is.
        var runningVersion: String?
        var fetchTag: (_ appVersion: String) async -> String?
        var installedWithHomebrew: () -> Bool

        static var live: Environment {
            Environment(store: SharedSettings.store,
                        now: { Date() },
                        runningVersion: LatestRelease.runningVersion,
                        fetchTag: { await LatestRelease.fetchTag(appVersion: $0) },
                        installedWithHomebrew: { LatestRelease.installedWithHomebrew })
        }
    }

    @Published private(set) var isEnabled: Bool
    @Published private(set) var available: Available? = nil

    // In SharedSettings.store, like the login item's flag, so the uninstaller
    // deletes them with the rest of the app's settings.
    static let enabledKey = "checkForUpdates"
    static let lastCheckKey = "updateCheckLastAttempt"
    static let latestTagKey = "updateCheckLatestTag"

    private let env: Environment
    private var timer: Timer?
    private var inFlight: Task<Void, Never>?
    private var stopped = false

    init(_ env: Environment) {
        self.env = env
        isEnabled = env.store.object(forKey: Self.enabledKey) as? Bool ?? true
        refreshNotice()
    }

    /// At launch: ask now if the last check was a day or more ago, then look at
    /// the clock every hour. The hour only sets how soon after the day comes
    /// round the request goes out; there is still at most one a day.
    func start() {
        guard timer == nil, !stopped else { return }
        checkIfDue()
        timer = Timer.scheduledTimer(withTimeInterval: 60 * 60, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.checkIfDue() }
        }
        timer?.tolerance = 5 * 60
    }

    /// For the uninstaller, before it deletes the settings: from here on nothing
    /// is asked and nothing is written, so a check that finishes late cannot put
    /// back what was just deleted.
    func stop() {
        stopped = true
        timer?.invalidate()
        timer = nil
        inFlight?.cancel()
        inFlight = nil
    }

    func setEnabled(_ on: Bool) {
        env.store.set(on, forKey: Self.enabledKey)
        isEnabled = on
        if on {
            refreshNotice()
            checkIfDue()
        } else {
            inFlight?.cancel()
            inFlight = nil
            available = nil
        }
    }

    /// Sends the request if one is due, and returns its task so that the tests
    /// can wait for it.
    @discardableResult
    func checkIfDue() -> Task<Void, Never>? {
        guard isEnabled, !stopped, inFlight == nil, let version = env.runningVersion else { return nil }
        let now = env.now()
        guard LatestRelease.isDue(lastCheck: env.store.object(forKey: Self.lastCheckKey) as? Date,
                                  now: now) else { return nil }
        // Stamped before asking, so a failure waits a day, as a success does.
        env.store.set(now, forKey: Self.lastCheckKey)
        let task = Task {
            let tag = await env.fetchTag(version)
            guard !Task.isCancelled else { return }     // switched off or stopped meanwhile
            inFlight = nil
            guard let tag else { return }
            env.store.set(tag, forKey: Self.latestTagKey)
            refreshNotice()
        }
        inFlight = task
        return task
    }

    /// From the stored tag rather than a new request, so the notice survives a
    /// relaunch, and goes away by itself once this copy is that version.
    private func refreshNotice() {
        guard isEnabled,
              let version = env.runningVersion,
              let tag = env.store.object(forKey: Self.latestTagKey) as? String,
              LatestRelease.isNewer(tag, than: version),
              let page = LatestRelease.page(for: tag) else {
            available = nil
            return
        }
        available = Available(version: LatestRelease.displayVersion(tag), page: page,
                              viaHomebrew: env.installedWithHomebrew())
    }
}

/// What is asked and how the answer is read, with no state, so that the tests
/// (UpdateCheckTests) can hold each piece to what the top of this file promises.
enum LatestRelease {
    static let endpoint = URL(string: "https://api.github.com/repos/dr-kbadawi/ttntfs/releases/latest")!
    static let interval: TimeInterval = 24 * 60 * 60

    /// This copy's version: the CFBundleShortVersionString that Settings →
    /// About shows through BuildInfo. nil for a build that scripts/package.sh
    /// did not stamp, such as a plain Xcode build: it carries the project's
    /// placeholder MARKETING_VERSION (project.yml), which says nothing about the
    /// code in it, and it would announce the very release it was built from.
    static var runningVersion: String? {
        let info = Bundle.main.infoDictionary ?? [:]
        guard (info["TTGitCommit"] as? String) != "xcode" else { return nil }
        return info["CFBundleShortVersionString"] as? String
    }

    /// The cask's folder under either Homebrew prefix: Apple Silicon's, and the
    /// Intel one that some Macs still carry under Rosetta.
    static var installedWithHomebrew: Bool {
        ["/opt/homebrew/Caskroom/ttntfs", "/usr/local/Caskroom/ttntfs"]
            .contains { FileManager.default.fileExists(atPath: $0) }
    }

    static func isDue(lastCheck: Date?, now: Date) -> Bool {
        guard let lastCheck else { return true }
        let elapsed = now.timeIntervalSince(lastCheck)
        // A clock set back puts the last check in the future. Up to a day ahead,
        // wait for the day to pass, so there is no second request the same day;
        // the wait is two days at most. Further ahead, the stamp came from a clock
        // set wrong, and waiting for it would silence the check for as long.
        return elapsed >= interval || elapsed < -interval
    }

    /// Ephemeral, and then some: nothing is kept, and nothing is sent beyond
    /// the request itself.
    static func sessionConfiguration(appVersion: String) -> URLSessionConfiguration {
        let config = URLSessionConfiguration.ephemeral
        config.httpCookieStorage = nil
        config.httpCookieAcceptPolicy = .never
        config.httpShouldSetCookies = false
        config.urlCredentialStorage = nil
        config.urlCache = nil
        config.requestCachePolicy = .reloadIgnoringLocalCacheData
        config.httpAdditionalHeaders = [
            // In place of the default, which also names the CFNetwork and
            // Darwin versions.
            "User-Agent": "TT-NTFS-Native/\(appVersion)",
            // Otherwise filled in from the user's preferred languages.
            "Accept-Language": "en",
        ]
        // At login the network is often not up yet. Wait for it, for up to an
        // hour, rather than fail and lose the day.
        config.waitsForConnectivity = true
        config.timeoutIntervalForRequest = 30
        config.timeoutIntervalForResource = 60 * 60
        return config
    }

    static func request(_ url: URL = endpoint) -> URLRequest {
        var request = URLRequest(url: url)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        return request
    }

    /// The latest release's tag, or nil on any failure, which nobody is told about.
    static func fetchTag(appVersion: String) async -> String? {
        let session = URLSession(configuration: sessionConfiguration(appVersion: appVersion))
        defer { session.finishTasksAndInvalidate() }
        do {
            let (data, response) = try await session.data(for: request())
            guard (response as? HTTPURLResponse)?.statusCode == 200 else { return nil }
            return tag(fromResponse: data)
        } catch {
            return nil
        }
    }

    /// `tag_name` of a /releases/latest answer, if it is a plain version tag.
    /// That endpoint never returns a draft or a pre-release; checked again here
    /// so that a change on GitHub's side cannot turn one into a notice.
    static func tag(fromResponse data: Data) -> String? {
        struct Release: Decodable {
            let tagName: String
            let draft: Bool?
            let prerelease: Bool?
        }
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        guard let release = try? decoder.decode(Release.self, from: data),
              release.draft != true, release.prerelease != true,
              components(release.tagName) != nil else { return nil }
        return release.tagName
    }

    /// "v0.4.10" → [0, 4, 10]. Anything but an optional "v" and one to four
    /// dot-separated runs of ASCII digits is nil -- "v0.5.0-rc1", "0..4",
    /// BuildInfo's "?" -- and nil is never newer than anything.
    static func components(_ version: String) -> [Int]? {
        let bare = version.hasPrefix("v") ? version.dropFirst() : Substring(version)
        let parts = bare.split(separator: ".", omittingEmptySubsequences: false)
        guard (1...4).contains(parts.count) else { return nil }
        var numbers: [Int] = []
        for part in parts {
            guard (1...9).contains(part.utf8.count),
                  part.utf8.allSatisfy({ $0 >= UInt8(ascii: "0") && $0 <= UInt8(ascii: "9") }),
                  let number = Int(part) else { return nil }
            numbers.append(number)
        }
        return numbers
    }

    /// Part by part, as numbers, a missing part counting as 0: 0.4.10 is newer
    /// than 0.4.9, and 0.5 is the same as 0.5.0.
    static func isNewer(_ candidate: String, than current: String) -> Bool {
        guard let new = components(candidate), let old = components(current) else { return false }
        for i in 0..<max(new.count, old.count) {
            let a = i < new.count ? new[i] : 0
            let b = i < old.count ? old[i] : 0
            if a != b { return a > b }
        }
        return false
    }

    static func displayVersion(_ tag: String) -> String {
        tag.hasPrefix("v") ? String(tag.dropFirst()) : tag
    }

    /// The release's page, built from a tag that passed components(), so the
    /// app never opens an address that came over the network.
    static func page(for tag: String) -> URL? {
        guard components(tag) != nil else { return nil }
        return URL(string: "https://github.com/dr-kbadawi/ttntfs/releases/tag/\(tag)")
    }
}

/// "Version X is available", in the menu-bar panel and in Settings → About.
/// Nothing at all unless the check has found a newer version.
struct UpdateNotice: View {
    @ObservedObject private var check = UpdateCheck.shared

    var body: some View {
        if let new = check.available {
            UpdateNoticeRow(new: new)
        }
    }
}

/// The headline shares its line with the button; what to do comes underneath,
/// across the full width, where the Homebrew command fits on one line.
struct UpdateNoticeRow: View {
    let new: UpdateCheck.Available

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            Image(systemName: "arrow.down.circle")
                .foregroundStyle(.secondary)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                HStack(alignment: .firstTextBaseline, spacing: 4) {
                    Text("Version \(new.version) is available.")
                        .foregroundStyle(.secondary)
                    Spacer(minLength: 4)
                    Button("Release Page") { NSWorkspace.shared.open(new.page) }
                        .controlSize(.small)
                        .help(new.page.absoluteString)
                }
                instructions
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)            // so the brew command can be copied
                    .help("The first launch of the new copy restarts the file system extension, "
                          + "which unmounts every NTFS disk it serves.")
            }
        }
        .font(.caption)
    }

    /// `brew update` first: `brew upgrade` alone does not fetch the tap again
    /// within a day of Homebrew's last fetch (its auto-update.sh), so it would
    /// often find nothing to upgrade. The command gets a line of its own;
    /// wrapped, it broke after "--".
    private var instructions: Text {
        new.viaHomebrew
            ? Text("Eject NTFS disks, then run\n`brew update && brew upgrade --cask ttntfs`")
            : Text("Eject NTFS disks before you install it.")
    }
}
