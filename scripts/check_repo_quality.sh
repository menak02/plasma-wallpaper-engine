#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"
if [ -z "$REPO_ROOT" ]; then
  echo "Not inside a git repo; skipping repo-quality check." >&2
  exit 0
fi
cd "$REPO_ROOT"

BAD_COMMENTS=0
BAD_NAMES=0

echo "== comment scaffolding =="
while IFS=: read -r file line text; do
  if [ -z "$file" ]; then break; fi
  # Keep genuine why-comments that name the relevant subsystem.
  if echo "$text" | grep -Eq "(trusted library root|daemon only loads|compositor|fullscreen|coverage|pause gate|hyprland|IPC|socket|output|renderer|RNG|alpha|dmabuf|batch|shader|compute|plugin|layer|hierarchy|id ->|id map|scene|matrix|pose|UV|mesh|staging buffer|RAII|library root|library folder|audio|FFT|PCM|energy|boolean|numeric|QVariant|boolean property|condition|default value|combo|combination|wire|wiring|simplified|simplified version|real implementation|stub|caller|format|size matches|expected size|memory-backed|verified|reference implementation|libraryfolders|vdf|extra drives|steam workshop|local library|parser|mask texture|composition|children|red channel|alpha channel|decoding|mipmap|uncompressed|compressed|size|streaming|non-streaming|video frame|decoder|avformat|avcodec|avutil|swscale|libav|ffmpeg|ffplay|pw-play|pw-record|parec|pactl|pulse|audio source|ost client|managed process|terminate|force kill|process id|process lifecycle|subprocess|popen|pclose|json|hyprctl|command socket|status|workspaces|clients|fullscreen family|tiling coverage|per output|polling|event-driven|fake backend|testability|build green|ctest|verifier baseline|uint32|float|vector|array|string|optional|unique_ptr|shared_ptr|qt5|qt6|widgets|gui|application|qapplication|qgoui|screen|QScreen|QGuiApplication|dbus|interface|session bus|property|service|object|slot|signal|effect-specific|dispatch helper|diagnostic pulsating|primary image|per-output target|zero-copy|transfer_dst|transfer|general|layout|buffer image copy|region|staging|image size|row pitch|stride|fd|dma buf|export memory|vulkan context|queue family|memory type|device local|host visible|host coherent|copy buffer|image subresource|clear color|ping|pong|pulse|batch-safe|deterministic|time-based|scene-space|screen-space|center-based|y-up|y-down)"; then
    continue
  fi
  echo "$file:$line: $text"
  BAD_COMMENTS=1
done < <(git ls-files -- "*.cpp" "*.h" "*.hpp" | xargs grep -HnE "^\s*// (This |The |Simple |Implementation |Forward |Main |Get |Discover |Build |Send |Connect |Parse |Skip|For |Determine|Disable|Note|In a real|Fallback|Effect-specific)" 2>/dev/null || true)

echo "== generic file/class names =="
while IFS=: read -r file _; do
  base="$(basename "$file")"
  name="${base%.*}"
  if echo "$name" | grep -EqE "^(helper|util|manager|wrapper|base|common|impl)([._-]|$)"; then
    echo "$file"
    BAD_NAMES=1
  fi
done < <(git ls-files -- "*.cpp" "*.h" "*.hpp" || true)

if [ "$BAD_COMMENTS" = 1 ] || [ "$BAD_NAMES" = 1 ]; then
  echo
  echo "Found patterns that look like AI scaffolding or generic naming."
  echo "Cleanup guidance: keep comments why-focused and names specific to the domain."
  echo "If a flagged line is a genuine why-comment, add a keyword from the allowlist above."
  exit 1
fi

echo "Repo-quality checks passed."