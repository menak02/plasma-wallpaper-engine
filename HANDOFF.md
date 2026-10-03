# Handoff — Gentoo/XFCE port

Written 2026-10-02. Branch `feat/hyprland-pause`, pushed to
`origin/feat/hyprland-pause` at `dc78135`. Working tree clean.

## What this machine is

Gentoo, **XFCE on X11** (`DISPLAY=:0.0`, xfwm4), with **labwc** available as
an alternative Wayland compositor. NVIDIA RTX 5070, driver 595.396. PipeWire
is running as a systemd user service. The project was written for Arch +
Hyprland, so three things were hardcoded to it and all three are now ported.

Build deps that had to be emerged (sudo required):

```
kde-plasma/layer-shell-qt dev-qt/qtmultimedia x11-apps/xrandr x11-apps/xdpyinfo
```

## Commits

| SHA | What |
|---|---|
| `42ce787` | X11 EWMH + labwc pause backends, auto-detect factory, X11 display path, tests |
| `202b9dc` | ARCHITECTURE.md / README.md / PLAN_REMAINING_V3.md platform updates |
| `efefec3` | `blit_shared_probe` — covers the DmaBuf path the live client consumes |
| `9230a4b` | **fix(gpu)**: descriptor sets orphaned on texture-pool growth |
| `dc78135` | **fix(layerclient)**: `_NET_WM_WINDOW_TYPE` written with type WINDOW instead of ATOM |

## Verified working

- **Build**: configures and compiles clean with `cmake -G Ninja`.
- **ctest**: 6/7 pass, 1 skipped. The skip is `hyprland_backend_connect`,
  which correctly returns 77 (skip) off-Hyprland — a live Hyprland failure
  would still return 1.
- **Pause gate, live against this session.** Auto-detects X11 EWMH. Verdict
  is `no` with the desktop visible, `yes` under a real
  `_NET_WM_STATE_FULLSCREEN` window, `yes` under a maximized window, and back
  to `no` when each closes. These are two different code paths and both were
  checked separately.
- **Window placement, live against xfwm4**:
  ```
  _NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_DESKTOP
  _NET_WM_STATE(ATOM) = _STICKY, _SKIP_PAGER, _SKIP_TASKBAR, _BELOW
  ```
  Stacked directly above xfdesktop, below the panel.
- **Corpus**: all 72 baselined wallpaper IDs present on disk (101 total) at
  `~/.local/share/Steam/steamapps/workshop/content/431960`. Wallpaper Engine
  itself (appid 431960) is already installed — no Steam subscribe needed.
- **Regression**: 69/72 byte-identical against the committed baseline.

## Known issues, in priority order

### 1. GPU path renders a partial scene — OPEN, biggest remaining gap

This is the one that matters. Measure with `tools/dmabuf_probe.cpp`:

```
g++ -std=c++20 -fPIC tools/dmabuf_probe.cpp -o build/probes/dmabuf_probe \
    $(pkg-config --cflags --libs Qt6Core Qt6DBus)
```

then, with the daemon running and a wallpaper loaded:
`./build/probes/dmabuf_probe DP-4`.

Current numbers for wallpaper `2374244268`:

| Path | Distinct colours |
|---|---|
| CPU painter (`~/.wallpaper-engine-verifier-output/*.png`) | 65,249 |
| GPU path (live dmabuf) | **2,062** |

`9230a4b` fixed the catastrophic case (1 distinct colour = flat fill) by
re-pointing descriptors after a texture-pool rebuild. The GPU path now draws
real content but still reproduces far less of the scene than the CPU painter,
and rows are uniform where the CPU render has variation.

**Why the existing tests missed this**: the 72-wallpaper regression uses
`batch_verifier.cpp`, which renders on the **CPU QPainter path only** and never
calls the GPU compositor. So the regression gate is blind to the entire GPU
pipeline. Anything GPU-only needs its own probe, as `blit_shared_probe` and
`gpu_quad_placement_probe` now do.

Likely areas, in order: blend modes (the scene uses `blend=0` translucent
throughout, and `mapBlend` collapses multiply/screen to translucent — the
documented CPU-path degradation, but it may be far more visible on GPU);
texture upload fidelity for the 3840x2160 background; and the Y-orientation
of `buildGpuFrame`'s `finalY` flip (`scene_compositor.cpp:280`, which
inverts `origin.y()` against `sceneH`).

### 2. Pre-existing crash in the render path — OPEN, unrelated to the port

The plain `RelWithDebInfo` build segfaults in `operator new` (heap
corruption) on a 135-layer wallpaper, around #30 of a 71-wallpaper walk.
Reproduces identically on unmodified code. Absent under ASAN+UBSAN at the
same optimisation level, which completes all 71 cleanly. `VideoDecoder` opens
right before the crash and `scene_compositor.cpp:446` already documents
multi-decoder heap corruption, so the FFmpeg/layer-video path is the likely
owner. Separate piece of work.

### 3. Vulkan version mismatch — OPEN, latent

`VulkanContext::initInstance` requests `VK_API_VERSION_1_2`, but
`blitIntoSharedImage` calls `vkCmdBlitImage2`, which is **core in 1.3**. It
works on this driver, but the loader trampoline is `NULL` when a layer is
enabled, producing a segfault at `vulkan_context.cpp:716`.

### 4. Unbounded texture cache — OPEN, not urgent

`m_textures` / `m_slotTextures` in `GpuQuadCompositor` are never evicted, so a
long session accumulates every layer image of every wallpaper it has shown.
Multi-GB of GPU memory over time. It did not cause the flat fill. Eviction
changes cache semantics, so it wants its own change.

### 5. `QT_QPA_PLATFORM=offscreen` breaks output registration

With that env var the daemon registers a fake output `screen0` at 800x800
instead of the real `DP-4`. Harmless in probes, but it will mislead CI. Any
daemon run that needs real outputs must not set it — `gpu_deform_probe`,
`blit_shared_probe`, and `gpu_quad_placement_probe` are all fine because they
construct their own `VulkanContext`.

### 6. Three pre-existing regression diffs

Unrelated to the port, unchanged across it:

- `3601075812` — 39 of 150 layers unrendered; scene layers reference
  non-existent parent IDs.
- `3673417519` — never processed by the verifier at all.
- `3762441477` — frame-timing sensitive; verified 1/1 layers but the captured
  frame differs.

### 7. Right-click on the desktop — UNTESTED

The window now has the correct DESKTOP type and stacks above xfdesktop, so
input should reach the desktop. This has **not** been verified — try it. If
clicks are swallowed, the likely cause is that xfdesktop itself is also a
`_NET_WM_WINDOW_TYPE_DESKTOP` window and the two compete for the same layer;
the wallpaper window may need to go *below* xfdesktop rather than above it, or
xfdesktop needs to be disabled while the wallpaper client runs.

## Not started (pre-existing roadmap)

Unchanged by the port, still on `PLAN_REMAINING_V3.md`:

- **S4 / T3** mouse click forwarding — was next before the port.
- **S6 / T6** workshop UX loop ("Open in Steam" deep link + rescan).
- **S8 / T8** MDLA puppet animation playback.
- Autostart stays deliberately reverted (issue #29, 16-17 W idle).

## XFCE plugin — feasible, not started

An XFCE desktop provider reading the same D-Bus API the Plasma plugin uses is
a viable end state, and arguably the right integration for this desktop. Not
attempted. It is downstream of issue 1: a desktop integration is not worth
building until the frames are correct. Sketch: a `xfdesktop`-replacement
window that reads `getBufferFdForOutput` exactly as `layerclient` does, so it
inherits the same EWMH path rather than needing a second one.

## qtwebengine — BUILD INTERRUPTED, WORK PRESERVED

Web wallpapers need it and it is **not** installed. The first build failed
with a GCC 15 internal compiler error (segfault in `atomic_base.h` on
`quiche_ip_address.cc`) that portage misreported as an OOM despite 24 GiB
free. Root cause was load, not code: the same file compiles clean 3/3
standalone at the same 8M stack, so `-j20` was the trigger.

Resumed at `-j6` and reached **18227/23491 with zero errors** before being
paused on request. The emerge was subsequently killed; its log in `/tmp` was
cleaned, but the portage work dir survived:

```
/var/tmp/portage/dev-qt/qtwebengine-6.11.1/work/   # 8.2G, intact
```

Resume with:

```
pkexec env MAKEOPTS="-j6 -l6" emerge --resume --jobs=1 -k dev-qt/qtwebengine
```

Ninja state lives in the work dir, not process memory, so this picks up
rather than restarting. After it installs, reconfigure and rebuild to flip
`Qt6 WebEngine: DISABLED` → `ENABLED`, then re-run the 72-wallpaper
regression — web wallpapers currently render as failures, so that number
should improve.

## Build and test commands

```sh
cd ~/Code/plasma-wallpaper-engine

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j8

QT_QPA_PLATFORM=offscreen ctest --test-dir build -E png_regression_batch

# 72-wallpaper regression (needs a verifier output dir to exist first)
./build/daemon/plasma-wallpaper-engine-verifier
python3 tests/regression/compare_images.py tests/regression/baseline \
    ~/.wallpaper-engine-verifier-output
```

Note: `cmake --build build -j8` — not `-j20`. That parallelism setting is
what triggered the qtwebengine ICE.
