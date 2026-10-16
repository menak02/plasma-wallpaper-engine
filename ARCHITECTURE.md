# Plasma Wallpaper Engine — Architecture

This is a **KDE Plasma wallpaper engine** that replicates Wallpaper Engine functionality. It has three main components: a **headless daemon** (core engine), a **KDE Plasma plugin** (desktop integration), and a **viewer** (debug/preview tool). They communicate via **D-Bus** and share rendered frames through **Vulkan DmaBuf buffers**.

---

## 1. High-Level Process Model

```
┌─────────────────────────────────────────────────────┐
│                    KDE Plasma Session                │
│                                                     │
│  ┌─────────────┐    D-Bus     ┌──────────────────┐  │
│  │ Plasma      │◄────────────►│  daemon           │  │
│  │ Plugin      │   (method    │  (headless)       │  │
│  │ (QML UI)    │    calls)    │                   │  │
│  └─────────────┘              │  ┌──────────────┐ │  │
│        ▲                      │  │ Vulkan       │ │  │
│        │                      │  │ Context      │ │  │
│        │                      │  │              │ │  │
│        │                      │  │ - DmaBuf     │ │  │
│        │                      │  │ - Render     │ │  │
│        │                      │  │   pass       │ │  │
│        │                      │  └──────────────┘ │  │
│        │                      │                   │  │
│        │                      │  ┌──────────────┐ │  │
│        │                      │  │ Scene        │ │  │
│        │                      │  │ Compositor   │ │  │
│        │                      │  │              │ │  │
│        │                      │  │ - Parse .pkg │ │  │
│        │                      │  │ - Layers     │ │  │
│        │                      │  │ - Particles  │ │  │
│        │                      │  │ - Effects    │ │  │
│        │                      │  └──────────────┘ │  │
│        │                      │                   │  │
│        │                      │  ┌──────────────┐ │  │
│        │                      │  │ IPC Service  │ │  │
│        │                      │  │ (D-Bus)      │ │  │
│        │                      │  └──────────────┘ │  │
│        │                      │                   │  │
│        │          DmaBuf FD   │                   │  │
│        │          (mmap)      │                   │  │
│        │                      │                   │  │
│  ┌─────────────┐              │                   │  │
│  │ Viewer      │◄─────────────┘                   │  │
│  │ (debug UI)  │                                  │  │
│  └─────────────┘                                  │  │
│                                                     │
└─────────────────────────────────────────────────────┘
```

**Key insight:** The daemon is **headless** — it does all rendering offscreen and exports frames as DMA-BUF file descriptors. The Plasma plugin and viewer are thin clients that just display those exported buffers.

---

## 2. Entry Point: `daemon/src/main.cpp`

The daemon bootstraps everything in this order:

1. **QGuiApplication** — needed for `QScreen` enumeration (multi-output detection), even though rendering is headless.
2. **VulkanContext init** — creates Vulkan instance/device.
3. **PluginRegistry** — loads builtin wallpaper plugins.
4. **Default DmaBuf allocation** — creates initial 1920x1080 buffer.
5. **Per-output buffer registration** — loops over `QGuiApplication::screens()` and calls `vulkanCtx.setResolutionForOutput()` for each monitor.
6. **Trusted directories** — parses `--trusted-directory=` CLI args to extend the wallpaper load-path allowlist.
7. **D-Bus registration** — registers `org.antigravity.WallpaperEngine` service with generated adaptor.
8. **60 FPS render loop** — `QTimer` at 16ms ticks:
   - `service.updateAndRender(dt, time)` — updates scene + renders to CPU canvas
   - `vulkanCtx.renderFrame(time)` — uploads canvas to GPU, renders to DmaBuf
   - `service.requestFrame()` — emits D-Bus signal so Plasma/viewer redraws

**Shutdown:** SIGINT/SIGTERM set `g_quitRequested`, which calls `QCoreApplication::quit()` on next timer tick.

---

## 3. IPC Layer: `daemon/src/ipc/wallpaper_service.*`

This is the **D-Bus service interface** (`org.antigravity.WallpaperEngine`). It's the only external-facing API.

**Key responsibilities:**
- **Wallpaper loading** — `loadWallpaper(path)` validates path against trusted directories, opens `.pkg` archive, delegates to `SceneCompositor`
- **Buffer export** — `getBufferFdForOutput(outputName)` returns a `QDBusUnixFileDescriptor` (DMA-BUF FD) for zero-copy sharing
- **Multi-output** — `setResolutionForOutput()`, `getOutputs()`, `getBufferInfoForOutput()`
- **Audio** — `startAudioCapture()`, `getAudioBands()`, `setAudioVolume()`, mute controls
- **Pause gating** — `setPauseEnabled()`, `setPauseAllOutputs()`, `setPauseCoverageThreshold()`, `isOutputCovered()`
- **Library management** — `scanLibrary()`, `getLibrary()`, `addCustomLibraryPath()`
- **Trust management** — `registerTrustedDirectory()` — security gate so D-Bus callers can't load arbitrary paths
- **Live properties** — `getWallpaperProperties()`, `setProperty()`, `getProperty()` for runtime tweaking

**Internal state:**
- `VulkanContext*` — GPU render target
- `PkgReader` — opens wallpaper archives
- `LibraryScanner` — scans Steam Workshop dirs
- `SceneCompositor` — renders frames
- `AudioPlayer` — plays wallpaper audio
- `CompositorBackend` — abstract interface for pause detection

**Pause gate atomics:** `m_pauseEnabled`, `m_pauseAllOutputs`, `m_pauseCoverageThreshold` (default 0.90), `m_pauseMuteAudio` — all `std::atomic<bool>` for thread safety.

---

## 4. Scene Compositor: `daemon/src/scene/scene_compositor.*`

This is the **heart of the engine**. It loads Wallpaper Engine `.pkg` files and composites layers every frame. Two composite backends exist: a **GPU path** (`GpuQuadCompositor`, textured-quad + mesh-deform pipelines, output to the DMA-BUF directly) and a **CPU painter path** (QPainter raster, below) used as fallback for puppet-bone scenes and when no Vulkan device is present. Both implement the same math; the GPU deform pipeline is regression-checked against the CPU painter by `tests/gpu_deform_probe.cpp`.

### 4a. Scene Loading

`loadScene(pkgReader)`:
1. Calls `SceneParser::parseScene()` to read `scene.json`, `project.json`, materials, textures, particles, effects from the `.pkg`
2. Sets particle emitters from parsed scene
3. Clears render graph
4. Scans for post-effects (currently only **film grain**)
5. Stores `SceneDescription` with layers, emitters, metadata

`loadWeb(html)`:
1. Creates `WebWallpaper` (QtWebEngine offscreen)
2. Grabs one QImage frame
3. Creates a single fullscreen layer
4. Each frame, re-grabs from WebWallpaper for live updates

### 4b. Per-Frame Rendering: `updateAndRender(dt, time)`

**Step 1 — Audio tick:**
- `m_audioVisualizer.update()` samples current audio bands

**Step 2 — Web wallpaper refresh:**
- If web mode, grab fresh QImage from QtWebEngine and replace layer 0

**Step 3 — Video decoding:**
- Throttled to ~30fps using accumulator
- Extracts embedded MP4 from layer's `videoData` to temp file
- Uses `VideoDecoder` to decode next frame
- Loops on EOF

**Step 4 — Clear canvas:**
- `m_canvas.fill(m_scene.clearColor)` — RGBA8888 QImage

**Step 5 — Layer compositing:**
- Iterates `m_scene.layers` in topological order (parents before children)
- For each visible layer with non-null image:
  - Computes **effective opacity** (interactive layers fade based on mouse distance)
  - Applies **blend mode** via QPainter composition modes
  - Resolves **parent transform chain** via `resolveParentTransform()` — walks up parent pointers, accumulates origin/scale/rotation
  - Applies **parallax offset** from mouse position
  - Evaluates **effect chains**:
    - **Breath** — sine-based scale + vertical offset
    - **Pulse** — audio-reactive or time-based sine scale
    - **Wind/WaterWaves/WaterRipple/FoliageSway** — sets mesh deformation flags (GPU path: routes the layer to the deform pipeline)
    - **Shake** — sine-based XY jitter
    - **Blur/FilmGrain/ColorAdjust/Tint** — handled later
  - **Mesh deformation** (if wind/water effects or puppet bones):
    - CPU painter: generates an 8×8 vertex grid; GPU deform pipeline: 16×16 grid (same `MeshDeformer` math, finer tessellation)
    - Deforms vertices based on time/speed/strength/direction
    - Renders deformed mesh with texture (CPU) or textured grid quads (GPU `deform_quad.vert`)
  - **Puppet bones** (if `layer.bones` present):
    - Converts scene bones to render bones
    - CPU skinning stub (identity transform currently)
  - **Normal sprite** — `painter.drawImage()` centered at transformed origin
  - **God rays/Shine** — radial gradient overlay with additive blending

**Step 6 — Particle rendering:**
- `m_particleEngine.update(dt, width, height)` — physics simulation
- `m_particleEngine.render(painter, width, height)` — draws particles

**Step 7 — Film grain post-process:**
- If scene has film grain effect:
  - **GPU path:** `m_compute.applyFilmGrain()` via Vulkan compute shader
  - **CPU fallback:** per-pixel interleaved gradient noise added to RGB channels

**Step 8 — Upload to Vulkan:**
- `m_vulkanCtx->uploadSceneImage()` copies QImage pixels to Vulkan staging buffer

### 4c. Parent Transform Resolution

`resolveParentTransform()`:
1. Builds chain from layer up to root
2. Accumulates from root downward:
   - Origin rotated by accumulated angle, scaled by accumulated scale
   - Scale multiplied
   - Angle added
3. Returns final screen-space origin, cumulative scale, cumulative rotation

---

## 5. Vulkan Rendering: `daemon/src/vulkan/vulkan_context.*`

### 5a. Initialization (`init()`)

1. **Instance** — `initInstance()` creates Vulkan instance with `VK_EXT_external_memory_dma_buf` extension
2. **Physical device** — `selectPhysicalDevice()` prefers discrete GPU
3. **Logical device** — `createLogicalDevice()` with graphics queue + DmaBuf extensions
4. **Command pool/fence** — for recording render commands

### 5b. DmaBuf Buffer Management

**Legacy single-output:** `setResolution(width, height, outBuffer)`:
- Calls `createExportableImage()` which:
  - Creates `VkImage` with `VK_IMAGE_TILING_OPTIMAL`
  - Allocates `VkDeviceMemory` with `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`
  - Exports memory as FD via `vkGetMemoryFdKHR`
  - Queries DRM modifier via `vkGetImageDrmFormatModifierPropertiesEXT`
  - Returns `DmaBufBuffer{fd, width, height, stride, format, modifier, size}`

**Multi-output:** `setResolutionForOutput(outputName, width, height, outBuffer)`:
- Same as above but stores in `m_outputTargets[outputName]`
- Default output aliases legacy path for backwards compatibility

### 5c. Frame Rendering (`renderFrame(time)`)

1. Records command buffer:
   - Transitions uploaded scene image to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`
   - Fullscreen triangle/quad draw to exportable DmaBuf image
2. Submits to graphics queue
3. Waits on fence
4. Current buffer pointer updated for D-Bus export

### 5d. Scene Image Upload

`uploadSceneImage(width, height, rgbaPixels)`:
- Copies RGBA8888 pixels into staging buffer
- Records `vkCmdCopyBufferToImage` to upload to scene texture
- Submits and waits

---

## 6. Vulkan Compute: `daemon/src/render/vulkan_compute.*`

**Purpose:** GPU-accelerated post-processing effects via compute shaders.

**Pipelines created in `SceneCompositor::initComputePipelines()`:**
- **blur** — 1 sampler + 1 storage image
- **water_waves** — 2 samplers + 1 storage image
- **pulse** — same bindings as water_waves
- **composition** — same bindings
- **film_grain** — 1 sampler + 1 storage image

**Shader bytecode:** Embedded as C arrays in `shaders_spv.h` (generated from `.comp` files).

**High-level apply functions:**
- `applyBlur(params)` — gaussian blur on image
- `applyWaterWaves(params)` — distortion using second texture as displacement
- `applyPulse(params)` — radial distortion
- `applyComposition(params)` — blends two images
- `applyFilmGrain(image, power, scale, frame)` — interleaved gradient noise

---

## 7. Render Graph: `daemon/src/render/render_graph.*`

**Purpose:** Higher-level abstraction for post-processing chains. Manages render targets and provides QPainter-based fallback implementations when Vulkan compute isn't available.

**Key methods:**
- `setResolution(width, height)` — clears existing targets
- `getOrCreateRenderTarget(name)` — lazily creates QImage targets
- `applyBlurPass()`, `applyWaterWavesPass()`, etc. — Qt-based shader emulation using QPainter

Used by `batch_verifier.cpp` for headless rendering without Vulkan.

---

## 8. Scene Parser: `daemon/src/scene/scene_parser.*`

**Purpose:** Reads Wallpaper Engine `.pkg` archives and builds `SceneDescription`.

**Parses:**
- `scene.json` — layer hierarchy, properties, metadata
- `project.json` — project settings
- Models (`.model`) — mesh data for puppet deformation
- Materials (`.material`) — texture bindings, blend modes
- Textures — DXT/BC compressed images, MP4 videos
- Particles — emitters with spawn rates, velocities, colors
- Effects — type, strength, speed, direction, color, visibility
- Live text/clock/date — renders text to QImage each frame

**Output:** `SceneDescription` containing:
- `layers` — vector of `SceneLayer` with image, transform, effects, parent pointer
- `emitters` — particle system configs
- Metadata (title, scene dimensions, etc.)

---

## 9. Asset Pipeline: `daemon/src/assets/*`

- **PkgReader** — opens `.pkg` archives (ZIP-like format), reads file entries
- **TexParser** — parses wallpaper metadata files
- **DxtDecoder** — decompresses DXT/BC texture formats
- **LibraryScanner** — scans Steam Workshop directories for installed wallpapers
- **VideoDecoder** — FFmpeg-based MP4 decoding for video wallpapers

---

## 10. Audio System: `daemon/src/audio/*`

- **AudioPlayer** — plays wallpaper audio tracks
- **AudioVisualizer** — captures system audio via PulseAudio/PipeWire monitor source, computes FFT bands for reactive effects

---

## 11. Pause Gate: `daemon/src/scene/pause_gate.*` + `compositor_backend.*`

**Purpose:** Skip rendering when wallpaper is fully covered by other windows (power optimization).

**How it works:**
1. **CompositorBackend** abstract interface (`daemon/src/scene/compositor_backend.h`) — queries the compositor for per-output coverage state:
   - `outputNames()` — outputs the daemon should track; empty means "no backend"
   - `isOutputCovered(name)` — true means pause this output (no tick, OST muted by default)
   - `update()` — refresh internal state from compositor events; called once per frame
   - `coverageThreshold()` — 0.0–1.0, default 0.90
2. **PauseGate** (`pause_gate.h` / `pause_gate.cpp`, `shouldRender()`) — decision logic:
   - Gate disabled: always render
   - `pauseAllOutputs` false: render if ANY output is uncovered
   - `pauseAllOutputs` true: render only if NO output is covered
   - Configurable `coverageThreshold` (default 0.90)
3. **Backend implementations** — see 11a–11c below. `makeCompositorBackend()` picks one at startup.
4. **FakeCompositorBackend** — test double for unit testing

**Integration:** `WallpaperService` calls `backend->update()` then `shouldRender()` in `updateAndRender()` before rendering each frame, and constructs the backend via `makeCompositorBackend()` in its constructor.

### 11a. Hyprland backend (unchanged, still the default on Hyprland)

`compositor_backend.cpp` — `HyprlandBackend`:
- Discovers the Hyprland control socket (`$HYPRLAND_INSTANCE_SIGNATURE`, or a signature directory under `$XDG_RUNTIME_DIR/hypr/`)
- Parses `hyprctl -j` JSON output for monitors, workspaces, clients
- Covered when the active workspace has a fullscreen client, or when the summed area of visible non-fullscreen clients on that monitor reaches `coverageThreshold`

**Summed, not unioned** — see the note in 11c for why the newer backends differ here.

### 11b. X11 EWMH backend — `x11_ewmh_backend.{h,cpp}`

Works on any EWMH-compliant X server (XFCE/xfwm4, LXDE, MATE, LXQt) by reading properties the window manager already maintains:

- `_NET_CLIENT_LIST`, `_NET_CLIENT_LIST_STACKING` — managed toplevels, bottom-to-top
- `_NET_CURRENT_DESKTOP`, `_NET_WM_DESKTOP` — visibility per workspace (`0xFFFFFFFF` = all desktops, as XFCE reports)
- `_NET_WM_STATE_FULLSCREEN`, `_NET_WM_STATE` — fullscreen detection
- `_NET_WM_WINDOW_TYPE` — chrome classification
- `_NET_DESKTOP_GEOMETRY` — the wallpaper area (falls back to root screen size)
- `_NET_SUPPORTING_WM_CHECK` — WM presence, for logging

Coverage: an output is covered when a fullscreen window on the current desktop overlaps it by at least the threshold, OR when the **union** of remaining normal-window rects reaches the threshold.

**Chrome is excluded** from coverage: `_NET_WM_WINDOW_TYPE_DOCK`, `_DESKTOP`, `_UTILITY`, `_SPLASH` and `skip-taskbar` windows are skipped. Without this, a docked `xfce4-panel` would read as a covering window and pause the wallpaper while the desktop is plainly visible. (The wallpaper's own window sets `_NET_WM_STATE_SKIP_TASKBAR`, so it never counts itself.)

**Output naming:** a single X screen is reported as one output named `X11-0`, bounded by the root geometry. Multi-head X11 is not split into per-monitor outputs.

**Exposed for testing:** `unionArea()` and `unionReachesThreshold()` are public statics so the coverage math is unit-tested headlessly (`tests/x11_ewmh_coverage_test.cpp`, registered as ctest `x11_ewmh_coverage` only when the backend was compiled in).

### 11c. labwc / wlr-IPC backend — `labwc_backend.{h,cpp}`

Speaks `wlr-ipc-unstable-v2` on the wire for labwc and other wlroots compositors, binding:

- `zwlr_output_manager_v1` — output names, positions, mode sizes
- `zwlr_foreign_toplevel_manager_v1` — the toplevel list
- `zwlr_foreign_toplevel_handle_v1` — title/app_id/state/output_enter/leave
- `zxdg_output_manager_v1` — authoritative logical output position/size
- `wl_output` — per-output geometry and the object identity used by `output_enter`

**The protocol tables are hand-written.** Gentoo portage has neither libwlroots nor `wayland-scanner` at build time, nor `wlrctl`/`swaymsg` binaries, so the `wl_interface`/`wl_message` tables are written out by hand from the upstream protocol XML (opcode == index in the `<request>`/`<event>` list; signatures match what `wayland-scanner` would emit, because libwayland reads them to type varargs and unpack events). Regenerating them with `wayland-scanner private-code` from the wlroots/xdg-protocols XMLs is the preferred long-term path and is documented at the top of `labwc_backend.cpp`.

**Limitation — per-window geometry is not available.** `wlr-foreign-toplevel-management` carries no window geometry (the entire event set is title, app_id, output_enter/leave, state, done, closed, parent). So only fullscreen and maximized toplevels can contribute rects, and a maximized one is approximated as the full output bounds. **A plain tiled window will NOT pause the wallpaper.** This is deliberate: guessing a rect would pause a visible wallpaper, which is worse than never pausing. Everything else (socket, registry, all globals, event state) is live. Dropping a real geometry source in later only requires pushing rects into that one vector — the union and threshold math is already exact and independent of rect origin.

Safety rules in the backend: every `wl_proxy_marshal_flags()` result is null-checked, listener callbacks tolerate null `user_data`/`proxy`, `update()` dispatches with a zero timeout so it never blocks, and a display error latches a dead state that clears all cached state and stops touching proxies. Not thread-safe - `update()` is `CompositorBackend::update()`, called from `WallpaperService::updateAndRender()` on the daemon's single frame-timer thread; call it from that same thread as the rest of the pause gate, the way the Hyprland backend is used.

### 11d. Backend selection — `makeCompositorBackend()`

`compositor_backend.cpp` probes the running session once at startup. First match wins; a candidate that is present but fails to initialize falls through to the next:

1. **Hyprland** — `$HYPRLAND_INSTANCE_SIGNATURE` set, or a live IPC socket under `$XDG_RUNTIME_DIR/hypr/`. Checked first specifically so existing Hyprland installs keep byte-identical behaviour.
2. **labwc** — `$WAYLAND_DISPLAY` set and the display socket connectable, then a real wlr-IPC registry round-trip. `$WAYLAND_DISPLAY` alone is not enough (GNOME, KDE and XWayland sessions all set it), so the backend itself has to confirm the globals. The socket probe is a bounded non-blocking `connect()` capped at 200 ms, so start-up cannot stall.
3. **X11** — `$DISPLAY` set; the backend opens the display and snapshots EWMH.
4. **`nullptr`** — nothing usable. Not an error: the pause gate never trips and the daemon keeps rendering. This is the safe fallback.

Backends that are not compiled into the binary are skipped. `compositor_backend.cpp` guards them behind `__has_include` plus `PWE_DISABLE_LABWC_BACKEND` / `PWE_DISABLE_X11_BACKEND` (the headless `pause_gate_test` target and the verifier define these, so neither pulls in xcb/libwayland).

**Union vs sum — why the newer backends differ.** The Hyprland backend *sums* client areas. The X11 and labwc backends compute the **union** of rects, clipped to the output bounds, by a sweep-line over x slabs. Summing double-counts overlap, so two windows overlapping 50% of the screen each can report a false "covered" verdict. The union/threshold decision is exposed as `unionReachesThreshold()` and exercised by `tests/x11_ewmh_coverage_test.cpp`. Note that the labwc backend's own `unionArea()` is an independent implementation from the X11 one — the file deliberately does not include the other backend's header.

### 11e. Tests

- `tests/pause_gate_test.cpp` (`ctest -R pause_gate_headless`) — headless decision tests over `shouldRender()` with `FakeCompositorBackend`
- `tests/x11_ewmh_coverage_test.cpp` (`ctest -R x11_ewmh_coverage`) — 22 assertions over the union-area sweep: clipping on all four edges, identical/overlapping/nested rects, gaps between rects, and the threshold decision including the zero-area bounds case. Registered only when the X11 backend was compiled in.
- `tests/hyprland_backend_test.cpp` (`ctest -R hyprland_backend_connect`) — integration-only; exits 77 (skip) when no Hyprland instance is discoverable, so non-Hyprland desktops report a skip rather than a failure. Registered with `SKIP_RETURN_CODE 77`, matching the `gpu_deform_probe` convention.

Current local state: `ctest` 4/5 pass, 1 skip.

---

## 12. Plugin System: `daemon/src/plugin/wallpaper_plugin.*`

**Purpose:** Extensibility layer for wallpaper plugins.

**Architecture:**
- `IWallpaperPlugin` interface — lifecycle hooks
- `HookPoint` enum — registration, load, unload, frame update, etc.
- `PluginRegistry` singleton — manages plugin registration/dispatch
- `REGISTER_WALLPAPER_PLUGIN` macro — self-registration

Currently used for builtin plugins, but designed for external plugins too.

---

## 13. KDE Plasma Plugin: `plugin/*`

**Purpose:** Desktop integration — QML UI + custom Qt Quick item for displaying wallpaper.

**Components:**
- **QML UI** — settings views (Library, Properties, Workshop, Engine Settings)
- **TextureItem** — custom `QQuickItem` that renders wallpaper
  - Currently renders black rectangle placeholder
  - Phase 2 plan: import DmaBuf as Qt Quick texture node for zero-copy display

**Communication:** Connects to daemon via D-Bus, calls `loadWallpaper()`, `getBufferFdForOutput()`, imports FD as texture.

---

## 14. Viewer: `viewer/*`

**Purpose:** Debug/preview tool that connects to daemon and displays live wallpaper.

**How it works:**
1. `ViewerWindow` connects to daemon via D-Bus
2. Calls `loadWallpaper()` with `.pkg` path
3. Receives `frameReady()` signal
4. Calls `getBufferFdForOutput()` to get DmaBuf FD
5. `LiveViewport` maps FD via `mmap()`
6. Renders mapped pixels using QPainter
7. Displays frame statistics

---

## 15. Batch Verifier: `daemon/src/tools/batch_verifier.cpp`

**Purpose:** Diagnostic tool that scans Steam Workshop wallpapers, renders them offscreen, and generates reports.

**Process:**
1. Scans library for `.pkg` files
2. Parses each wallpaper's scene
3. Renders layers/particles to QImage using QPainter
4. Captures PNG snapshot
5. Classifies as verified/needs-review/broken
6. Generates markdown report with per-layer debug info

Used for regression testing — baseline PNGs stored in `tests/regression/baseline/`.

---

## 16. Data Flow Summary

```
User selects wallpaper in Plasma
           │
           ▼
Plasma Plugin ──D-Bus──► WallpaperService::loadWallpaper(path)
           │                    │
           │                    ▼
           │              SceneCompositor::loadScene(pkg)
           │                    │
           │                    ▼
           │              SceneParser reads scene.json, textures, effects
           │                    │
           │                    ▼
           │              SceneDescription stored
           │
           │  ◄── 60 FPS timer ──►
           │
           ▼                    ▼
D-Bus getBufferFd()    updateAndRender() each frame:
     │                     - Audio tick
     │                     - Web/video refresh
     │                     - Layer compositing (QPainter)
     │                     - Particle simulation
     │                     - Film grain post-process
     │                     - Upload to Vulkan
     ▼                     - renderFrame() to DmaBuf
TextureItem                    │
(Qt Quick)                      ▼
  mmap DmaBuf FD         DmaBuf buffer ready
  display frame          for export
```

---

### 16a. Wallpaper display client: `layerclient/*`

The daemon is headless, so something has to put the exported DMA-BUF on screen. `layerclient` is that client for a real desktop background. It picks its display path at **runtime**, from the environment, not at configure time, so one binary works on both display servers (`WallpaperLayer::detectBackend()`):

1. `$WAYLAND_DISPLAY` set → **wlr-layer-shell** background surface
2. `$DISPLAY` set → **X11 EWMH desktop-level window**
3. Neither → falls back to the Qt platform name, and if that is inconclusive, assumes X11

Environment is tested before the Qt platform name because a Wayland session launched from inside an X session (labwc started by hand, a nested compositor) has both `WAYLAND_DISPLAY` and an inherited `DISPLAY` set, and only layer-shell actually works there.

**Wayland — wlr-layer-shell** (`configureLayerShell()`, via LayerShellQt): requests a `Layer::LayerBackground` surface with `exclusiveZone = -1` (ignores panel/bar exclusions), zero margins, all four anchors, `KeyboardInteractivityNone`, `scope = "wallpaper"`, pinned to a `QScreen`. The compositor sizes and places the surface, so the window is only made visible. LayerShellQt is optional at build time: without it the target still builds and only the X11 path is available, and on a Wayland session the client leaves the window unmapped rather than showing a fullscreen toplevel over the desktop. The same guard applies when the compositor does not implement wlr-layer-shell at all (`LayerShellQt::Window::get()` returns null).

**X11 — EWMH desktop window** (`x11_desktop_window.{h,cpp}`, `configureX11()`): there is no layer-shell protocol on X11, so the window is pushed down the stacking order by convention. It requests, as plain X properties:
- `_NET_WM_WINDOW_TYPE` = `_NET_WM_WINDOW_TYPE_DESKTOP`
- `_NET_WM_STATE` = `_NET_WM_STATE_BELOW` + `_NET_WM_STATE_STICKY` + `_NET_WM_STATE_SKIP_TASKBAR` + `_NET_WM_STATE_SKIP_PAGER`
- `_NET_WM_DESKTOP` = `0xFFFFFFFF` (all desktops)

Qt-side flags (`FramelessWindowHint | WindowStaysOnBottomHint | Tool`) are set before the native window exists; the EWMH properties above are written after `create()` and remain authoritative, because Qt stamps its own `_NET_WM_WINDOW_TYPE_UTILITY` at that point.

The work is split into two passes and both are required:
- `configureBeforeMap()` — properties must exist before the window manager handles the `MapRequest`; a window type written after the map is frequently ignored.
- `restackAfterMap()` — `_NET_WM_STATE_BELOW` is a *relative* stacking request and window managers recompute placement during the initial map, so the state is re-asserted both on a 0 ms post-map timer and on the first `Expose` (xfwm4 recomputes placement around exactly that moment), followed by a `ConfigureWindow` that pushes the window to the absolute bottom. It is re-asserted on screen geometry changes too, since a resize can push the window back up.

**Previously** on X11 the client fell through to `showFullScreen()`, which would have put a fullscreen toplevel *on top of* the desktop. The X11 path replaces that; the layer-shell path is unchanged and still preferred whenever `$WAYLAND_DISPLAY` is set.

The X11 path is always built (needs only `libxcb`); the layer-shell path is conditional on LayerShellQt being found. `layerclient/CMakeLists.txt` logs which path is active at configure time.

---

## 17. Key Technologies

- **C++20** — modern C++ with spans, atomic, smart pointers
- **Qt6** — QGuiApplication, QScreen, QPainter, QDBus, QtWebEngine
- **Vulkan** — GPU rendering, DmaBuf export, compute shaders
- **D-Bus** — IPC between daemon, Plasma plugin, viewer
- **SPIR-V** — embedded compute shader bytecode
- **FFmpeg** — video decoding for MP4 wallpapers
- **PulseAudio/PipeWire** — audio capture for reactive effects
- **DXT/BC textures** — compressed texture formats from Wallpaper Engine
- **JSON** — scene description parsing

---

## 18. Security Model

- **Trusted directories:** Only paths in `m_trustedDirs` can be loaded via D-Bus `loadWallpaper()`
- **CLI seeding:** `--trusted-directory=` args extend allowlist at startup
- **Ephemeral loads:** `loadWallpaperEphemeral(path)` grants one-shot trust for the wallpaper's
  effective load root (the directory itself, or a file's parent) and the grant is consumed by
  that load attempt — success *or* failure. Preview clients (the viewer) use this variant so
  previewing never permanently widens the allowlist; a crashed client cannot leak grants
  because none ever outlive the load call.
- **D-Bus registration:** Explicit adaptor class limits exposed interface
- **No arbitrary path loading:** Prevents loading wallpapers from untrusted locations

---

## 19. Current State & Missing Pieces

**Implemented:**
- Core rendering pipeline (layers, blend modes, effects)
- Particle systems
- Audio-reactive effects
- Film grain post-processing
- Multi-output DmaBuf export
- Pause gate with compositor backends for Hyprland, X11 (EWMH), and labwc (wlr-IPC)
- Web wallpapers (QtWebEngine)
- Video wallpapers (FFmpeg)
- Batch verifier + regression tests

**Not yet implemented / incomplete:**
- Plasma plugin `TextureItem` — currently black rectangle, needs DmaBuf import
- Puppet bone animation — CPU skinning is identity transform stub
- Advanced effects (color adjust, tint) — parsed but not applied
- Proper error handling for corrupt wallpapers
- Performance optimizations for layer caching
- labwc pause backend: per-window geometry (wlr-foreign-toplevel-management does not expose it, so plain tiled windows do not pause)
- X11 pause backend: multi-monitor output splitting (a single X screen is reported as one output, `X11-0`)

---

## 20. Platform support

The daemon is display-server agnostic: it renders headless and shares DMA-BUFs over D-Bus, and both the pause gate and the display client pick their path at runtime from the environment. Current state:

| Compositor / session | Pause gate | Wallpaper display path |
|---|---|---|
| **Hyprland** (Wayland) | Hyprland IPC (`hyprctl` JSON) — fullscreen workspace, or summed client area ≥ threshold | wlr-layer-shell via LayerShellQt |
| **XFCE / xfwm4, MATE, LXDE, LXQt** (X11) | X11 EWMH — fullscreen window, or union of normal-window rects ≥ threshold | EWMH desktop-level window (`_NET_WM_WINDOW_TYPE_DESKTOP` + `BELOW`/`STICKY`/`SKIP_TASKBAR`/`SKIP_PAGER`) |
| **labwc** (wlroots) | wlr-IPC — fullscreen or **maximized** toplevel only; see the limitation below | wlr-layer-shell via LayerShellQt |
| Anything else, or no session detected | gate inactive, daemon keeps rendering | layer-shell on Wayland, X11 desktop window otherwise |

**The Hyprland path is retained, not replaced.** It is still checked first, so existing Hyprland users get the same behaviour they had before the X11 and labwc backends were added. The Hyprland backend is the only one that pauses on plain tiled windows, because it is the only one whose IPC exposes per-client geometry.

**labwc limitation, stated plainly:** `wlr-foreign-toplevel-management` does not expose per-window geometry, so only fullscreen and maximized toplevels contribute rects. A plain tiled window will not pause the wallpaper. This is intentional — a wrong guess would pause a visible wallpaper.

**X11 caveat:** a single X screen is reported as one output (`X11-0`) bounded by the root geometry; multi-head X11 is not split per monitor.

**Build requirements per backend:** the X11 backends need `libxcb`; the labwc backend needs `libwayland-client` but not libwlroots. Both are detected with `pkg-config` and compiled out when absent, with `PWE_DISABLE_X11_BACKEND` / `PWE_DISABLE_LABWC_BACKEND` keeping detection in sync. The display client needs `libxcb` always and LayerShellQt optionally. `CMAKE_EXPORT_COMPILE_COMMANDS` is ON so editors can resolve Qt6 headers, which are not on the default include path on Gentoo.

---

That's the full architecture. Every major component, how it initializes, what it does per frame, and how pieces connect.
