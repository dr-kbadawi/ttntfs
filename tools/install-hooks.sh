#!/bin/bash
# Install a pre-push hook that runs tools/ci.sh --quick.
#
#   tools/install-hooks.sh          install
#   tools/install-hooks.sh --remove uninstall
#
# --quick skips the fixture suite, which takes long enough that a hook running
# it would get bypassed with --no-verify, and a hook people bypass is worse
# than none. The full suite is what CI is for. Skip the hook for one push with
#   git push --no-verify
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
HOOK="$ROOT/.git/hooks/pre-push"

if [ "${1:-}" = "--remove" ]; then
	[ -f "$HOOK" ] && rm -f "$HOOK" && echo "removed $HOOK" || echo "no hook installed"
	exit 0
fi

cat > "$HOOK" <<'INNER'
#!/bin/bash
# Installed by tools/install-hooks.sh. Remove with tools/install-hooks.sh --remove.
exec "$(git rev-parse --show-toplevel)/tools/ci.sh" --quick
INNER
chmod +x "$HOOK"
echo "installed $HOOK (runs tools/ci.sh --quick; bypass with git push --no-verify)"
