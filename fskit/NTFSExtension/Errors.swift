// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
//
// Error mapping. The core returns negative errnos; some of them are Linux-only
// values defined in platform/include/linux/errno.h (EUCLEAN, ENOMEDIUM, ...).
// Every one of them passes through platform_errno_to_host() and then through a
// Darwin range check before it becomes an FSKit NSError, so the kernel never
// sees an errno it does not know.

import Foundation
import FSKit
import os

let log = Logger(subsystem: "ch.techtag.ntfs", category: "extension")

/// Largest errno Darwin defines (`ELAST` in <sys/errno.h>). Anything above it
/// that platform_errno_to_host() did not translate is collapsed to EIO.
private let darwinLastErrno: Int32 = ELAST

/// Negative-errno (or positive) return from the core -> positive Darwin errno.
@inline(__always)
func hostErrno(_ rc: Int32) -> Int32 {
    let e = platform_errno_to_host(rc < 0 ? -rc : rc)
    if e <= 0 { return EIO }
    if e > darwinLastErrno { return EIO }
    return e
}

@inline(__always)
func hostErrno(_ rc: Int) -> Int32 { hostErrno(Int32(clamping: rc)) }

/// Converts a negative-errno return from the core into an FSKit error.
@inline(__always)
func posixError(_ rc: Int32) -> Error {
    fs_errorForPOSIXError(hostErrno(rc))
}

@inline(__always)
func posixError(_ rc: Int) -> Error { posixError(Int32(clamping: rc)) }

func posixError(_ rc: Int32, _ message: String) -> Error {
    let e = hostErrno(rc)
    if e == ENOENT || e == EEXIST || e == ENOTEMPTY || e == ENOATTR || e == ENODATA {
        log.debug("\(message, privacy: .public): errno \(e)")
    } else {
        log.error("\(message, privacy: .public): rc \(rc) -> errno \(e)")
    }
    return fs_errorForPOSIXError(e)
}

func posixError(_ rc: Int, _ message: String) -> Error { posixError(Int32(clamping: rc), message) }

/// A Darwin errno we produce ourselves (never passes through the core).
@inline(__always)
func fsError(_ errno: Int32) -> Error { fs_errorForPOSIXError(errno) }

/// Xattr calls: Linux says ENODATA where Darwin's xattr(2) says ENOATTR.
func xattrError(_ rc: Int32, _ message: String) -> Error {
    let e = hostErrno(rc)
    if e == ENODATA { return fs_errorForPOSIXError(ENOATTR) }
    return posixError(rc, message)
}

/// The FSKit-domain error for a directory cookie that no longer resolves.
func invalidCookieError() -> Error {
    FSError(.invalidDirectoryCookie)
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
