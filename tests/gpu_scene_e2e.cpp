// End-to-end GPU scene regression: load a real .pkg, render it through the
// GPU compositor, and assert the exported DmaBuf contains a real frame.
//
// WHY THIS EXISTS
// The 72-wallpaper PNG regression (tests/regression) renders on the CPU
// QPainter path only -- daemon/src/tools/batch_verifier.cpp never calls the
// GPU compositor. So the entire Vulkan pipeline is invisible to it. That is
// how a wallpaper which rendered as a flat single-colour fill on screen still
// scored "69/72 byte-identical". Every GPU-path bug found so far
// (orphaned descriptor sets after a texture-pool rebuild, and R/B channel
// transposition on the dmabuf blit) was invisible to that suite and to the
// three existing GPU probes, which construct synthetic scenes rather than
// parsing a real package.
//
// This test closes that gap using the committed fixture scene.pkg. It is the
// smallest scene that still exercises parse -> texture upload -> instance
// packing -> render -> blit -> export.
//
// WHAT IT ASSERTS
//   1. The scene parses and reports at least one layer with a decoded image.
//   2. The GPU compositor initialises and reports the GPU path is active.
//   3. The exported DmaBuf, read through the same mmap path the layerclient
//      uses, has many distinct colours. A flat fill is <= 2 and means the
//      frame never landed -- this is the regression that matters.
//   4. Row-to-row variation exists: a wallpaper that is one uniform colour
//      per row is broken even if the colour count is high.
//   5. Channel order is sane: the frame is not a single-channel image
//      replicated across R, G and B (a classic sign of a broken format
//      conversion or a descriptor bound to the wrong texture).
//
// Exits 77 (skip) when no Vulkan device is present so GPU-less developer
// machines still pass. CI installs lavapipe and treats a skip as a failure.

#include "../daemon/src/assets/pkg_reader.h"
#include "../daemon/src/scene/scene_compositor.h"
#include "../daemon/src/vulkan/vulkan_context.h"

#include <QGuiApplication>

#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using WallpaperEngine::Assets::PkgReader;
using WallpaperEngine::Render::DmaBufBuffer;
using WallpaperEngine::Render::VulkanContext;
using WallpaperEngine::Scene::SceneCompositor;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

struct FrameStats {
    size_t distinctColors = 0;
    bool rowsVary = false;
    bool channelsDiffer = false;
    uint32_t firstPixel = 0;
};

// Reads the exported DmaBuf exactly the way the layerclient does.
FrameStats inspect(int fd, size_t size, uint32_t stride, uint32_t w, uint32_t h)
{
    FrameStats s;
    void* p = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        return s;
    }
    const auto* b = static_cast<const unsigned char*>(p);

    std::set<uint32_t> px;
    std::vector<uint32_t> firstOfRow;
    for (uint32_t y = 0; y < h; y += 2) {
        const auto* row = b + static_cast<size_t>(y) * stride;
        uint32_t first = uint32_t(row[0]) | (uint32_t(row[1]) << 8)
                       | (uint32_t(row[2]) << 16);
        firstOfRow.push_back(first);
        for (uint32_t x = 0; x < w; x += 2) {
            const auto* q = row + static_cast<size_t>(x) * 4;
            // dmabuf is B8G8R8A8; treat it as opaque and compare all three.
            px.insert(uint32_t(q[0]) | (uint32_t(q[1]) << 8)
                      | (uint32_t(q[2]) << 16));
            if (px.size() > 200000) {
                break;
            }
        }
        if (s.firstPixel == 0 && y == 0) {
            s.firstPixel = first;
        }
    }
    s.distinctColors = px.size();

    // Rows must not all be the same colour.
    for (size_t i = 1; i < firstOfRow.size(); ++i) {
        if (firstOfRow[i] != firstOfRow[0]) {
            s.rowsVary = true;
            break;
        }
    }

    // At least one pixel must have R, G and B not all equal.
    for (uint32_t y = 0; y < h && !s.channelsDiffer; y += 8) {
        const auto* row = b + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < w; x += 8) {
            const auto* q = row + static_cast<size_t>(x) * 4;
            if (!(q[0] == q[1] && q[1] == q[2])) {
                s.channelsDiffer = true;
                break;
            }
        }
    }

    munmap(p, size);
    return s;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    if (argc < 2) {
        std::printf("usage: gpu_scene_e2e <path-to-scene.pkg>\n");
        return 2;
    }
    const std::string pkgPath = argv[1];
    const uint32_t W = 1920, H = 1080;

    VulkanContext ctx;
    if (!ctx.init()) {
        std::printf("SKIP: no Vulkan device available\n");
        return 77;
    }

    DmaBufBuffer buf;
    if (!ctx.setResolution(W, H, buf)) {
        // 78, not 77: see blit_shared_probe.cpp. This environment cannot
        // export a DMA_BUF, which is distinct from having no GPU at all.
        std::printf("UNSUPPORTED: cannot export a DMA_BUF (needs a DRM render node)\n");
        return 78;
    }
    std::printf("exported DmaBuf: %ux%u stride=%u size=%zu\n",
                W, H, buf.stride, buf.size);

    // SceneCompositor owns its own GpuQuadCompositor internally, so this goes
    // through the public load path rather than poking the compositor directly.
    SceneCompositor compositor(&ctx);
    compositor.setTargetResolution(W, H);

    PkgReader pkg;
    check(pkg.open(pkgPath), "fixture scene.pkg opened");
    if (!pkg.isOpen() && !compositor.hasScene()) {
        // open() failing is already recorded as a failed check above.
    }

    const bool loaded = compositor.loadScene(pkg);
    check(loaded, "scene loaded into SceneCompositor");
    if (!loaded) {
        std::printf("gpu_scene_e2e: %d assertion(s) FAILED\n", ++g_failures);
        return 1;
    }

    check(compositor.hasScene(), "a scene is loaded");
    check(compositor.usingGpuCompositor(),
          "GPU path is active (a CPU-painter fallback must not pass this test)");
    std::printf("  scene layers: %zu\n", compositor.getScene().layers.size());
    check(!compositor.getScene().layers.empty(), "scene has at least one layer");

    // Render a few frames so time-driven state settles, then inspect.
    for (int i = 0; i < 8; ++i) {
        compositor.updateAndRender(1.0f / 60.0f, 0.5f + i * 0.016f);
    }

    const FrameStats s = inspect(buf.fd, buf.size, buf.stride, W, H);
    std::printf("  exported frame: distinct colours=%zu firstPixel=0x%08x\n",
                s.distinctColors, s.firstPixel);

    // The regression this exists for: a flat single-colour frame.
    check(s.distinctColors > 2,
          "exported DmaBuf is not a flat fill (this is the flat-frame regression)");
    check(s.distinctColors > 64,
          "exported DmaBuf has real image content (>64 distinct colours)");
    check(s.rowsVary, "colour varies down the frame (rows are not uniform)");
    check(s.channelsDiffer, "R/G/B are not all identical (channel order sane)");

    if (g_failures) {
        std::printf("gpu_scene_e2e: %d assertion(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("gpu_scene_e2e: all assertions passed\n");
    return 0;
}