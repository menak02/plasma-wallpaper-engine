# Remaining V3 — Competitive Analysis + Plan (post 9dbb7fa)

Batch 55✅/17⚠️/0❌. Verifier 72/72 baseline match (mae=0.000). C1–C9 from PLAN_REMAINING(_V2) all landed.

## Competitive landscape (researched 2026-09-03)

| Project | Stack | Wayland | Interactive | Audio-reactive | Web | Video | Packaging | Distribution |
|---|---|---|---|---|---|---|---|---|
| **Almamu/linux-wallpaperengine** | C++ / OpenGL 3.3 / GLFW+SDL2 / mpv+FFmpeg / CEF | layer-shell + X11 | mouse forwarding, fullscreen pause | PulseAudio 64-band | CEF (full WebGL) | mpv | cmake install | AUR, website (wpengine.alma.mu), GUI by 3rd party (Suhoiyis GTK4) |
| **AzPepoze/linux-wallpaperengine** | **Go** / native reimplementation | yes | mouse events | yes | native WebGL | yes | single binary | GitHub releases, own GUI |
| **waywallen** (ex catsout/wallpaper-engine-kde-plugin) | C++/QML **KDE Plasma plugin** | Plasma-only | Plasma integration | yes | QtWebEngine | QtMultimedia | KPack plugin | AUR (`plasma6-wallpapers-wallpaper-engine-git`), full wallpaper **manager GUI**, workshop browse |
| **Hidamari** | Python / video-only | GNOME/wayland | pause on fullscreen/maximized, volume | no | webpage-as-wallpaper | mpv/yt-dlp (streaming URLs!) | Flatpak on **Flathub**, autostart | Flathub, simple UX |
| **Ours** | C++ / Qt6 / Vulkan compute / D-Bus daemon | layer-shell-style per-output DmaBuf | mouse parallax only | engine exists, **not wired** | QtWebEngine (loadFinished + JS bridge done) | ffmpeg single-decoder | install() daemon only | **none** — no README, no AUR, no CI |

### What we do better (keep and advertise)
1. **Verifier + PNG regression baseline (72 wallpapers, mae gate)** — nobody else has CI-grade per-wallpaper output verification. Unique selling point.
2. Vulkan compute pipeline (descriptor-wired) — GPU effect path none of the others have.
3. Security hardening done (path traversal, LZ4 bounds, D-Bus path allowlist) — Almamu had CVEs here.
4. Single-decoder video fix + multi-output per-screen buffers + live property reload via D-Bus.

### What they have that we lack (the gaps)
- G1 **Mouse forwarding / click interaction** — Almamu + AzPepoze. We only do parallax (`scene_compositor.cpp:106`). Interactive wallpapers are a visible class in the workshop.
- G2 **Pause on fullscreen / maximized window** — Almamu + Hidamari. We have `setMuteOnFullscreen` for audio only; render still burns GPU. Perf + battery differentiator.
- G3 **Autostart after login** — Hidamari's basic UX. Our daemon needs a systemd user unit / autostart .desktop.
- G4 **Workshop browse → subscribe flow** — waywallen is a full manager. Our plugin QML already queries Steam Web API (`WorkshopView.qml`) but has no "open in Steam"/rescan loop. LibraryScanner already auto-discovers workshop paths, so this is a small UX loop.
- G5 **Packaging: AUR + README + site** — every competitor is on AUR/Flathub with a README. We have zero user-facing surface.
- G6 **Rendering depth** — composition is still QPainter raster with Vulkan compute for effects; Almamu/AzPepoze render scene natively on GPU. Medium-term: QRhi or full-Vulkan swapchain path.
- G7 **Streaming URLs (yt-dlp)** — Hidamari-only niche; optional.

---

## Plan (merged, priority order)

### T0 — Repo hygiene (do first, 30 min) — ✅ DONE (S0)
- Untracked 386 build artifacts (build/, build-asan/) + all of .claude/ (was also a gitlink for the nested worktree repo). Pruned all 3 worktrees + branches (analysis-worktree's 2 unmerged commits verified obsolete: they removed C9 multi-output features; VideoDecoder UAF fix superseded by master's m_dataCopy version).
- Moved 9 root debug scripts (dump_*.py, patch_*.patch) to tools/debug/, deleted batch_verifier.cpp.orig.
- **NEW FINDING:** D-Bus `loadWallpaper` path allowlist was recorded as a decision (memory: decision-dbus-path-validation.md) but never implemented — master only checks `info.exists()` at wallpaper_service.cpp:161. Promoted to its own task: see T1b.

### T1b — D-Bus loadWallpaper path allowlist — P1 security — ✅ DONE (S0.5)
Implemented and verified live over D-Bus: canonicalized-path containment check against trusted library roots (Steam workshop roots seeded from LibraryScanner + custom dirs), static `..`-segment rejection, explicit `registerTrustedDirectory` D-Bus call (dirs only, refuses `/`), `--trusted-directory=` daemon flag, and auto-trust in `addCustomLibraryPath`. Viewer GUI/CLI register the picked file's directory before load so arbitrary user picks keep working. Live probe results: deny `/etc/passwd`, deny `../../` traversal, deny unregistered `/tmp`, allow workshop pkg, register→allow flow OK, file-as-dir refused. Verifier regression 72/72 PASS (earlier 17-fail scare was a stale Aug-16 `build/bin/` binary from pre-repo state — deleted; CMake outputs to `build/daemon/`).

### T1 — Wire audio reactive (last C6 half) — P1 — ✅ DONE
Wired end-to-end and verified with a live 440Hz sine: compositor owns `AudioVisualizer`, ticks it per frame (`updateAndRender`), live band energy modulates the Pulse effect (`getBand(0)`, gated on `isLive()` so batch stays deterministic). D-Bus: `startAudioCapture`/`stopAudioCapture`/`getAudioBands`. Auto-start on scenes with pulse effects; capture targets the **default sink monitor only** (never mic) and also hears the wallpaper's own OST. Regression 72/72 PASS.

Key findings while debugging:
- Original `startLiveCapture` never connected `QIODevice::readyRead` → capture produced silence by design.
- Qt Multimedia `QAudioSource` (Qt 6.11 + PipeWire) wedges/kills the event loop after start/stop → replaced with `QProcess` capture: `pw-record --raw --format=s16 ... -` (parec fallback; plain parec also hangs on this setup — pw-record is reliable).
- `QVariant(float)` fails to marshal over D-Bus — the reply is **silently dropped** (method runs, client times out). Cast to `double`.
- `getBand()`/`update()` had latent OOB on a fresh daemon (buffers sized only in never-called `init()`) — bounds-checked.
- gdb attach fails with ptrace_scope=1 (not a child); forced SIGABRT + `coredumpctl info` gives full stacks instead. `qInfo` output doesn't reach redirected stdout — use `std::cout` for daemon probes.

### T2 — FilmGrain GPU pass — P1
Parsed (`scene_parser.cpp:548`) but ignored (`scene_compositor.cpp:349` empty case). RenderGraph descriptor plumbing is fixed, so add grain shader (noise overlay, seed+intensity push constants) to the RenderGraph path. Same for any remaining `ColorAdjust`/`Tint` cases that are free wins.

### T3 — G1 Mouse forwarding — P2
Beyond parallax: forward cursor position + click events to (a) JS engine (`wallpaperPropertyListener`-style hooks) and (b) web wallpapers via `runJavaScript`. Source: compositor already gets normalized mouse via D-Bus; add click channel from viewer/plugin.

### T4 — G2 Pause on fullscreen/maximized — P2
Extend existing `AudioPlayer` fullscreen detection to a render gate: stop compositor ticking (or drop to 1 fps) when a fullscreen window is focused. Big battery/perf win, cheap to implement.

### T5 — G3 Autostart + install polish — P2
systemd user unit (`plasma-wallpaper-engine-daemon.service`) + autostart .desktop; `install()` rules for verifier + viewer; restore last wallpaper on daemon start (persist D-Bus state).

### T6 — G4 Workshop UX loop — P2
In plugin QML: "Open in Steam" deep link per search result (`steam://url/CommunityFilePage/<fileid>`) + auto rescan via `LibraryScanner` after return. No download code needed.

### T7 — G5 Docs + packaging — P2 (highest visibility-per-effort)
- README.md: what it is, build steps, compatibility statement, screenshot of verifier output.
- AUR PKGBUILD (`plasma-wallpaper-engine-git`) — both major competitors live on AUR.
- Later: Flathub, GitHub Pages site.

### T8 — MDLA puppet animation playback — P3
Bones render in rest pose (`mesh_renderer.h:35` `animatedPos/animatedAngle` never driven). Implement MDLA timeline playback driving bone state.

### T9 — G6 GPU composition path — P3 (medium-term)
Evaluate QRhi vs pure-Vulkan swapchain to move composition off QPainter raster. Big change; schedule after T1–T7 stabilize.

### T10 — 17 ⚠️ wallpapers — P3
All script-dependent (clock/date visibility). Feed user-property values (from the T-landed `setProperty` path) through `JSEngine` re-eval and re-measure how many convert to ✅.

### GitHub integrations (paired with this plan)
1. `.github/workflows/build.yml` — matrix build + ctest (regression job needs wallpapers as private artifact/cache).
2. `regression.yml` — verifier → compare_images.py → PR comment with diff PNGs (our unique gate).
3. Nightly ASan job (build-asan config exists).
4. Issue templates: wallpaper-ID + log required. PR template: "55/17/0 unchanged?" checklist.
5. Tag → release.yml: tarball with daemon+viewer+plugin header; AUR PKGBUILD update on tag.
6. GitHub Discussions + wiki compatibility table (waywallen-style), linking verifier results.

## Commit slicing
- S0: hygiene (T0) — ✅ committed
- S0.5: DBus path allowlist (T1b) — ✅ committed
- S1: audio wire (T1) — ✅ committed
- S2: FilmGrain (T2)
- S3: fullscreen pause (T4)
- S4: mouse click forwarding (T3)
- S5: autostart + install (T5)
- S6: workshop UX (T6)
- S7: README + AUR (T7)
- S8: MDLA playback (T8)

Each commit: `cmake --build build -j`, verifier 72/72 baseline, `ctest` green, batch 55/17/0 unchanged or better.
