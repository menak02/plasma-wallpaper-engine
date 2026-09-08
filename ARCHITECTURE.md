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

This is the **heart of the engine**. It loads Wallpaper Engine `.pkg` files and composites layers every frame.

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
    - **Wind/WaterWaves/WaterRipple/FoliageSway** — sets mesh deformation flags
    - **Shake** — sine-based XY jitter
    - **Blur/FilmGrain/ColorAdjust/Tint** — handled later
  - **Mesh deformation** (if wind/water effects or puppet bones):
    - Generates 8×8 vertex grid
    - Deforms vertices based on time/speed/strength/direction
    - Renders deformed mesh with texture
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
1. **CompositorBackend** abstract interface — queries compositor for per-output coverage state
2. **HyprlandBackend** — concrete implementation:
   - Discovers Hyprland control socket
   - Parses `hyprctl` JSON output
   - Tracks monitors, workspaces, clients
   - Determines if output is covered by fullscreen/tiling windows
3. **PauseGate** — decision logic:
   - If `pauseAllOutputs` is false: render if ANY output is uncovered
   - If `pauseAllOutputs` is true: render only if ALL outputs are uncovered
   - Configurable `coverageThreshold` (default 90%)
4. **FakeCompositorBackend** — test double for unit testing

**Integration:** Called from `WallpaperService::updateAndRender()` before rendering each frame.

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
- Hyprland pause gate
- Web wallpapers (QtWebEngine)
- Video wallpapers (FFmpeg)
- Batch verifier + regression tests

**Not yet implemented / incomplete:**
- Plasma plugin `TextureItem` — currently black rectangle, needs DmaBuf import
- Puppet bone animation — CPU skinning is identity transform stub
- Advanced effects (color adjust, tint) — parsed but not applied
- Proper error handling for corrupt wallpapers
- Performance optimizations for layer caching

---

That's the full architecture. Every major component, how it initializes, what it does per frame, and how pieces connect.
