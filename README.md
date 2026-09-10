# Plasma Wallpaper Engine

[![Build and Test](https://github.com/menak02/plasma-wallpaper-engine/actions/workflows/build-and-test.yml/badge.svg?branch=feat%2Fhyprland-pause)](https://github.com/menak02/plasma-wallpaper-engine/actions/workflows/build-and-test.yml)

License: GPL-3.0 (see `LICENSE`).

A KDE/Qt6-native Wallpaper Engine for Linux: a headless Vulkan daemon composites Wallpaper Engine scene, video, and web wallpapers and shares frames as **zero-copy DMA-BUFs** over D-Bus to thin clients — a wlroots layer-shell client (Hyprland & friends), a Qt Quick debug viewer, and a KDE Plasma plugin.

## How it works

```
Workshop .pkg ──► daemon (Vulkan scene composite; CPU painter fallback)
                     │  60 fps render loop, pause-gated
                     ▼
              DMA-BUF per output ──► layerclient (Hyprland desktop)
                     │                  viewer (debug preview)
                     └── D-Bus: load/pause/mouse/audio/properties ──► Plasma plugin
```

- **Daemon** — loads `scene.pkg` archives, standalone video wallpapers (`project.json` type `video`), and web wallpapers; renders to per-output DMA-BUFs; runs the audio-reactive pipeline; pauses rendering on covered outputs.
- **layerclient** — displays the daemon's buffer as a real desktop background via `wlr-layer-shell` (optional, needs LayerShellQt).
- **Viewer** — interactive preview with file picker, live parallax, ~20 fps frame pump.
- **Verifier** — headless batch renderer + PNG regression baseline (see Testing).

## Features

- Scene, video (MP4/FFmpeg), and web (QtWebEngine) wallpapers from Steam's Workshop
- Per-output DMA-BUF export; multiple monitors, one buffer each
- Pause gate: per-output coverage detection through Hyprland IPC (fullscreen-family or ≥90% tiled coverage), OST auto-mute
- Audio-reactive effects (PipeWire/Pulse monitor capture → FFT bands → pulse effects)
- Vulkan compute post-processing (film grain today; descriptor stack ready for more)
- Live property manipulation, library scanning across Steam library roots, mouse parallax
- Session restore: last wallpaper comes back on daemon start (auto-disabled at install: no autostart unit — run the daemon by hand for now)
- Security: D-Bus load-path allowlist with canonicalization, one-shot ephemeral preview grants, `--trusted-directory` seeding

## Build

Dependencies (Arch names in parens): CMake, Qt6 Core/Gui/Qml/DBus/Network (`qt6-base`, `qt6-declarative`), Vulkan headers/loader (`vulkan-headers`, `vulkan-icd-loader`), liblz4, FFmpeg (`ffmpeg`), PulseAudio client libs (`libpulse`), optionally Qt6 WebEngine (`qt6-webengine`) and LayerShellQt (`layer-shell-qt`).

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build
```

Artifacts:

| Binary | Location | Purpose |
|---|---|---|
| `plasma-wallpaper-engine-daemon` | libexec | the engine |
| `plasma-wallpaper-engine-viewer` | bin | interactive preview |
| `plasma-wallpaper-engine-layerclient` | bin | Hyprland/wlroots desktop layer (if LayerShellQt found) |
| `plasma-wallpaper-engine-verifier` | libexec | batch verifier |

## Run

```sh
# daemon (no autostart unit for now — idle power draw; run by hand)
./build/daemon/plasma-wallpaper-engine-daemon

# desktop background on Hyprland
plasma-wallpaper-engine-layerclient

# preview / pick wallpapers
plasma-wallpaper-engine-viewer
```

The daemon restores the last active wallpaper on start. Workshop discovery covers `~/.local/share/Steam`, `~/.steam/steam`, Flatpak Steam, and any path in `libraryfolders.vdf`.

## Testing

- **Verifier**: `QT_QPA_PLATFORM=offscreen plasma-wallpaper-engine-verifier` renders every workshop wallpaper to `~/.wallpaper-engine-verifier-output/` with a per-item classification report.
- **Regression**: `ctest` compares output PNGs against `tests/regression/baseline/` (MAE + changed-ratio gates).
- **Smoke**: `scripts/smoke_test_daemon.sh` loads a wallpaper, asserts DMA-BUF export, and checks RSS stability over D-Bus.
- **Pause gate**: headless decision tests (`ctest -R pause_gate`).

## Status & known gaps

- Scene compositing is GPU (textured-quad + mesh-deform pipelines, matching CPU painter math); plain+deform scenes render fully on Vulkan, with the CPU painter as fallback for puppet-bone scenes and when no Vulkan device is present. Vulkan compute covers post-effects (film grain).
- Puppet bone animation renders in rest pose (T8); puppet-bone scenes still composite on CPU.
- Mouse parallax is wired; click forwarding to web/JS wallpapers is not yet (T3).

## Related projects

- [Almamu/linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine) — OpenGL 3.3, layer-shell + X11
- [AzPepoze/linux-wallpaperengine](https://github.com/AzPepoze/linux-wallpaperengine) — Go reimplementation
- [waywallen](https://invent.kde.org/) (ex catsout/wallpaper-engine-kde-plugin) — KDE Plasma plugin
- [Hidamari](https://github.com/jeffshee/gnome-ext-hidamari) — GNOME, video-focused
- [Wallpaper Engine](https://store.steampowered.com/app/431960/) — the original (Windows); you need it installed via Steam for Workshop content

Full competitive analysis and roadmap: `PLAN_REMAINING_V3.md`. Architecture deep-dive: `ARCHITECTURE.md`.
