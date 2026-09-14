#!/bin/bash
# Image a capture stick and analyse its journal, in one step.
#
#   tools/phase4-capture/capture.sh <diskNsM> <name>
#   e.g. tools/phase4-capture/capture.sh disk4s1 A-cap1-dirty
#
# A dirty capture MUST be imaged before Windows sees the stick again: Windows
# replays and cleans the log at mount, and that includes autoplay after a
# reboot. Pull the stick, come here, run this, and only then take it back.
#
# Needs sudo once, for the raw device -- a partition is not readable as the
# user. Everything after that runs unprivileged.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd)
dev="${1:?usage: capture.sh <diskNsM> <name>}"; name="${2:?usage: capture.sh <diskNsM> <name>}"
out="$root/tools/phase4-capture/captures"; mkdir -p "$out"

command -v "$root/tools/.local/bin/ntfscat" >/dev/null || { echo "build ntfsprogs first: tools/build-ntfsprogs.sh" >&2; exit 2; }
[ -x "$root/build-logfile/ntfslog" ] || cmake -S "$root/core/logfile" -B "$root/build-logfile" >/dev/null 2>&1 && cmake --build "$root/build-logfile" >/dev/null 2>&1

echo "==> unmounting $dev (macOS auto-mounts NTFS; imaging a mounted volume races the driver)"
diskutil unmountDisk "/dev/${dev%%s[0-9]*}" >/dev/null 2>&1 || diskutil unmount "/dev/$dev" >/dev/null 2>&1 || true

img="$out/$name.img"
echo "==> imaging /dev/r$dev -> $img   (needs sudo)"
sudo dd if="/dev/r$dev" of="$img" bs=1m status=progress || { echo "dd failed" >&2; exit 1; }
sudo chown "$(id -u):$(id -g)" "$img"
ls -lh "$img" | awk '{print "    "$5}'

echo "==> extracting the raw \$LogFile"
"$root/tools/.local/bin/ntfscat" -i 2 -a 0x80 "$img" > "$out/$name.logfile" 2>/dev/null \
  && ls -lh "$out/$name.logfile" | awk '{print "    "$5}' || echo "    (ntfscat could not read it; the image itself is still usable)"

echo "==> our analysis"
"$root/build-logfile/ntfslog" -v "$img" | tee "$out/$name.ntfslog.txt" | head -20

echo "==> ntfsprogs' interpretation (v1.1 only; it may reject a 2.0 log)"
"$root/tools/.local/bin/ntfsdump_logfile" "$img" > "$out/$name.ntfsprogs.txt" 2>&1 \
  && echo "    $(wc -l < "$out/$name.ntfsprogs.txt") lines" || echo "    (rejected, which is itself information)"

echo
echo "saved under tools/phase4-capture/captures/$name.*"
echo "NOTE: never run 'ntfslog --apply' on the original image. Copy it first."
