#!/bin/bash
# Print build-setting assignments for xcodebuild, derived from VERSION and git.
#
#   MARKETING_VERSION      what a user sees: VERSION file (e.g. 0.4.0)
#   CURRENT_PROJECT_VERSION monotonic build number: commits on this branch
#   TT_GIT_COMMIT          short hash, "-dirty" if the tree has changes
#
# Both Info.plists reference these, so every build -- dev or packaged -- carries
# the commit it was made from. A build from an uncommitted tree says so.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
V="$(tr -d ' \n' < "$ROOT/VERSION")"
N="$(git -C "$ROOT" rev-list --count HEAD 2>/dev/null || echo 0)"
C="$(git -C "$ROOT" describe --always --dirty --abbrev=9 2>/dev/null || echo unknown)"
echo "MARKETING_VERSION=$V"
echo "CURRENT_PROJECT_VERSION=$N"
echo "TT_GIT_COMMIT=$C"
