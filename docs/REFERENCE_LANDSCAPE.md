# Reference Landscape — how the other implementations work, and where we stand

**Date:** 2026-10-15 · Method: source-level study of Almamu/linux-wallpaperengine (cloned,
code-verified), web research into Wallpaper Engine's community-documented internals and the
open-source alternatives (sources inline). Companion to `PLAN_REMAINING_V3.md` (competitive
section) and `RED_TEAM_REPORT_2026-10-15.md` (security).

## 1. Original Wallpaper Engine (closed source — evidence)

No source release exists; all format knowledge is reverse engineering
([RePKG](https://github.com/notscuffed/repkg): "PKG and TEX formats reverse engineered by me").
Key documented internals:

- **Containers**: `.pkg` (PKGV, LZ4 entries); binary stamps `TEXxxxx`, `MDLxxxx`, `SPVxxxx`.
- **Scene**: `project.json` + `scene.json` (camera, objects, materials with `passes[]`,
  `blending`, `shader`, `constantshadervalues`, `combos`, `usershadervalues`).
- **Render runtime is D3D9-era**: shaders are GLSL-120-ish `.frag/.vert` translated to
  **HLSL Shader Model 3.0** (Steam guide 770802221) — ~50 built-in effects
  (docs.wallpaperengine.io/en/scene/effects/overview.html).
- **Web wallpapers are CEF** (Chromium Embedded Framework), not Electron
  (docs.wallpaperengine.io/en/web/debug/debug.html).
- **Puppets (MDLA)**: SoA vertex streams, dual parent hierarchy (`bind_parent`/`anim_parent`),
  keyframe tracks with blend curves (deepwiki.com/waywallen/open-wallpaper-engine/3.4).
- **Properties**: typed (`color`, `slider`, `bool`, `combo`, `textinput`, `file`, `directory`)
  with `window.wallpaperPropertyListener` callbacks including `applyGeneralProperties`
  (fps limit) and `setPaused`.
- **Performance model**: per-wallpaper pause / stop-to-free-memory / keep-running when
  fullscreen apps run + FPS/quality caps.

## 2. Almamu/linux-wallpaperengine (closest open reimplementation)

C++20/OpenGL 3.3, 4.7k★. Verified from source:

| | Them | Us |
|---|---|---|
| Scene/effect depth | FBO ping-pong effect graph, WPE named RTs, glslang→SPIRV-Cross shader normalization (`Render/Shaders/GLSLContext.cpp`), TEXS animations, puppet meshes | QPainter composite + 4 compute effects; MDLA bones in rest pose |
| Video | libmpv, played from *inside* the .pkg (memory-stream protocol) | libav decode + frame copy; hostile-input hardened |
| Web | CEF 135 + custom scheme serving pkg files | QtWebEngine, sandboxed profile |
| Desktop | X11 root-pixmap **and** wlr-layer-shell | per-output dma-buf over D-Bus (Plasma plugin) |
| Input | mouse pos+clicks to shaders/JS/CEF, Hyprland IPC fallback | parallax only |
| Fullscreen pause | toplevel detectors + pause-aware playlists | none (G2) |
| Hostile files | **unchecked** `new char[uncompressedSize]` (`TextureParser.cpp:73`), unchecked pkg allocs | capped/validated everywhere + fixtures + ASan evidence |
| Testing | Catch2 unit tests, manual webm eyeball, CodeQL, GCC+Clang CI | 72-PNG mae=0 pixel regression, hostile fixtures, vulkan probe |
| Docs/packaging | README + reverse-engineered format docs + AUR | none yet (G5) |

**Adopt from them (ranked):**
1. Shader normalization pipeline (glslang→SPIR-V→SPIRV-Cross) — the compatibility key for T9.
2. WPE effect-graph semantics: named FBOs (`_rt_imageLayerComposite_<id>_a/_b`, `_rt_Bloom`),
   per-pass targets, first-pass blend-mode relocation (`CImage.cpp:769-776`).
3. Pause-on-fullscreen done thoroughly (active-only option, ignore-by-app-id, timer
   compensation) — direct input for G2/S3.
4. PulseAudio capture buffer discipline (hole handling, double-buffer flip).
5. In-tree reverse-engineering docs (`docs/textures/TEXTURE_FORMAT.md`) — our format
   knowledge lives only in parser code.
6. CI hygiene: clang-format gate, GCC+Clang matrix, CodeQL, ccache.

## 3. Alternatives worth stealing from

- **nvpaper** (Rust): Vulkan Video decode → exported NV12 dma-buf → `zwp_linux_dmabuf_v1`
  import, **zero CPU copies**; `wp_viewporter` + `wp_fractional_scale` for per-output
  scaling; single `poll(2)` loop. Known TODOs (buffer pooling, explicit sync via
  `linux-drm-syncobj`) are exactly our H6/H9 class. — github.com/Yurzi/nvpaper
- **catsout/wallpaper-engine-kde-plugin**: window-state model (focused/maximized/fullscreen)
  + battery + TTY/sleep feeding one play/pause decision; auto-pause on maximized/focused;
  per-app pause rules. The state machine S3 should copy. (CaptSilver fork adds a Vulkan
  scene renderer.)
- **Lively (Windows)**: player-per-process isolation + gRPC IPC; WinEventHook fullscreen
  detection with pause/stop/keep-running policies. Pattern for H9 (no blocking work on the
  control loop).
- **swww/awww**: daemon + short-lived CLI clients over unix socket, memfd/wl_shm frames;
  damage-tracking via `wl_surface.damage_buffer` + frame callbacks (Wayland Book).
- **Almamu, mpvpaper**: watch-lists/process-name pause rules (mpvpaper), per-screen roots
  and `--screen-span` (Almamu xdg-output).
- **waywallen**: Rust + C Vulkan bridge + Lua plugins, open-wallpaper-engine plugin
  (CEF OSR → Vulkan blit) — shows the plugin seam we already have.

## 4. Where we stand (honest position)

**We are ahead on:** hostile-input hardening (the only implementation with parser caps,
fixtures, and ASan-proven fixes), automated pixel regression (mae=0 gate over 72 baselines —
unique in the space), zero-copy dma-buf export with per-output targets, D-Bus security
model, deterministic offline rendering (verifier), Plasma-native config UI.

**We are behind on:** effect/shader rendering depth (the #1 visual gap — most workshop
scenes use effects we don't render), mouse/click forwarding (G1), fullscreen pause (G2),
SceneScript API depth (our 17 ⚠️ wallpapers), puppet/MDLA animation (T8), X11, playlists,
docs/README/LICENSE/AUR (G5), CI breadth (one workflow, no sanitizer/format jobs).

**Near-term mapping:** P3 (perf) items map to nvpaper's lessons (persistent staging = their
buffer pool, fused grain = fewer copies, idle gating = damage/frame-callback pacing);
S3/T3 map to the KDE plugin's window-state model and Almamu's detector options;
T9 should follow Almamu's shader normalization rather than hand-mapping effects.

## 5. Tools & dependencies

**Build (ours, all verified present):** cmake ≥3.20, C++20, Qt6 (Core/Gui/Quick/DBus/
Network/Widgets; optional WebEngine/Multimedia), Vulkan, liblz4, FFmpeg libs (pkg-config),
optional ECM/KF6/Plasma for the plugin, Python3 for regression compare.

**Runtime tools:** `pw-record` (audio capture — present), `ffmpeg`/`ffplay` (video thumbs
and BGM — present), `parec`/`pactl` (audio fallback + mute-on-other-audio — **missing on
this host**), `glslangValidator` (shader validation in build — **missing**; optional).

**Corpus tooling (for the workshop run):** `steamcmd` (authenticated:
`+workshop_download_item 431960 <id>`), `DepotDownloader`, and `RePKG`
(`repkg extract -c`, `repkg info -t`) for normalization/metadata — all external, nothing
to build here.

**vs Almamu:** they additionally need OpenGL 3.3/GLEW/GLM/GLFW/SDL2/MPV/PulseAudio/Freetype/
X11/Wayland-EGL and CEF 135 (auto-downloaded at configure). Our dependency footprint is
smaller because QtWebEngine replaces CEF and libav replaces MPV/SDL2 — at the cost of the
scene-effect rendering depth discussed above.
