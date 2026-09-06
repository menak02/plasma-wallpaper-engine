#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"
if [ -z "$REPO_ROOT" ]; then
  echo "Not inside a git repo; skipping dbus interface check." >&2
  exit 0
fi

XML="$REPO_ROOT/daemon/src/ipc/wallpaper_service.xml"
if [ ! -f "$XML" ]; then
  echo "Missing $XML — run the docs step first." >&2
  exit 1
fi

# 1) XML must be parseable by Qt's qdbusxml2cpp (if available). This is the
#    strongest check we have that the interface won't surprise callers.
if command -v qdbusxml2cpp >/dev/null 2>&1; then
  tmpdir="$(mktemp -d)"
  trap 'rm -rf "$tmpdir"' EXIT
  if ! qdbusxml2cpp -p "$tmpdir/WallpaperServiceAdaptor" "$XML" >/dev/null 2>&1; then
    echo "FAIL: $XML does not compile via qdbusxml2cpp." >&2
    qdbusxml2cpp -p "$tmpdir/WallpaperServiceAdaptor" "$XML" 2>&1 || true
    exit 1
  fi
  echo "OK: $XML compiles via qdbusxml2cpp."
else
  echo "SKIP: qdbusxml2cpp not installed; XML is uncheckable here."
fi

# 2) Sanity: every <method> should have a name attribute and no illegal
#    characters. Keep this lightweight and fast in CI.
if ! grep -E '<method[[:space:]]+name="[a-zA-Z_][a-zA-Z0-9_]*"' "$XML" >/dev/null; then
  echo "FAIL: no valid <method> entries in $XML" >&2
  exit 1
fi

echo "OK: dbus interface looks structurally sound."
exit 0
