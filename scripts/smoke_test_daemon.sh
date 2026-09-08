#!/usr/bin/env bash
# Repeatable daemon smoke test: start (or reuse) the daemon on the session
# bus, load a workshop wallpaper, pull a buffer FD, sample RSS for leak
# drift, and exercise the pause-gate/mouse D-Bus surface. Closes the
# "no repeatable wallpaper load + frame/buffer smoke test" blocker.
#
# Usage: scripts/smoke_test_daemon.sh [scene.pkg|workshop-dir] [timeout]
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"
DAEMON="$REPO_ROOT/build/daemon/plasma-wallpaper-engine-daemon"
DEFAULT_WP="$HOME/.local/share/Steam/steamapps/workshop/content/431960/2076312085/scene.pkg"
WP="${1:-$DEFAULT_WP}"
DBUS_DEST="org.plasmawallpaperengine.Daemon"
DBUS_PATH="/WallpaperEngine"
DBUS_IFACE="org.plasmawallpaperengine.Daemon"

[ -x "$DAEMON" ] || { echo "FATAL: daemon not built at $DAEMON" >&2; exit 1; }
[ -e "$WP" ] || { echo "SKIP: no test wallpaper at $WP" >&2; exit 77; }

gdbus_call() { gdbus call --session --dest "$DBUS_DEST" --object-path "$DBUS_PATH" --method "$DBUS_IFACE.$1" "${@:2}"; }

STARTED_HERE=0
if ! gdbus_call getOutputs >/dev/null 2>&1; then
    echo "== starting daemon for smoke test"
    "$DAEMON" >/tmp/pwe-smoke-daemon.log 2>&1 &
    DAEMON_PID=$!
    STARTED_HERE=1
    for _ in $(seq 1 20); do
        gdbus_call getOutputs >/dev/null 2>&1 && break
        sleep 0.5
    done
fi
trap '[ -n "${DAEMON_PID:-}" ] && kill "$DAEMON_PID" 2>/dev/null || true' EXIT

echo "== load wallpaper: $WP"
DIR="$(dirname "$(readlink -f "$WP")")"
gdbus_call registerTrustedDirectory "$DIR" >/dev/null
LOAD_REPLY="$(gdbus_call loadWallpaper "$(readlink -f "$WP")")"
echo "  load: $LOAD_REPLY"
grep -q "true" <<<"$LOAD_REPLY" || { echo "FAIL: loadWallpaper rejected" >&2; exit 1; }

echo "== buffer export"
OUT="$(gdbus_call getOutputs | grep -oE "'[^']+'" | head -1 | tr -d "'")"
INFO="$(gdbus_call getBufferInfoForOutput "$OUT")"
FD_REPLY="$(gdbus_call getBufferFdForOutput "$OUT")"
grep -q "handle" <<<"$FD_REPLY" || { echo "FAIL: no dmabuf fd exported for $OUT" >&2; exit 1; }
echo "  output=$OUT info=${INFO:0:80}... fd ok"

echo "== pause gate + mouse surface"
gdbus_call setMousePosition 0.5 0.5 >/dev/null
gdbus_call getPauseState >/dev/null
echo "  pause state + setMousePosition callable"

echo "== RSS drift sample (5s apart, leak check)"
read -r RSS1 _ < <(pgrep -f plasma-wallpaper-engine-daemon | head -1 | xargs -r ps -o rss= -p)
sleep 5
read -r RSS2 _ < <(pgrep -f plasma-wallpaper-engine-daemon | head -1 | xargs -r ps -o rss= -p)
GROWTH=$(( (RSS2 - RSS1) * 100 / (RSS1 > 0 ? RSS1 : 1) ))
echo "  rss: ${RSS1}kB -> ${RSS2}kB (${GROWTH}%)"
[ "$GROWTH" -le 20 ] || { echo "FAIL: RSS grew ${GROWTH}% in 5s" >&2; exit 1; }

echo "PASS: load, dmabuf export, pause surface, and memory stability verified"
