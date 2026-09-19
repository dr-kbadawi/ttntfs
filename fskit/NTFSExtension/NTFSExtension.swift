// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 TechTag GmbH
//
// Extension entry point. ExtensionFoundation requires this to be Swift.

import ExtensionFoundation
import FSKit

@main
struct NTFSExtension: UnaryFileSystemExtension {
    var fileSystem: NTFSFileSystem { NTFSFileSystem.shared }
}
