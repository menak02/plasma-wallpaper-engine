# Remaining V3 — Competitive Analysis + Plan (post c271566, updated 2026-09-09)

Batch 55✅/17⚠️/0❌. Verifier 72/72 baseline match (mae=0.000). C1–C9 from PLAN_REMAINING(_V2) all landed. Landed since: D-Bus path allowlist (S0.5), audio-reactive wiring (S1), FilmGrain GPU post-process (S2, first wired Vulkan compute pass), pause gate (S3), GPU quad composition revival + mesh-deform pipeline (S7, 2026-09-09). **Next up: S4 = T3 mouse click forwarding.**

## Competitive landscape (researched 2026-09-03)

| Project | Stack | Wayland | Interactive | Audio-reactive | Web | Video | Packaging | Distribution |
|---|---|---|---|---|---|---|---|---|
| **Almamu/linux-wallpaperengine** | C++ / OpenGL 3.3 / GLFW+SDL2 / mpv+FFmpeg / CEF | layer-shell + X11 | mouse forwarding, fullscreen pause | PulseAudio 64-band | CEF (full WebGL) | mpv | cmake install | AUR, website (wpengine.alma.mu), GUI by 3rd party (Suhoiyis GTK4) |
| **AzPepoze/linux-wallpaperengine** | **Go** / native reimplementation | yes | mouse events | yes | native WebGL | yes | single binary | GitHub releases, own GUI |
| **waywallen** (ex catsout/wallpaper-engine-kde-plugin) | C++/QML **KDE Plasma plugin** | Plasma-only | Plasma integration | yes | QtWebEngine | QtMultimedia | KPack plugin | AUR (`plasma6-wallpapers-wallpaper-engine-git`), full wallpaper **manager GUI**, workshop browse |
| **Hidamari** | Python / video-only | GNOME/wayland | pause on fullscreen/maximized, volume | no | webpage-as-wallpaper | mpv/yt-dlp (streaming URLs!) | Flatpak on **Flathub**, autostart | Flathub, simple UX |
| **Ours** | C++ / Qt6 / Vulkan compute / D-Bus daemon | layer-shell-style per-output DmaBuf | mouse parallax only | ✅ wired (T1: monitor capture → pulse) | QtWebEngine (loadFinished + JS bridge done) | ffmpeg single-decoder | install() daemon only | **none** — no README, no AUR, no CI |
| **Ours, gaps still open** | | | G1 clicks, G2 fullscreen pause | 64-band parity vs PulseAudio TBD | | | G5 | |

### What we do better (keep and advertise)
1. **Verifier + PNG regression baseline (72 wallpapers, mae gate)** — nobody else has CI-grade per-wallpaper output verification. Unique selling point.
2. Vulkan compute pipeline (descriptor-wired) — GPU effect path none of the others have.
3. Security hardening done (path traversal, LZ4 bounds, D-Bus path allowlist) — Almamu had CVEs here.
4. Single-decoder video fix + multi-output per-screen buffers + live property reload via D-Bus.

### What they have that we lack (the gaps)
- G1 **Mouse forwarding / click interaction** — Almamu + AzPepoze. We only do parallax (`scene_compositor.cpp:106`). Interactive wallpapers are a visible class in the workshop.
- G2 ~~**Pause on fullscreen / maximized window**~~ — ✅ DONE (S3): Hyprland IPC coverage gate (fullscreen-family or ≥90% tiled), per-output, auto-mute, headless decision tests.
- G3 ~~**Autostart after login**~~ — implemented in T5 (systemd user unit) but **intentionally reverted on 2026-09-09**: idle power draw (16-17W, issue #29) made always-on unjustifiable. Session restore remains; users run the daemon by hand until GPU composition makes idle cheap again. Re-add the unit once power is acceptable.
- G4 **Workshop browse → subscribe flow** — waywallen is a full manager. Our plugin QML already queries Steam Web API (`WorkshopView.qml`) but has no "open in Steam"/rescan loop. LibraryScanner already auto-discovers workshop paths, so this is a small UX loop.
- G5 **Packaging: AUR + README + site** — every competitor is on AUR/Flathub with a README. We have zero user-facing surface.
- G6 **Rendering depth** — the GPU scene path is now ALIVE (2026-09-09): plain + mesh-deform layers composite on Vulkan (textured-quad + deform grid pipelines); film grain stays a compute post-process. Remaining CPU-only: puppet-bone scenes (T8), god rays/shine overlays, blur. CPU painter remains the fallback path.
- G7 **Streaming URLs (yt-dlp)** — Hidamari-only niche; optional.

---

## Plan (merged, priority order)

### T0 — Repo hygiene (do first, 30 min) — ✅ DONE (S0)
- Untracked 386 build artifacts (build/, build-asan/) + all of .claude/ (was also a gitlink for the nested worktree repo). Pruned all 3 worktrees + branches (analysis-worktree's 2 unmerged commits verified obsolete: they removed C9 multi-output features; VideoDecoder UAF fix superseded by master's m_dataCopy version).
- Moved 9 root debug scripts (dump_*.py, patch_*.patch) to tools/debug/, deleted batch_verifier.cpp.orig.
- **NEW FINDING:** D-Bus `loadWallpaper` path allowlist was recorded as a decision (memory: decision-dbus-path-validation.md) but never implemented — master only checks `info.exists()` at wallpaper_service.cpp:161. Promoted to its own task: see T1b.

### T1b — D-Bus loadWallpaper path allowlist — P1 security — ✅ DONE (S0.5)
Implemented and verified live over D-Bus: canonicalized-path containment check against trusted library roots (Steam workshop roots seeded from LibraryScanner + custom dirs), static `..`-segment rejection, explicit `registerTrustedDirectory` D-Bus call (dirs only, refuses `/`), `--trusted-directory=` daemon flag, and auto-trust in `addCustomLibraryPath`. Viewer GUI/CLI register the picked file's directory before load so arbitrary user picks keep working. Live probe results: deny `/etc/passwd`, deny `../../` traversal, deny unregistered `/tmp`, allow workshop pkg, register→allow flow OK, file-as-dir refused. Verifier regression 72/72 PASS (earlier 17-fail scare was a stale Aug-16 `build/bin/` binary from pre-repo state — deleted; CMake outputs to `build/daemon/`). Postscript: `build/bin/` leftovers + empty `build/<id>/` dirs fully swept in S2; both build/ dirs had to be reconfigured from scratch because their caches still pointed at the old `plasma-wallpaper-engine-20260826T143005Z-1-001` project path.

### T1 — Wire audio reactive (last C6 half) — P1 — ✅ DONE
Wired end-to-end and verified with a live 440Hz sine: compositor owns `AudioVisualizer`, ticks it per frame (`updateAndRender`), live band energy modulates the Pulse effect (`getBand(0)`, gated on `isLive()` so batch stays deterministic). D-Bus: `startAudioCapture`/`stopAudioCapture`/`getAudioBands`. Auto-start on scenes with pulse effects; capture targets the **default sink monitor only** (never mic) and also hears the wallpaper's own OST. Regression 72/72 PASS.

Key findings while debugging:
- Original `startLiveCapture` never connected `QIODevice::readyRead` → capture produced silence by design.
- Qt Multimedia `QAudioSource` (Qt 6.11 + PipeWire) wedges/kills the event loop after start/stop → replaced with `QProcess` capture: `pw-record --raw --format=s16 ... -` (parec fallback; plain parec also hangs on this setup — pw-record is reliable).
- `QVariant(float)` fails to marshal over D-Bus — the reply is **silently dropped** (method runs, client times out). Cast to `double`.
- `getBand()`/`update()` had latent OOB on a fresh daemon (buffers sized only in never-called `init()`) — bounds-checked.
- gdb attach fails with ptrace_scope=1 (not a child); forced SIGABRT + `coredumpctl info` gives full stacks instead. `qInfo` output doesn't reach redirected stdout — use `std::cout` for daemon probes.

### T2 — FilmGrain GPU pass — P1 — ✅ DONE (S2)
Full post-process path now live: new `film_grain.comp` (interleaved-gradient-noise overlay modeled on WE's filmgrainpower/filmgrainscale user props, deterministic per frame index) embedded as `film_grain_spv` in shaders_spv.h. `VulkanCompute::applyFilmGrain()` does the first real dispatch of the whole compute stack: upload→grain→readback in ONE submit (RAII staging buffer + persistent images, re-created on resize). SceneCompositor scans scenes at load for a visible FilmGrain effect (screen-space, like WE) and applies it post-composite; CPU fallback mirrors the GPU math byte-wise. Parser maps `grainpower`/`power`→strength, `grainscale`→scale. Verified: unit probe (grain applied, bit-identical same-frame, differs across frames), live daemon probe on 3690417937 (`FilmGrain post-process power=0.3 scale=4`, VulkanCompute ACTIVE, 60fps), regression 72/72 PASS (verifier uses its own blit loop, untouched). Discovery: ALL prior RenderGraph/VulkanCompute pass helpers were dead code — grain is the first wired pass. Note: `power` mapping is generic (any effect pass with a `power` constant now lands in strength); fine for grain, revisit if other effects collide.

### T3 — G1 Mouse forwarding — P2 (NEXT after S3)
Beyond parallax: forward cursor position + click events to (a) JS engine (`wallpaperPropertyListener`-style hooks) and (b) web wallpapers via `runJavaScript`. Source: compositor already gets normalized mouse via D-Bus; add click channel from viewer/plugin.

### T4 — G2 Pause on fullscreen/maximized — P2
Extend existing `AudioPlayer` fullscreen detection to a render gate: stop compositor ticking (or drop to 1 fps) when a fullscreen window is focused. Big battery/perf win, cheap to implement.

### T5 — G3 Autostart + install polish — P2 ✅ DONE, then REVERTED (2026-09-09)
systemd user unit (`plasma-wallpaper-engine-daemon.service`) installed to `systemduserunitdir` when found; restore last wallpaper on daemon start — active path persisted to `~/.config/plasma-wallpaper-engine-daemon/last-wallpaper` on every successful load, restored through the ordinary load path at startup so trust rules still apply. Verified A/B: state present → auto-restore; absent → empty start.

**Reverted 2026-09-09:** measured idle draw with the daemon always-on was 16-17W (issue #29). Unit file deleted from the system, install wiring removed from daemon/CMakeLists.txt, unit template deleted from packaging/ (recoverable from git history). Session restore itself is untouched — only autostart wiring is gone.

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

#### T9 progress — GPU quad + deform pipeline ALIVE (2026-09-09)
The full-Vulkan scene composite landed (commit 4d26edc) and — the surprise — it had been **silently dead since d6c8ec3**, every scene falling back to the CPU painter. Root-cause chain, each found by bisection with an offscreen probe (`tests/gpu_deform_probe.cpp`, now permanent regression):
1. `vkCmdBindVertexBuffers` was passed a pointer to ONE VkBuffer for a 2-binding bind → binding 1 (instance stream) read a garbage stack handle → all quads degenerated to points. (Instance-rate fetch is exactly what a standalone minimal repro validated; the app-specific diff was this call.)
2. `CORNER_STRIP` was declared and measured but never memcpy'd into the persistent vertex buffer → binding 0 fed a zero strip even after (1) was fixed.
3. Strip re-upload wasn't re-armed after vertex-buffer realloc (large scenes triggered realloc → strip lost again).
4. GL-style Y-flip (`1.0 - y/h*2`) in quad/particle/deform vertex shaders — Vulkan NDC is Y-down — made GPU output upside-down vs the CPU painter.

New since: `deform_quad.vert` 16×16 grid pipeline for waterwaves/waterripple/wind/foliagesway (48B DeformInstance stream), deform params mirrored from the CPU painter's last-visible-effect semantics, shared 24B push-constant layout (time only consumed by deform), pre-signaled initial fence (first `beginFrame` would have deadlocked on an unsignaled fence), negative-slot texture caching fixed (rejection had forced init()'s white texture to fail → CPU path). Negative-slot + unsignaled-fence fixes were in the interrupted pre-4d26edc slice.

Verified: `gpu_deform_probe` PASS (GPU deform boundary == CPU `MeshDeformer` math, 116.0 == 116.0; mixed plain+deform scene renders), ctest 5/5, verifier 72/72 byte-identical baseline. Still CPU-only: puppet bones (T8), god rays/shine, blur; see G6 above.

#### T9 addendum — layer/dirty-region caching decision (2026-09-08)
How Wallpaper Engine (Windows) actually works, per help.wallpaperengine.io and community findings:
scene wallpapers are GPU-composited every frame — WE redraws the full scene at the configured
FPS and relies on the compositor (DWM) for occlusion/pausing, NOT on caching static layers.
Their perf guidance is frame-rate limits + fullscreen pause, which we already have (pause gate,
per-output buffers). linux-wallpaperengine (Almamu, OpenGL 3.3) likewise re-renders the whole
scene graph each frame on GPU. **Nobody caches static-layer rasters**; the industry answer to
"162 layers is slow" is "compose on GPU so 162 textured quads are trivially cheap."

Decision: skip static-layer raster caching as a primary optimization — invalidation is
error-prone (per-layer effects, audio pulse, parallax offsets, puppet bones, video/web layers
all change per frame) and it only helps CPU-heavy scenes we plan to move to GPU anyway
(T9). The cheap, robust wins already landed instead: persistent staging buffer (no per-frame
allocation), no-op-scale fast path in uploadSceneImage, log-spam removal.

Cache lifetime rules (for the staging buffer, the only persistent render-side cache we keep):
- freed/reallocated only when buffer geometry changes (ensureStagingBuffer size check)
- daemon shutdown destroys it via existing cleanup paths; no cross-session, no on-disk state
- contains only the CURRENT frame's pixels — nothing survives a wallpaper switch

Shaders are compile-once-at-load SPIR-V embedded in the binary (shaders_spv.h); no shader
cache on disk exists or is needed — pipeline objects live for the daemon's lifetime only.

### T10 — 17 ⚠️ wallpapers — P3
All script-dependent (clock/date visibility). Feed user-property values (from the T-landed `setProperty` path) through `JSEngine` re-eval and re-measure how many convert to ✅.

### GitHub integrations (paired with this plan)
1. ✅ CI regression (was: "needs wallpapers as private artifact/cache"): self-contained `fixture_regression` job runs on every push; the full 72-wallpaper library regression runs in `.github/workflows/regression-full.yml` (manual + weekly). The private corpus (scene.pkgs for every baselined ID) is published to the `ci-regression-data-v1` release by `scripts/package_ci_regression.sh` as split 1.9G volumes under the 2 GiB asset cap; CI reassembles + SHA256-verifies, renders offscreen and diffs against the committed baseline. Compare step is the numpy-vectorized path (11.5s for 72 pairs).
3. Nightly ASan job (build-asan config exists).
4. Issue templates: wallpaper-ID + log required. PR template: "55/17/0 unchanged?" checklist.
5. Tag → release.yml: tarball with daemon+viewer+plugin header; AUR PKGBUILD update on tag.
6. GitHub Discussions + wiki compatibility table (waywallen-style), linking verifier results.

## Commit slicing
- S0: hygiene (T0) — ✅ committed
- S0.5: DBus path allowlist (T1b) — ✅ committed
- S1: audio wire (T1) — ✅ committed
- S2: FilmGrain (T2) — ✅ committed (c271566)
- S3: fullscreen pause (T4) — ✅ committed (63a563c, 139306a)
- S4: mouse click forwarding (T3) ← NEXT
- S5: autostart + install (T5) — ✅ committed a8851fa, **install wiring reverted 2026-09-09**
- S6: workshop UX (T6)
- S7: GPU quad + deform composition (T9 slice) — ✅ committed 4d26edc
- S7: README + AUR (T7)
- S8: MDLA playback (T8)

Each commit: `cmake --build build -j`, verifier 72/72 baseline, `ctest` green, batch 55/17/0 unchanged or better.
