// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH

import XCTest

final class UnicodeNameAlternativesTests: XCTestCase {
    func testDecomposedKanaOffersNFC() {
        XCTAssertEqual(unicodeNameAlternatives("\u{304B}\u{3099}").map { Array($0.utf8) },
                       [Array("\u{304C}".utf8)])
    }

    func testPrecomposedLatinOffersNFD() {
        XCTAssertEqual(unicodeNameAlternatives("caf\u{00E9}").map { Array($0.utf8) },
                       [Array("cafe\u{0301}".utf8)])
    }

    func testHangulOffersDecomposedJamo() {
        XCTAssertEqual(unicodeNameAlternatives("\u{D55C}").map { Array($0.utf8) },
                       [Array("\u{1112}\u{1161}\u{11AB}".utf8)])
    }

    func testASCIIHasNoAlternatives() {
        XCTAssertTrue(unicodeNameAlternatives("sample.txt").isEmpty)
    }

    func testMixedSpellingOffersBothFormsInOrder() {
        XCTAssertEqual(unicodeNameAlternatives("\u{304B}\u{3099}\u{00E9}").map { Array($0.utf8) },
                       [Array("\u{304C}\u{00E9}".utf8), Array("\u{304B}\u{3099}e\u{0301}".utf8)])
    }
}
