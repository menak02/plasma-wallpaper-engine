# S3 — Hyprland-aware pause gate + subprocess hygiene

Branch: `feat/hyprland-pause` off `master`

## Goal
Pause wallpaper render + mute OST by default on covered outputs, with instant resume and no stale process trail. First compositor backend is Hyprland; the pause gate itself stays compositor-agnostic behind a small backend interface.

## Commit scope (single bisectable change)
1. ManagedProcess wrapper: start / terminate / forceKill(timeout) semantics.
2. Retfit AudioPlayer OST client and AudioVisualizer capture client through it.
3. Cap and cancel per-second `pactl` audio-activity probe.
4. CompositorBackend interface for per-output "covered" decision.
5. Hyprland IPC backend: fullscreen-family OR tiling coverage >= 90%.
6. Render-gate pause per-output, with pause-all toggle; pause mutes OST by default; resume keeps last frame + scene loaded.
7. Headless decision-test + ctest for pause/no-pause scenarios.

## Detection model
Covered if, on the wallpaper output:
- any window in fullscreen-family state (fullscreen / pseudotile / floating fullscreen), OR
- tiled/pseudotiled window rects cover >= 90% of visible wallpaper area.
Gaps excluded. Movable pseudo-tiled window is a rect that can uncover the desktop.

## Pause semantics
- Pause = tick skip for affected output(s), scene stays loaded, last frame stays available.
- Resume is zero-deferred.
- OST: muted by default while paused; user can override via existing audio settings later.
- Audio settings remain user discretion: mute on fullscreen, mute on other audio, master mute, volume.

## Defaults
- Threshold: 90%
- Pause scope default: per-output
- OST while paused: mute-by-default when paused

## What this commit does NOT do
- No scene teardown on pause.
- No new per-frame scans or subprocesses.
- No cross-compositor backends beyond Hyprland yet.
- No settings UI exposure yet; D-Bus/settings exposure is next step after gate proven.

## Testing
Headless decision-test feeds Hyprland backend fake window/workspace/fullscreen state and asserts pause/no-pause per output under:
- single moveable pseudo-tiled window, desktop visible -> no pause
- multiple pseudo-tiles covering >= 90% -> pause
- any fullscreen-family window -> immediate pause
- workspace switch to bare workspace -> resume
- one output covered, other bare -> per-output pause
- pause-all toggle on -> both pause
- toggle off -> only covered output pauses

## CI / workflow track (parallel, separate commits)
1. .github/workflows/build.yml: matrix build + ctest, headless/offscreen daemon smoke.
2. Headless smoke test: daemon starts, registers buffer, D-Bus service exports, trivial scene loads, render gate toggleable, managed processes clean.
3. Vectorize compare_images.py if still needed before wiring CI.
4. Issue/PR templates + nightly ASan later.

## TODO
- [ ] Create feat/hyprland-pause branch off master
- [ ] Write this plan into repo
- [ ] ManagedProcess wrapper
- [ ] AudioPlayer + AudioVisualizer cleanup
- [ ] CompositorBackend interface + Hyprland backend
- [ ] Pause gate wiring
- [ ] Headless decision-test
- [ ] Build + verifier baseline + ctest green
