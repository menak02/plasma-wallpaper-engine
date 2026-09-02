# Remaining V2 — Post 184d3cb

Batch 55✅/17⚠️/0❌. Video single-decoder, web loadFinished, live props, Vulkan descriptors fixed.

## Still open (from previous 10)

### C7 TexCache + puppet bones (P3 perf + P1 warp) — ✅ DONE
- TexCache: `QCache<string, TexImage>` LRU 64 in `scene_parser.cpp:22` (earlier commit).
- Puppet bones: parsed into `SceneLayer::bones` (`scene_parser.cpp:638`), and now fed through the render path: new `MeshDeformer::boneWeightedDeform` (CPU skinning stub — builds world-space bone matrices from the parent chain, assigns grid vertices to nearest bones with inverse-distance weights, identity transform in rest pose per wqLouis MDLV0023 reference; bone matrices are bind-pose refs, not applied at rest). `scene_compositor.cpp` renders layers with bones via the deform path (8x8 grid + `renderDeformedMesh`) instead of plain `drawImage`. Animated bone state (`animatedPos`/`animatedAngle`) left at identity until MDLA animation playback is implemented. Verified on 3771397959 (Fille_puppet.mdl): renders unchanged in rest pose, 55/17/0 intact.

### C8 Viewer --list-properties + sliders (P2) — ✅ DONE
- `viewer/src/main.cpp` now supports `--list-properties <id>` (D-Bus `getWallpaperProperties`, sorted key=value output) and `--set-property k=v [file]` (parses bool/int/double/string, optional wallpaper load first, then `setProperty`). GUI mode unchanged. Batch 55/17/0 unchanged.

### C9 wlr-layer-shell multi-output (P3) — ✅ DONE
- `VulkanContext` now keeps a per-output map `outputName -> OutputTarget{image, memory, DmaBufBuffer}` (`setResolutionForOutput`, `getBufferForOutput`, `removeOutput`, `getOutputNames`); image creation factored into shared `createExportableImage`. `renderFrame` copies/clears the legacy primary image AND every registered output image in one command buffer. D-Bus slots added: `setResolutionForOutput(name,w,h)`, `getBufferFdForOutput(name)`, `getBufferInfoForOutput(name)`, `getOutputs()`. `main.cpp` switched to `QGuiApplication` and auto-registers a buffer per `QScreen` at its native size. Live smoke test on eDP-1 registered 1920x1080. Batch 55/17/0 unchanged.

Each commit `cmake --build build -j` `verifier 72/72` `55/17/0` unchanged or better, `daemon` `VulkanCompute ACTIVE`.

## Regression tests (P3 item 10 from PLAN_REMAINING) — ✅ DONE
- `tests/regression/compare_images.py`: dependency-free PNG decoder + pixel diff (MAE threshold 2.0, changed-pixel ratio 2%, per-pixel tolerance 8). Supports single pairs and full directory comparison.
- `tests/regression/baseline/`: 72 reference PNGs from the verified 55✅/17⚠️/0❌ run.
- CMake: `enable_testing()` + `png_regression_batch` ctest (auto-skips if baseline or verifier output missing; 900s timeout).
- Workflow: run `plasma-wallpaper-engine-verifier` first, then `cd build && ctest`. Verified: 72/72 matched, mae=0.000, ctest passes (~6.5 min).
