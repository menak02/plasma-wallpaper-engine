# Remaining V2 — Post 184d3cb

Batch 55✅/17⚠️/0❌. Video single-decoder, web loadFinished, live props, Vulkan descriptors fixed.

## Still open (from previous 10)

### C7 TexCache + puppet bones (P3 perf + P1 warp)
- `daemon/src/assets/tex_parser.cpp:57` per-layer `DxtDecoder` 33M `ldexp` for same `fondo.tex` 92×.
  Fix: `QCache<string, TexImage>` LRU 64, `resolveTexture` check cache before `PkgReader::readFile` + `TexParser::parse`.
- `daemon/src/scene/scene_parser.cpp:586` `resolveModel` ignores `puppet.json` `bones [{name,parent,pos,angle,weights}]`.
  Fix: parse `bones` into `SceneLayer::bones` vector, feed `MeshDeformer` bone-weighted deform (stub: log + identity transform, no GL).

### C8 Viewer --list-properties + sliders (P2) — ✅ DONE
- `viewer/src/main.cpp` now supports `--list-properties <id>` (D-Bus `getWallpaperProperties`, sorted key=value output) and `--set-property k=v [file]` (parses bool/int/double/string, optional wallpaper load first, then `setProperty`). GUI mode unchanged. Batch 55/17/0 unchanged.

### C9 wlr-layer-shell multi-output (P3) — ✅ DONE
- `VulkanContext` now keeps a per-output map `outputName -> OutputTarget{image, memory, DmaBufBuffer}` (`setResolutionForOutput`, `getBufferForOutput`, `removeOutput`, `getOutputNames`); image creation factored into shared `createExportableImage`. `renderFrame` copies/clears the legacy primary image AND every registered output image in one command buffer. D-Bus slots added: `setResolutionForOutput(name,w,h)`, `getBufferFdForOutput(name)`, `getBufferInfoForOutput(name)`, `getOutputs()`. `main.cpp` switched to `QGuiApplication` and auto-registers a buffer per `QScreen` at its native size. Live smoke test on eDP-1 registered 1920x1080. Batch 55/17/0 unchanged.

Each commit `cmake --build build -j` `verifier 72/72` `55/17/0` unchanged or better, `daemon` `VulkanCompute ACTIVE`.
