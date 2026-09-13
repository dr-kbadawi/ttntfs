#!/bin/bash
# Build a signed, notarized, stapled disk image for distribution.
#
#   fskit/scripts/package.sh                       # build, sign, notarize, staple
#   fskit/scripts/package.sh --no-notarize         # stop after making the .dmg
#   NOTARY_PROFILE=other fskit/scripts/package.sh  # different keychain profile
#
# Requires (see README "Signing and enabling the extension"):
#   - a "Developer ID Application" identity in the keychain
#   - the two Developer ID provisioning profiles installed
#   - notarization credentials stored:  xcrun notarytool store-credentials ttntfs
#
# Build outside iCloud-synced folders: File Provider stamps bundles with
# extended attributes and codesign then refuses them, which is why the default
# derived-data path is in /tmp rather than next to the project.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
FSKIT=$(dirname "$HERE")
NAME=${NAME:-TT NTFS Native}
DD=${DERIVED_DATA:-/tmp/ttntfs-dd}
OUT=${OUT:-$FSKIT/build/dist}
NOTARY_PROFILE=${NOTARY_PROFILE:-ttntfs}
NOTARIZE=1
[ "${1:-}" = "--no-notarize" ] && NOTARIZE=0

APP="$DD/Build/Products/Release/$NAME.app"
DMG="$OUT/$NAME.dmg"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

echo "==> building Release (Developer ID), from scratch"
# A release artifact is built clean: stale derived data has produced both wrong
# contents and spurious failures -- Xcode caches the entitlements file's state
# and then refuses the build claiming it "was modified during the build" when
# the project has merely been regenerated since.
rm -rf "$DD"
xcodebuild -project "$FSKIT/NTFS.xcodeproj" -scheme NTFS -configuration Release \
           -derivedDataPath "$DD" build >/dev/null
[ -d "$APP" ] || { echo "no app at $APP" >&2; exit 1; }

echo "==> checking the signature"
codesign --verify --deep --strict "$APP"
# Capture first rather than piping into `grep -q`: grep exits on the first match
# and the writer takes SIGPIPE, which `set -o pipefail` reports as failure.
siginfo=$(codesign -dvv "$APP" 2>&1 || true)
case "$siginfo" in
    *"Authority=Developer ID Application"*) ;;
    *) echo "not signed with Developer ID -- check CODE_SIGN_IDENTITY" >&2; exit 1;;
esac
for target in "$APP" "$APP/Contents/Extensions/NTFSExtension.appex"; do
    ents=$(codesign -d --entitlements - --xml "$target" 2>/dev/null || true)
    case "$ents" in
        *get-task-allow*)
            echo "$target carries get-task-allow; notarization would reject it" >&2
            echo "(CODE_SIGN_INJECT_BASE_ENTITLEMENTS must be NO for Release)" >&2
            exit 1;;
    esac
done

if [ "$NOTARIZE" = 1 ]; then
    echo "==> notarizing the app"
    ZIP="$STAGE/app.zip"
    /usr/bin/ditto -c -k --keepParent "$APP" "$ZIP"
    xcrun notarytool submit "$ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$APP"
fi

echo "==> building the disk image"
mkdir -p "$OUT" "$STAGE/vol"
cp -R "$APP" "$STAGE/vol/"
ln -s /Applications "$STAGE/vol/Applications"
rm -f "$DMG"
hdiutil create -volname "$NAME" -srcfolder "$STAGE/vol" -ov -format UDZO "$DMG" >/dev/null

echo "==> signing the disk image"
IDENTITY=$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -1 | awk -F'"' '{print $2}')
codesign --force --timestamp --sign "$IDENTITY" "$DMG"

if [ "$NOTARIZE" = 1 ]; then
    # The image is notarized separately from the app inside it: a download gets
    # the .dmg, and Gatekeeper checks that before anything is copied out of it.
    echo "==> notarizing the disk image"
    xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$DMG"
    echo "==> verdict"
    spctl -a -vv -t open --context context:primary-signature "$DMG" 2>&1 | sed 's/^/    /'
fi

# Xcode's build product registers itself with LaunchServices, which then reports
# the module from a derived-data path. That shadows the installed copy, survives
# uninstalling /Applications, and makes System Settings list the extension twice.
# A packaging run should not leave that behind.
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
if [ -x "$LSREGISTER" ]; then
    pluginkit -r "$APP/Contents/Extensions/NTFSExtension.appex" >/dev/null 2>&1 || true
    "$LSREGISTER" -u "$APP" >/dev/null 2>&1 || true
    echo "==> unregistered the build product from LaunchServices"
fi

echo
echo "$DMG"
ls -lh "$DMG" | awk '{print "  " $5}'
