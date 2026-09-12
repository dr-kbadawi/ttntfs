// SPDX-License-Identifier: GPL-2.0

import Foundation
import FSKit
import os

let log = Logger(subsystem: "org.ntfsmac.NTFS", category: "extension")

/// Converts a negative-errno return from the core into an FSKit error.
@inline(__always)
func posixError(_ rc: Int32) -> Error {
    fs_errorForPOSIXError(rc < 0 ? -rc : rc)
}

@inline(__always)
func posixError(_ code: Int32, _ message: String) -> Error {
    log.error("\(message, privacy: .public): errno \(code)")
    return fs_errorForPOSIXError(code)
}

/// Installs the core's logger so its messages land in the unified log.
func installCoreLogger() {
    ntfs_set_logger({ level, msg, _ in
        let text = msg.map { String(cString: $0) } ?? ""
        switch level {
        case Int32(NTFS_LOG_ERROR): log.error("core: \(text, privacy: .public)")
        case Int32(NTFS_LOG_WARN):  log.warning("core: \(text, privacy: .public)")
        case Int32(NTFS_LOG_INFO):  log.info("core: \(text, privacy: .public)")
        default:                    log.debug("core: \(text, privacy: .public)")
        }
    }, nil)
}
