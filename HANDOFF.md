# Plasma Wallpaper Engine — Handoff

## Current Status
- **Build**: Clean compile — daemon, viewer, verifier all build
- **Batch**: 55 ✅, 17 ⚠️, 0 ❌ (72 wallpapers total)
- **Regression**: Zero — all prior fixes preserved
- **Opacity mask fix**: Applied ✅ — 3771397959 no longer black
- **Coordinate fix**: Confirmed algebraically identical to old code (both correct)
- **Rect special case**: Removed (was rarely triggered)

## Root Cause Found: Opacity Masks Ignored

### The Problem
Solid layers with `effects/opacity/effect.json` and a mask texture (e.g., Vignettage, Solide) are rendered as **solid opaque rectangles** because:
1. `resolveEffect()` classifies opacity effects as `EffectType::Tint` or falls to default `EffectType::WaterWaves`
2. `scene_compositor.cpp` never applies `LayerEffect::maskImage` to layer images
3. `batch_verifier.cpp` same issue

### Example: 3771397959 (completely black)
```
LAYER: 'Vignettage' id=228 visible=1 opacity=1 imageNull=0 imgW=64 imgH=64
  centerPixel=rgba(0,0,0,255) opaque~512 trans~0 blend=0
  origin=(1920,1080) size=(3840,2160)
  effects: [{"file": "effects/opacity/effect.json", "passes": [{"textures": [null, "masks/opacity_mask_44e4bf0b"]}]}]
```
- `solidlayer` creates 64x64 solid black (0,0,0,255) image
- Opacity mask `masks/opacity_mask_44e4bf0b.tex` exists in PKG (187KB)
- Without mask applied, entire 3840x2160 area is opaque black
- Rendered last (zOrder highest), covers everything → completely black

### Fix Needed
1. ~~Add `EffectType::OpacityMask` to enum in `scene_parser.h`~~ ✅ DONE
2. ~~Detect "opacity" in effect file path and set type to `OpacityMask`~~ ✅ DONE
3. ~~In `scene_compositor.cpp` and `batch_verifier.cpp`, for `OpacityMask` effects:~~ ✅ DONE
   - ~~Load mask texture (already in `LayerEffect::maskImage`)~~
   - ~~Apply mask to layer image: multiply alpha by mask grayscale value~~
   - ~~Or: generate masked solid image at layer.size dimensions~~

### Results After Fix
- **3771397959**: Vignettage now transparent at center (was solid black). Canvas shows content.
- **3725071796**: No opacity masks. Missing layers = Clock (visible=false, script-dependent)
- **3715762023, 3640755971, 3465215190**: Already "Completely Sure" — "scattered" issue was visual only or already resolved

### Remaining Issues
1. **Script-dependent visibility** — Clock/Date/Day layers have `visible: false` or `{"user":"clockdateday","value":false}` → need JavaScript evaluation
2. **Foliagesway effect** — 3725071796 "Fille" has foliagesway effects with masks (not opacity masks, foliage sway masks) — may affect visual quality
3. **Other effect types** — filmgrain, blurprecise, iris, shake, waterwaves — currently classified but not applied

## Completed Work (Do NOT Redo)
- Full red team analysis, security audit
- CMakeLists restructured, KDE deps optional
- Q_OBJECT headers added, AUTOMOC enabled
- Texture parser: TEXB0004 fully rewritten (FIF+isVideoMp4 detection, MP4 vs non-MP4 paths)
- DXT decoder: auto-detection before ARGB fallback, half-float support, MP4/ftyp detection
- GenericImage: alpha swap removed (was destroying images)
- Stride mismatch fixed in resolveTexture()
- Format enum matches Almamu's codes
- SceneLayer: id/parentId (ints), angles (QVector3D), parent/children vectors
- Topological sort: parents render before children (depth-first by zOrder)
- Transform inheritance: resolveParentTransform() walks parent chain root→leaf
- VideoDecoder C++ class created (ffmpeg wrapper, AVFormatContext, custom AVIO)
- VideoDecoder integrated into batch_verifier (ffmpeg CLI approach — write temp MP4, shell ffmpeg, read PNG)
- 3432157109 video playback working
- All debug output cleaned
- Batch verifier accepts CLI args, --id filter for single-wallpaper debug
- Opacity mask support: EffectType::OpacityMask, applyOpacityMasks() applies mask to layer images at parse time
- 72 PNGs in ~/.wallpaper-engine-verifier-output/

## ⚠️ Wallpapers (Script-Dependent)
These show "wrong" because they need JavaScript for time-of-day visibility control:
3122339805, 3353454232, 3465215190, 3504887068, 3591326656, 3604883533, 3677082224, 3690417937, 3691117335, 3708834685, 3771397959

## Video Textures Found
- 3432157109: `wp_lighthouse_sunset.tex` (2560x1440 h264) ✅
- 3673417519/3668185720: `lofi_chloe_night_loop_WE.tex` (1920x1080)
- 3504887068: `150357561853877bbe8152cd85.tex` (1920x1080)
- 3515745440: `Untitled.tex` (1920x1080)
- 3122339805: `City Video.tex` (638x746), `Nightlife Vid.tex` (1080x1080), `ezgif.com-reverse.tex` (336x336)

## Key Files
- `daemon/src/scene/scene_parser.h` — EffectType::OpacityMask added ✅
- `daemon/src/scene/scene_parser.cpp` — applyOpacityMasks() added ✅, resolveEffect() detects opacity
- `daemon/src/scene/scene_compositor.cpp` — no changes needed (mask applied at parse time)
- `daemon/src/tools/batch_verifier.cpp` — --id filter for single-wallpaper debug
- `daemon/src/assets/tex_parser.cpp` — TEXB parser (DO NOT TOUCH FIF/imageCount reordering)
- `daemon/src/assets/dxt_decoder.cpp` — texture decoding
- `daemon/src/assets/video_decoder.h/cpp` — ffmpeg wrapper
- `~/.wallpaper-engine-verifier-output/` — 72 PNGs + analysis_results.md
- `/tmp/linux-wallpaperengine/` — Almamu's reference (DO NOT MODIFY)

## System
- Omarchy (Arch-based), no KDE Plasma
- Qt6 6.11.2, Vulkan 1.4.357, GCC 16.2.1, Python 3.14.6
- Caveman ultra mode active

## Rules
- Almamu's parser has flaws ours fixed — don't reintroduce them
- tex_parser.cpp FIF/imageCount reordering BROKE ALL WALLPAPERS — was reverted, never touch again
- TEXB0004: FIF+isVideoMp4 fields ONLY after imageCount; per-mipmap extras ONLY for MP4 video textures
