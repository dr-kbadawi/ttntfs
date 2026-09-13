#!/bin/bash
# Enable (or disable) the TT NTFS Native FSKit module without System Settings.
#
# On macOS 26.3 the File System Extensions toggle in System Settings never
# turns on for a third-party module: the settings host (LoginItems.appex) talks
# to fskitd as an unentitled FSClient and fskitd answers EPERM
# ("Failed to enabled FSExtension: Error Domain=NSPOSIXErrorDomain Code=1").
# The per-user fskit_agent keeps the real enabled list in
#   ~/Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist
# and reads it only at start, so: edit the list, SIGKILL the agent (SIGTERM is
# ignored, launchctl kickstart is refused), launchd respawns it on demand.
#
#   fskit/scripts/enable-module.sh            # enable (also prunes stray copies)
#   fskit/scripts/enable-module.sh --disable  # remove from the list
#
# Order matters after a reinstall: copy the app, LAUNCH IT ONCE (LaunchServices
# re-registers the bundle on first launch), then run this script. The agent
# caches the extension's plugin UUID; every re-registration (install, first
# launch, lsregister -f) changes it and pkd kills any running module instance,
# which unmounts every ttntfs volume. Unregistering the old bundle also drops
# the identifier from the enabled list, so the script must run again anyway.
set -euo pipefail

ID=${NTFS_MODULE_ID:-ch.techtag.ntfs.extension}
PLIST="$HOME/Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist"
MODE=enable
[ "${1:-}" = "--disable" ] && MODE=disable

# Refuse while one of our volumes is mounted: restarting fskit_agent drops it.
# A mounted ttntfs volume looks like plain "ntfs ... fskit" in `mount`, which is
# indistinguishable from Apple's own driver on macOS 26, and the module process
# keeps running indefinitely after an unmount -- so neither is a usable signal.
# What does track a live volume is the block device: the module holds one open
# fd on /dev/diskN while serving, and none when idle.
serving=
for pid in $(pgrep -f 'Contents/Extensions/NTFSExtension' 2>/dev/null); do
    if lsof -p "$pid" 2>/dev/null | grep -qE '/dev/r?disk'; then serving=$pid; break; fi
done
if [ -n "$serving" ]; then
    echo "a ttntfs volume is mounted (module pid $serving holds a block device open);" >&2
    echo "restarting fskit_agent would drop it. Unmount first:" >&2
    mount | grep -E '\(ntfs[,)]' | sed 's/^/  /' >&2
    echo "  diskutil unmount /dev/diskNsM" >&2
    exit 1
fi
[ -f "$PLIST" ] || { echo "no $PLIST (open System Settings > Login Items & Extensions once)" >&2; exit 1; }

# Prune stray registrations. LaunchServices registers every copy of the bundle it
# sees, including Xcode's build output, and System Settings then lists the
# extension twice. Both the enabled list and the pluginkit election name the
# bundle ID, not a path, so with two copies registered it is undefined which one
# fskit_agent launches -- and if that is a build directory that later gets
# deleted, probes fail and the disk falls back to Apple's read-only driver.
APP=${NTFS_APP:-/Applications/TT NTFS Native.app}
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
if [ -x "$LSREGISTER" ]; then
    APP_NAME=$(basename "$APP")
    "$LSREGISTER" -dump 2>/dev/null \
        | sed -n "s|^[[:space:]]*path:[[:space:]]*\(/.*/${APP_NAME}\) (0x[0-9a-f]*)\$|\1|p" \
        | sort -u | while IFS= read -r found; do
        # /private/tmp and /tmp are the same place; compare canonical paths.
        [ "$(cd "$found" 2>/dev/null && pwd -P)" = "$(cd "$APP" 2>/dev/null && pwd -P)" ] && continue
        echo "unregistering stray copy: $found"
        "$LSREGISTER" -u "$found" >/dev/null 2>&1
        pluginkit -r "$found/Contents/Extensions/NTFSExtension.appex" >/dev/null 2>&1
    done
fi

cp "$PLIST" "$PLIST.bak"
python3 - "$PLIST" "$ID" "$MODE" <<'PY'
import plistlib, sys
path, ident, mode = sys.argv[1:4]
with open(path, 'rb') as f:
    modules = plistlib.load(f)
if mode == 'enable' and ident not in modules:
    modules.append(ident)
if mode == 'disable':
    modules = [m for m in modules if m != ident]
with open(path, 'wb') as f:
    plistlib.dump(modules, f)
print('enabled modules:', ', '.join(modules))
PY

PID=$(pgrep -x fskit_agent || true)
if [ -n "$PID" ]; then
    kill -9 "$PID"
    echo "fskit_agent $PID killed; launchd respawns it on the next FSKit request"
fi
sleep 1
# pluginkit election is a separate switch; keep it in step.
if [ "$MODE" = enable ]; then pluginkit -e use -i "$ID"; else pluginkit -e ignore -i "$ID"; fi
STATE=$(pluginkit -m -i "$ID" | cut -c1-1)
echo "pluginkit: ${STATE:-?} $ID   (+ enabled, - disabled)"
echo "verify:  fskit/scripts/mount-test.sh"
