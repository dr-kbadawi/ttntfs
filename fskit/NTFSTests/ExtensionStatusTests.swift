// SPDX-License-Identifier: GPL-2.0
//
// How the menu learns whether the extension is on, from the fix "keep the
// menu's enable state current after a change in System Settings".
//
// Refreshes run on their own and can finish out of order: one started just
// after fskit_agent was restarted can answer late. Without the ordering, that
// older answer would overwrite a newer one and put the menu back to a state
// that no longer holds. And FSClient fails for a moment while fskit_agent
// restarts: such a failure must not overwrite what is already known.

import XCTest

final class ExtensionStatusTests: XCTestCase {

    // MARK: the order of answers

    func testAnswersInTheOrderAskedAreAllUsed() {
        var order = RefreshOrder()
        let first = order.start(), second = order.start()
        XCTAssertTrue(order.accept(first))
        XCTAssertTrue(order.accept(second))
    }

    /// The case the ordering exists for: the older answer arrives last.
    func testAnOlderAnswerAfterANewerOneIsDropped() {
        var order = RefreshOrder()
        let first = order.start(), second = order.start()
        XCTAssertTrue(order.accept(second))
        XCTAssertFalse(order.accept(first))
    }

    func testEachAnswerIsUsedOnce() {
        var order = RefreshOrder()
        let only = order.start()
        XCTAssertTrue(order.accept(only))
        XCTAssertFalse(order.accept(only))
    }

    func testARefreshStartedAfterAnAnswerIsUsed() {
        var order = RefreshOrder()
        let first = order.start()
        XCTAssertTrue(order.accept(first))
        let later = order.start()
        XCTAssertTrue(order.accept(later))
    }

    /// Three in flight, answering second, first, third: the first is dropped.
    func testOnlyNewerAnswersGetThrough() {
        var order = RefreshOrder()
        let first = order.start(), second = order.start(), third = order.start()
        XCTAssertTrue(order.accept(second))
        XCTAssertFalse(order.accept(first))
        XCTAssertTrue(order.accept(third))
    }

    // MARK: a refresh that fails

    /// An error is no answer: a state already known stays.
    func testAFailedRefreshKeepsWhatIsKnown() {
        for known: ExtensionStatus.State in [.enabled, .disabled, .notInstalled] {
            XCTAssertEqual(ExtensionStatus.stateAfterFailedRefresh(known), known, "\(known)")
        }
    }

    /// With nothing known yet, "not registered" is the best guess, so the menu
    /// offers to set the extension up rather than wait.
    func testAFailedFirstRefreshGuessesNotInstalled() {
        XCTAssertEqual(ExtensionStatus.stateAfterFailedRefresh(.unknown), .notInstalled)
    }
}
