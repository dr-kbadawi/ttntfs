#!/bin/bash
# Remove TT NTFS Native and every trace it leaves behind.
#
#   fskit/scripts/uninstall.sh            # ask before deleting
#   fskit/scripts/uninstall.sh --yes      # no prompt
#
# Order matters: volumes have to go before the module is disabled, and the
# module before the bundle is unregistered, or fskitd keeps a half-dead
# instance around and the next install inherits the confusion.
#
# DO THIS FIRST, while the app still exists: open it, go to Settings and switch
# OFF "Open at login". That calls SMAppService.unregister(), which is the only
# supported way to remove the Login Item record -- macOS keeps those in
# Background Task Management, which has no per-item command line, and only the
# app itself can unregister. Delete the bundle first and the record is orphaned:
# it lingers under System Settings > General > Login Items & Extensions >
# Open at Login until you remove it there by hand with the "-" button.
set -uo pipefail

APP=${NTFS_APP:-/Applications/TT NTFS Native.app}
ID=${NTFS_MODULE_ID:-ch.techtag.ntfs.extension}
APP_ID=${NTFS_APP_ID:-ch.techtag.ntfs}
GROUP=${NTFS_GROUP:-group.ch.techtag.ntfs}
FSKIT_LIST="$HOME/Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist"
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
NAME_HINT=$(basename "$APP" .app)

TRACES=(
    "$APP"
    "$HOME/Library/Group Containers/$GROUP"
    "$HOME/Library/Containers/$APP_ID"
    "$HOME/Library/Preferences/$APP_ID.plist"
    "$HOME/Library/Preferences/$GROUP.plist"
    "$HOME/Library/Caches/$APP_ID"
    "$HOME/Library/Saved Application State/$APP_ID.savedState"
    "$HOME/Library/HTTPStorages/$APP_ID"
)

if [ "${1:-}" != "--yes" ]; then
    if [ -d "$APP" ]; then
        echo "Before continuing: open the app, and in Settings switch OFF"
        echo "\"Open at login\". Nothing here can remove that record once the"
        echo "bundle is gone -- see the comment at the top of this script."
        echo
    fi
    echo "This will remove:"
    for t in "${TRACES[@]}"; do [ -e "$t" ] && echo "  $t"; done
    echo "  the $ID entry in FSKit's enabled-module list"
    echo "  its LaunchServices and PlugInKit registrations"
    read -r -p "Continue? [y/N] " reply
    case "$reply" in [yY]*) ;; *) echo "aborted"; exit 1;; esac
fi

# 1. Unmount anything our module is serving. It holds one fd on the block
#    device while a volume is live; idle instances hold none.
for pid in $(pgrep -f 'Contents/Extensions/NTFSExtension' 2>/dev/null); do
    lsof -p "$pid" 2>/dev/null | grep -qE '/dev/r?disk' || continue
    echo "the module is serving a mounted volume; unmount it first:"
    mount | grep -E '\(ntfs[,)]' | sed 's/^/  /'
    echo "  diskutil unmount /dev/diskNsM"
    exit 1
done

# 2. Drop it from fskit_agent's list and restart the agent so it forgets.
if [ -f "$FSKIT_LIST" ]; then
    python3 - "$FSKIT_LIST" "$ID" <<'PY'
import plistlib, sys
path, ident = sys.argv[1:3]
with open(path, 'rb') as f:
    modules = plistlib.load(f)
if ident in modules:
    modules = [m for m in modules if m != ident]
    with open(path, 'wb') as f:
        plistlib.dump(modules, f)
    print(f"removed {ident} from the FSKit enabled list")
PY
fi
# (the agent is restarted after deregistration below, so it forgets the module)

# 3. Quit the app before pulling the bundle out from under it.
pkill -f "$(basename "$APP")/Contents/MacOS" 2>/dev/null && echo "quit the app"
sleep 1

# 4. Deregister. Do this while the bundle still exists, or the databases keep
#    a record pointing at a path that is gone.
pluginkit -e ignore -i "$ID" >/dev/null 2>&1
# Every registered copy, not just the installed one: Xcode registers whatever it
# builds, so a build product left in a derived-data directory keeps the module
# alive in LaunchServices and FSKit long after /Applications is emptied.
if [ -x "$LSREGISTER" ]; then
    APP_NAME=$(basename "$APP")
    "$LSREGISTER" -dump 2>/dev/null \
        | sed -n "s|^[[:space:]]*path:[[:space:]]*\(/.*/${APP_NAME}\) (0x[0-9a-f]*)\$|\1|p" \
        | sort -u | while IFS= read -r found; do
        pluginkit -r "$found/Contents/Extensions/NTFSExtension.appex" >/dev/null 2>&1
        "$LSREGISTER" -u "$found" >/dev/null 2>&1
        echo "unregistered $found"
    done
fi

# 5. Restart the agent so FSKit drops its cached view of the module. Doing this
#    only now means it re-reads both the list and the registrations.
pid=$(pgrep -x fskit_agent || true)
[ -n "$pid" ] && kill -9 "$pid" && echo "restarted fskit_agent"

# 6. Delete.
for t in "${TRACES[@]}"; do
    [ -e "$t" ] || continue
    rm -rf "$t" && echo "removed $t"
done

echo
echo "done."
echo
echo "If \"$NAME_HINT\" still appears under System Settings > General >"
echo "Login Items & Extensions > Open at Login, select it and press \"-\"."
echo "That record can only be removed by the app itself (via its \"Open at"
echo "login\" switch) or by hand here; there is no command for it."
