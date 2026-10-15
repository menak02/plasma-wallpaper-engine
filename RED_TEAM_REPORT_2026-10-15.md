# RED TEAM REPORT + NEXT-PHASE PLAN — plasma-wallpaper-engine

**Date:** 2026-10-15 · **Method:** 3-agent red team (security / rendering-systems / codebase map) + live desktop probe · **Base commit:** 2230b95 · **Role:** senior systems engineer (rendering pipelines, syscalls, untrusted-input hardening)

---

## 0. Environment probe (SSH into live desktop)

- Session: **GNOME shell** on tty2/seat0 — **not Plasma**. Plugin cannot be live-tested here; daemon/layer-shell path only. (SSH env is `tty`; session bus reachable at `/run/user/1000/bus`.)
- Daemon **not running**; no `org.antigravity.WallpaperEngine` on the session bus at probe time.
- Serena LSP: initialized (`opencode.json` MCP entry + `.serena/project.yml`, cpp/clangd) — but **no `compile_commands.json`** in repo, so clangd falls back to blind parsing. Fix: `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` + symlink to repo root.

## 1. Severity roll-up (35 findings)

### CRITICAL (5)

| # | Area | Finding | Evidence |
|---|------|---------|----------|
| C1 | Vulkan | **GPU use-after-free every frame** — `uploadSceneImage` destroys staging buffer still referenced by previous frame's pending `vkQueueSubmit`; fence only waited later in `renderFrame` | destroy `vulkan_context.cpp:394`, pending submit `:546`, fence wait `:450` (tick order main.cpp:126→129) |
| C2 | Vulkan | **R/B channel swap on entire GPU path** — RGBA8888 canvas raw-memcpy'd into `VK_FORMAT_B8G8R8A8_UNORM`; **CI blind**: verifier bypasses Vulkan entirely | `scene_compositor.cpp:518-519`, `vulkan_context.cpp:293,351`, `batch_verifier.cpp:100,148,250` |
| C3 | Security | **DXT decoder u32 integer overflow → heap buffer overflow** (RCE via hostile workshop tex): `width*height*4` wraps; pixel loops write past alloc | `dxt_decoder.cpp:166` + 12 sites (242…381) |
| C4 | Security | **D-Bus zero peer authorization** — all 30+ slots callable by any session-bus peer (trust-root expansion, JS injection, dma-buf fd grab) | `main.cpp:89-99` (`ExportAllSlots`), all slots in `wallpaper_service.cpp` |
| C5 | Vulkan | **Multi-output copy uses legacy resolution extent** for every target → truncated copies / UB on non-1080p outputs | `vulkan_context.cpp:501-505` applied to `targets` `:463-466` |

### HIGH (11)

| # | Area | Finding | Evidence |
|---|------|---------|----------|
| H1 | Security | `registerTrustedDirectory` accepts world-writable dirs (`/tmp`) — only `"/"` rejected; `addCustomLibraryPath` weaker (exists() only). Feeds parser RCE chain | `wallpaper_service.cpp:363-387`, `:348-361` |
| H2 | Security | TEXB0001/0002 path: **no 100MB cap** on `uncompSize` (cap exists only in TEXB0003/0004) + `static_cast<int>` negative capacity for `LZ4_decompress_safe`, return ignored | `tex_parser.cpp:396-427` |
| H3 | Security | Predictable `/tmp/plasma_we_bgm.<ext>` write, `QFile` follows symlinks → local arbitrary-file clobber | `audio_player.cpp:38-46` |
| H4 | Security | Untrusted workshop HTML in **unsandboxed QtWebEngine** (local-file access default on, no profile isolation) + `setProperty` interpolates D-Bus strings into `runJavaScript` unescaped → JS injection → file read + exfil | `web_wallpaper.cpp:45-71,125-141`, `scene_compositor.cpp:177-205` |
| H5 | Security | AVIO seek callback: unclamped `pos` → `size_t` underflow in read callback → OOB heap read | `video_decoder.cpp:81-88` |
| H6 | Rendering | **Staging buffer alloc+free every frame** (`vkCreateBuffer`+`vkAllocateMemory` per tick) | `vulkan_context.cpp:414-431`, destroy `:394` |
| H7 | Rendering | **Film-grain full CPU round-trip**: 2 GPU submits, 4–5× frame memcpy, double `vkWaitForFences` stall, result re-uploaded instead of dispatched onto export image | `vulkan_compute.cpp:589-653`, `:255,:274` |
| H8 | Rendering | **No damage tracking** — 60 Hz unconditional re-composite/upload/submit; idle no-scene still submits `vkCmdClearColorImage` every 16 ms (= plan gap G2/T4) | `main.cpp:111-133`, `vulkan_context.cpp:509-524` |
| H9 | Rendering | **Blocking syscalls on main thread**: audio capture boot up to ~5 s (`pactl`+`pw-record` waits), `pactl` every second (500 ms wait), sync pkg parse+DXT decode inside D-Bus slot, `WebEngineView::grab()` per frame, sync `LibraryScanner` in ctor | `audio_visualizer.cpp:196-254`, `audio_player.cpp:74-75`, `wallpaper_service.cpp:21,221-334`, `web_wallpaper.cpp:107` |
| H10 | Rendering | **Static video-decoder map leak + cross-wallpaper id collision** — keyed by layer id (restarts at 0 per scene); wallpaper switch reuses wrong decoder, old ones never freed | `scene_compositor.cpp:228-264` |
| H11 | Vulkan | Staging size `finalWidth*finalHeight*4` in u32 → overflow → undersized buffer + GPU OOB; D-Bus `setResolution` accepts unbounded uint32 (GPU-mem DoS) | `vulkan_context.cpp:406`, `wallpaper_service.cpp:90-99` |

### MEDIUM (13, condensed)

- PKG entry `offset+length` u32 wrap + uncapped `entry.length` alloc vs actual file size (`pkg_reader.cpp:98-103`).
- `sws_scale` fed target height as source slice height — OOB on upscale (`video_decoder.cpp:224-226`, `:191`).
- QImage wraps decoder buffer with mismatched dims → OOB read (`scene_parser.cpp:773-775,927-929` vs `dxt_decoder.cpp:347-349`).
- DMA-BUF fds exported to any session peer, no caller check (`wallpaper_service.cpp:66-75,112-121`).
- No acquire/release sync for dma-buf readers: barrier `dstStage=BOTTOM_OF_PIPE`, viewer mmaps without `DMA_BUF_IOCTL_SYNC`, never re-fetches on resize, ignores `bufferResized` (`vulkan_context.cpp:528-536`, `viewer_window.cpp:53,212-224`).
- **TexCache effectively dead**: `QCache` max cost 64 KB rejects all real textures; `if (true)` re-decodes on cache hit (`scene_parser.cpp:22,746-758`).
- Queue family picked for GRAPHICS without COMPUTE check; `VulkanCompute` hardcodes family 0; `findMemoryType` returns 0 on failure; nearly every `VkResult` unchecked (`vulkan_context.cpp:196-201,275,323,434,546`, `vulkan_compute.cpp:16,255-274`).
- Multi-output = one legacy-res canvas blitted everywhere; `setResolutionForOutput` reallocates without fence (`scene_compositor.cpp:124-131`, `vulkan_context.cpp:459-467,559`).
- Per-frame heap churn: FFT vectors, ring-buffer erase-front, targets vector, bone-deform vectors, `finalImg` deep copy (`audio_visualizer.cpp:137,145,267-268`, `scene_compositor.cpp:415-442`, `vulkan_context.cpp:402,460`).
- `std::cout` every frame from upload path (`vulkan_context.cpp:442-443`).
- Library scanner path-existence oracle: `preview: "../../../../.ssh/id_rsa"` surfaces in `getLibrary` (`library_scanner.cpp:142,164`).
- JS evaluated on main thread, no timeout — `while(true){}` hangs daemon (`js_engine.cpp:276,288-291`).
- Untrusted video piped through host `ffmpeg` CLI (argv-safe, but large CVE surface) (`scene_parser.cpp:824-829,894-899`).

### LOW (6, condensed)

- Premultiplied-alpha mismatch: straight-alpha export read as `ARGB32_Premultiplied` by viewer (`viewer_window.cpp:72` vs `scene_compositor.cpp:65`).
- Grain frame index = `floor(time)` → grain animates at 1 Hz not 60 (`scene_compositor.cpp:487`).
- Unchecked `vkMapMemory` before memcpy (`vulkan_context.cpp:434`, `vulkan_compute.cpp:591,651`).
- Plugin `TextureItem` is a placeholder — dma-buf exported but **no in-tree consumer** (`texture_item.cpp:15-24`).
- `batch_verifier` hardcoded output dir + debug ID list (`batch_verifier.cpp:161`).
- CI `regression-full.yml` verifier invocation mismatch: no `--output` flag; positional arg makes workshop=`$HOME/verifier-output` (empty) while compare reads that dir; real output stays `~/.wallpaper-engine-verifier-output` — **CI corpus path is broken as written** (`.github/workflows/regression-full.yml` vs `batch_verifier` arg parsing).

## 2. Verified ALREADY SECURE (do not re-litigate)

1. Trusted-root prefix match correct — `startsWith(root + '/')` so `/a/b` ≠ `/a/bc`; `..` rejected post-canonicalize (`wallpaper_service.cpp:49-56`).
2. PKG entry-name sanitization: `..` rejected, leading `/` stripped, entry count ≤1024, text files ≤8 MB (`pkg_reader.cpp:7-17,37,113`).
3. All LZ4 via `LZ4_decompress_safe` with bounded output; TEXB0003/0004 capped at 100 MB (`tex_parser.cpp:219,229,325,337,367,379`).
4. Zero command injection — `ffmpeg`/`pw-record`/`parec`/`ffplay`/`pw-play`/`pactl` all argv lists, sink name from `pactl` output never from wallpaper; no `system()`/`popen()`/shell.
5. DXT block reads bounds-checked (`dxt_decoder.cpp:202-212`); `.rej` QImage-stride fix already applied (`scene_parser.cpp:760-768,920-928`); `registerTrustedDirectory` refuses `/`; QJSEngine sandbox exposes no fs/net/QObject bridges (`js_engine.cpp:228-260`).

## 3. Dead-code inventory (confirmed by call-graph)

| Item | Status |
|---|---|
| `daemon/src/vulkan_context.{cpp,h}` (root stub) | DEAD — not in CMake source list (real one: `src/vulkan/vulkan_context.cpp`) |
| RenderGraph pass helpers (blur/water/pulse/composition, render targets) | DEAD — only `setResolution`/`clear` ever called; **prior "all dead" claim confirmed** |
| `VulkanCompute::applyBlur/WaterWaves/Pulse/Composition`, `dispatchCompute`, layout/copy helpers | DEAD — only `applyFilmGrain` wired (`scene_compositor.cpp:490`) |
| `PluginRegistry::dispatch` + plugin hooks | DEAD — no call sites, `loadBuiltinPlugins` no-op |
| `daemon/test_one.cpp`, 4× `.bak`/`.rej` (`tex_parser.cpp.rej`, `tex_parser.{cpp,h}.bak`, `dxt_decoder.cpp.bak`) | DEAD — not built (explicit CMake source lists); `.bak` retains weaker parsing → delete |
| `avioReadPacket` placeholder, `AudioVisualizer::computeBands`, `WebWallpaper::loadFile` | DEAD — no call sites |
| `ParticleEngine` | **ALIVE** — updated+rendered every frame (`scene_compositor.cpp:478-479`) — refutes "particle dead" assumption |
| `MeshDeformer` | HALF — grid deform live; bone path live but identity-stub (T8) (`mesh_renderer.h:35-36`) |
| `getBufferFdForOutput`/`setResolutionForOutput` | Exported, **no in-tree consumer** (viewer uses legacy `getBufferFd` only) |

## 4. Per-frame cost breakdown (1920×1080, F ≈ 7.9 MiB)

**Grain off, 1 output:** CPU composite writes F → upload `scaled()`+`copy()` touches 2F → staging memcpy F (+ full Vulkan buffer/memory create-alloc-free per frame) → GPU copy F VRAM (1 submit) → D-Bus signal. Viewer mmap read = true zero-copy *if fd consumed* (plugin consumes nothing).
→ **CPU copies/frame ≈ 2–3×F (~16–24 MB), 1 staging alloc/free cycle/frame.**

**Grain on:** +memcpy in F +memcpy out F + GPU buf→img/dispatch/img→buf + second submit + double full pipeline stall, then export image written *again* (grain never feeds it directly). → **4–5×F CPU copies, 2 submits.**

**Idle no-scene:** still 1 `vkCmdClearColorImage` submit per 16 ms per output.

## 5. Module map (factual)

Daemon src ≈ 9.0k LOC / repo text ≈ 11.1k. Modules: assets 1844 (+4 stale), audio 587, ipc 519, plugin 233, render 2069 (5 `.comp` + `shaders_spv.h`), scene 2577 (`scene_parser.cpp` 984), vulkan 702, main 141, tools 305. plugin/ 788 (QML 712), viewer 558, tests 72 baseline PNGs + `compare_images.py` (dependency-free PNG decoder; tolerances PIXEL_TOLERANCE=8, MAE=2.0, CHANGED_RATIO=2%), scripts 98, CI 99.
Build: cmake≥3.20, C++20, Qt6 (WebEngine/Multimedia optional), Vulkan required, liblz4+ffmpeg required. **No sanitizers anywhere** (despite `build-asan/` in .gitignore); **`CMAKE_EXPORT_COMPILE_COMMANDS` unset**; install() only daemon→libexec + plugin QML/data; verifier and viewer **not installed**. ctest: single `png_regression_batch` (timeout 900, registered only if both dirs exist at configure).
Stubs: video_decoder QImage stub, mesh_renderer bone stub (T8), web_wallpaper no-WebEngine stub, CPU skinning identity.
Absent: README, LICENSE, PKGBUILD/AUR, .desktop, systemd unit, second workflow.

## 6. Roadmap delta vs PLAN_REMAINING_V3

Plan's S3 (fullscreen pause) is correct work but **wrong next move** — rendering correctness + parser security outrank features.

| Phase | Scope | Rationale |
|---|---|---|
| **P0 — Stabilize GPU path** | C1 fence-before-free, C2 RGBA→BGRA, C5 per-target extent; **add Vulkan-path CI probe** (render ≥1 wallpaper through real upload+renderFrame offscreen, diff PNG) | Daemon ships wrong colors + UAF; CI certifies a path the daemon doesn't execute |
| **P1 — Parser hardening** | C3 DXT u64 sizing + pixel cap, H2 TEXB0001/2 cap, H5 AVIO seek clamp, PKG entry-vs-filesize; hostile fixtures in `tests/fixtures/hostile/` (overflow tex, huge-entry pkg, seek-bomb mp4) — must fail safely | RCE/DoS surface from hostile workshop content |
| **P2 — IPC hardening** | C4 peer authz on `registerTrustedDirectory`/`setProperty`/`getBufferFd*` (caller pid/uid or polkit), H1 refuse world-writable trust roots, H4 WebEngine profile isolation + JSON-escape setProperty, H3 QTemporaryFile for bgm | Closes sandbox→daemon escape chain |
| **P3 — Perf** | H6 persistent staging, H7 fuse grain onto export image, H8 dirty-flag/idle gating (= plan T4/G2 merged), H9 worker-thread offload of capture boot/scans/decodes, H10 decoder-map ownership, H11 widen staging math + clamp resolution | Kills ~60% frame traffic + idle GPU burn |
| **P4 — Feature parity** | T3 mouse/click forwarding, T4 fullscreen pause, T5 autostart+install, T6 workshop UX loop | Original plan S3–S6 |
| **P5 — Ship** | T7 README+LICENSE+AUR, **fix broken CI corpus invocation** (`--output` mismatch), delete dead files, enable `EXPORT_COMPILE_COMMANDS`, sanitizer CI job (justified by P1 findings), issue/PR templates, tag→release | Highest visibility-per-effort |

Unchanged from plan: T8 MDLA playback (P5+), T9 QRhi/full-Vulkan composition (medium-term; P3's fused-grain work is the first step), T10 17⚠️ script wallpapers.

Commit gates (unchanged): `cmake --build build -j`, verifier 72/72 mae=0.000, ctest green, batch 55/17/0 unchanged or better.

---

# NEXT-PHASE STRUCTURED PROMPT

````markdown
# Role
You are a senior systems engineer specializing in graphics pipelines (Vulkan,
dma-buf, Wayland) and untrusted-input hardening (C/C++ parsers, IPC authz).
Work via SSH into a live Linux desktop session (GNOME on tty2, seat0; session
bus at /run/user/1000/bus). Desktop is logged in; the project daemon is NOT
running — start it for live probes when needed.

# Project
plasma-wallpaper-engine — C++20/Qt6 Wallpaper Engine-compatible daemon:
untrusted Steam workshop packages (LZ4 pkg, TEX/DXT, video, JS/web) → CPU
QPainter composite → Vulkan compute/export → per-output dma-buf via D-Bus
(org.antigravity.WallpaperEngine). CI: 72-wallpaper PNG regression via
batch_verifier (NOTE: verifier bypasses the Vulkan path — GPU bugs invisible
to CI). Full plan: PLAN_REMAINING_V3.md. Findings: RED_TEAM_REPORT_2026-10-15.md.
Serena LSP active (cpp/clangd).

# Current state (post-red-team, verified)
- 5 CRITICAL: GPU staging UAF (vulkan_context.cpp:394 vs :546), RGBA/BGRA
  swap on GPU path, DXT u32 overflow → heap overflow (dxt_decoder.cpp:166),
  zero D-Bus peer authz, multi-output wrong copy extent.
- 11 HIGH: /tmp trust-root, TEXB0001/2 uncapped alloc, /tmp bgm symlink
  clobber, unsandboxed QtWebEngine + setProperty JS injection, AVIO seek OOB,
  per-frame staging alloc, film-grain CPU round-trip, no damage tracking
  (60fps idle burn), blocking main-thread syscalls, static decoder-map leak,
  staging 32-bit overflow + unbounded setResolution.
- Verified secure: trusted-root prefix match, pkg name sanitization,
  LZ4_decompress_safe, argv-only process spawn, QJSEngine sandbox.
- Dead code: root vulkan_context.*, RenderGraph helpers, 4× .bak/.rej,
  test_one.cpp, PluginRegistry::dispatch.
- CI bug: regression-full.yml verifier args don't match CLI (positional eats
  output dir) — corpus regression is broken as written.

# Phase objective
P0→P2 (stabilize + harden) before any new features. Do NOT start S3/T3-T7
until P0-P2 land green.

# Method
1. Read PLAN_REMAINING_V3.md + RED_TEAM_REPORT_2026-10-15.md.
2. Build: cmake -B build -DCMAKE_BUILD_TYPE=Debug
   -DCMAKE_EXPORT_COMPILE_COMMANDS=ON (symlink compile_commands.json to
   repo root for clangd). Then cmake --build build -j.
3. Per finding: fix → ctest (png_regression_batch must stay 72/72 PASS, mae=0)
   → batch 55/17/0 unchanged or better → commit (conventional, sliced per
   finding-class, not per file).
4. Live verification over SSH/D-Bus where possible (deny/allow probes for
   authz, hostile fixture packages for parsers, validation layers for Vulkan).
5. Add a Vulkan-path regression probe: render ≥1 wallpaper through
   uploadSceneImage+renderFrame offscreen, dump PNG, diff vs baseline — CI
   must stop certifying a path the daemon doesn't execute.
6. New hostile-input fixtures live in tests/fixtures/hostile/ (overflow tex,
   4GiB-entry pkg, seek-bomb mp4) — each must fail safely, not crash.

# Constraints
- Never weaken the mae=0.000 baseline gate.
- No feature work (mouse forwarding, fullscreen pause, autostart) until P0-P2.
- Security fixes over convenience: peer authz on D-Bus, refuse world-writable
  trust roots, sandbox WebEngine profile.
- Delete dead code rather than leaving it (.bak, .rej, duplicate
  vulkan_context, dead RenderGraph helpers) — but verify CMake source lists
  first.
- Every Vulkan call that can fail: check VkResult, fall back to CPU path.
- Report after each phase: findings fixed / test results / new findings.

# Deliverable per slice
Commit + one-paragraph verification note (what was probed live, what ctest
said) appended to PLAN_REMAINING_V3.md status line.
````
