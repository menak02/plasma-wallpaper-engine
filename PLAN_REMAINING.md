# Remaining Remediation Plan — Plasma Wallpaper Engine

Generated 2026-09-01. Batch 55✅/17⚠️/0❌. VulkanCompute ACTIVE, Web stub, video lazy, FFT radix2 done.

## Gap vs Almamu linux-wallpaperengine (OpenGL 3.3, GLFW/SDL2, mpv/FFmpeg, Pulse FF TW3, CEF)

### P0 — Crash / Correctness (must fix before features)
1. **VideoDecoder double-free** `daemon/src/assets/video_decoder.cpp:34` `avio_context_free` vs `avformat_close_input` with `AVFMT_FLAG_CUSTOM_IO`, `thread_count 0` thread-pool race on 2nd `638→1080` video in same wallpaper `3122339805`. Mitigated by not keeping 3 decoders at parse (`scene_parser.cpp:825` now single temp-file first-frame + `videoData` lazy). Remaining: daemon tick still creates 1 decoder per `videoData` layer sequentially and keeps it `shared_ptr` → will re-trigger. Fix: single shared decoder reused round-robin, mutex, `thread_count 1` for verifier, `avcodec_flush_buffers` before `avformat_close_input` with `pb=null`.
2. **Vulkan dispatch empty sets** `daemon/src/render/vulkan_compute.cpp:416` `applyBlur` creates `DescriptorPool` but never `VkWriteDescriptorSet` for `inputImage.view/sampler` → dispatch with unbound descriptors → validation black. Must wire `updateDescriptorSets`.

### P1 — User-visible wallpaper types
3. **Web** `daemon/src/scene/web_wallpaper.cpp:97` `grab()` needs `loadFinished` + `QEventLoop` 1s timeout, `QtWebChannel` `wallpaperPropertyListener` bridge for `userProperties`. Currently placeholder `20,20,30`. Add `find_package Qt6 WebEngine` already optional, add `QWebChannel` + `runJavaScript`.
4. **Puppet warp / bones** `daemon/src/render/mesh_renderer.h:14` `boneWeights` unused. `scene_parser.cpp:586` `resolveModel` ignores `bones`/`weights`/`puppet` JSON. Need parse and feed to `MeshDeformer` per-vertex bone transform.
5. **Effects tail** `scene_parser.h:28` added `FoliageSway/FilmGrain/Blur` but `scene_compositor.cpp:292` only `FoliageSway→Wind` 12px, `FilmGrain` ignored. Need `RenderGraph` GPU path for `blur` first, then `FilmGrain` noise overlay.

### P2 — Interactivity / properties
6. **Property live reload** `daemon/src/ipc/wallpaper_service.cpp:193` `setProperty` stores but `SceneDescription` never re-evaluates `JSEngine::init` or `visible` `opacity` etc. Almamu `--list-properties --set-property`. Add `WallpaperService::setProperty` → `SceneParser::reparse` or `JSEngine` re-init + `loadScene` without re-reading PKG.
7. **Audio reactive live** `daemon/src/audio/audio_visualizer.cpp:167` `startLiveCapture` `QAudioSource` stub, not called from `wallpaper_service`. Almamu `--no-audio-processing` `PulseAudio` 64 `barcount` → `engine.registerAudioBuffers`. Wire `PulseAudio` monitor → `AudioVisualizer::onLiveData` ring `8192` → `getBand` → `JSEngine` + `Vulkan` push constants.

### P3 — Platform / perf
8. **Multi-output / scaling** `daemon/src/vulkan/vulkan_context.cpp:258` single `m_sharedImage` `1920x1080` `main.cpp:41`. Need `--screen-root/span --scaling stretch/fit/fill --clamping` per output `wlr-layer-shell` + `xdg-output`, `KDE` `LayerShell` plugin `plugin/src/package.cpp`.
9. **Caching** `daemon/src/assets/tex_parser.cpp:57` per-layer `DxtDecoder::decodeToRgba` 33M `ldexp` for `RG1616F`, no `QCache<string, TexImage>`. Add `LRU` `ComputeImage` cache.
10. **Tests / CI** `daemon/src/tools/batch_verifier.cpp:61` manual `QGuiApplication` 40s, no `ctest` diff vs Almamu `--screenshot` `analysis_results.md` 72 PNGs not compared. Add `ctest` `compare` `threshold`.

## Commit slicing (one commit per bullet, verify build+batch each)
- C1: Video single-reuse mutex + thread_count 1
- C2: RenderGraph blur GPU dispatch with descriptor writes
- C3: Puppet bone parse + MeshDeformer bone path (stub)
- C4: Property live reload D-Bus
- C5: Web loadFinished sync + JS bridge
- C6: Pulse live wire + batch/daemon smoke

Each commit must `cmake --build build -j` `build/daemon/plasma-wallpaper-engine-verifier` 72/72 `55/17/0` unchanged or better, `build-asan` clean, `rtk git add` + `commit`.
