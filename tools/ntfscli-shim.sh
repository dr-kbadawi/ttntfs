#!/bin/bash
# tools/ntfscli-shim.sh - a fake ntfscli that answers the read-only commands
# run-tests.sh uses (ls -R -l, cat, xattr get/list) with ntfs-3g's own tools.
#
#   tools/run-tests.sh --ntfscli tools/ntfscli-shim.sh ground-truth
#
# must pass: it proves the runner's normalisers, diffs and hashing are sound
# independently of the core under test. Every other command fails with
# ENOSYS, as the real stub build does. Not a test of anything in core/.
set -u
. "$(dirname "$0")/env.sh"

while [ $# -gt 0 ] && [ "${1#-}" != "$1" ]; do
	case "$1" in
	-h|--help) echo "usage: ntfscli-shim.sh COMMAND IMAGE [ARGS]" >&2; echo "core: ntfs-3g shim (tools/ntfscli-shim.sh)" >&2; exit 2 ;;
	esac
	shift
done
[ $# -ge 2 ] || { echo "ntfscli-shim: COMMAND IMAGE required" >&2; exit 2; }
cmd="$1"; img="$2"; shift 2

case "$cmd" in
probe)
	ntfsinfo -m "$img" >/dev/null 2>&1 && { echo "$img: NTFS"; exit 0; }
	echo "ntfscli: probe: $img: ENXIO" >&2; exit 1 ;;
ls)
	path=/; for a in "$@"; do case "$a" in -*) ;; *) path="$a" ;; esac; done
	# ntfsls -R -l -F -> ntfscli's long format:
	#   "fffff nlink size YYYY-MM-DDTHH:MM:SS.fffffffZ name[/]"
	if [ "$path" = / ]; then set -- -R -l -F "$img"; else set -- -R -l -F -p "$path" "$img"; fi
	ntfsls "$@" | awk '
	/^\/.*:$/ || /^$/ { print; next }
	{
		if (!match($0, /^ *[0-9]+ [A-Z][a-z][a-z] [ 0-9][0-9] [0-9][0-9]:[0-9][0-9] +-?[0-9]+ /)) next
		hdr = substr($0, RSTART, RLENGTH); name = substr($0, RLENGTH + 1)
		if (name == "./" || name == "../") next
		size = hdr; sub(/^ */, "", size); sub(/ .*/, "", size)
		printf "%s  1 %12s 0000-00-00T00:00:00.0000000Z %s\n", (name ~ /\/$/) ? "d----" : "-----", size, name
	}'
	exit "${PIPESTATUS[0]}" ;;
cat)
	exec ntfscat "$img" "$1" ;;
xattr)
	op="$1"; path="$2"
	case "$op" in
	get) exec ntfscat -n "$3" "$img" "$path" ;;
	list) ntfsinfo -F "$path" "$img" 2>/dev/null | sed -n "s/^.Attribute name:[[:space:]]*'\(.*\)'$/\1/p"; exit 0 ;;
	esac
	echo "ntfscli: xattr $op: ENOSYS (shim)" >&2; exit 1 ;;
*)
	echo "ntfscli: $cmd: ENOSYS (shim: only ls/cat/xattr get|list/probe are answered by ntfs-3g)" >&2
	exit 1 ;;
esac
