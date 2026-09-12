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
#   fskit/scripts/enable-module.sh            # enable
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

if mount | grep -q " ($ID\|ttntfs"; then
    echo "a ttntfs volume is mounted; unmount it first (restarting fskit_agent would drop it)" >&2
    exit 1
fi
[ -f "$PLIST" ] || { echo "no $PLIST (open System Settings > Login Items & Extensions once)" >&2; exit 1; }

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
