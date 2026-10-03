// Diagnostic: does blitIntoShared() — the path the layerclient actually
// consumes — write the composited frame into the exportable DmaBuf?
//
// Existing gpu_deform_probe validates the GPU path through
// readback(), which copies to a host-visible staging buffer. The live
// wallpaper client does not use readback(): it mmaps the exported DmaBuf
// that blitIntoShared() writes. So blitIntoShared() has no coverage, and
// a defect there is invisible to ctest while producing garbage on screen.
//
// This probe asserts the exported DmaBuf actually changes when the scene
// changes, which is the property the client depends on. It exits 77 when
// no Vulkan device is present.
//
// Run headless with QT_QPA_PLATFORM=offscreen (set here).

#include "../daemon/src/render/gpu_quad_compositor.h"
#include "../daemon/src/vulkan/vulkan_context.h"

#include <QGuiApplication>

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using WallpaperEngine::Render::GpuGrainParams;
using WallpaperEngine::Render::GpuLayer;
using WallpaperEngine::Render::GpuParticle;
using WallpaperEngine::Render::GpuQuadCompositor;
using WallpaperEngine::Render::VulkanContext;

namespace {

constexpr uint32_t kW = 1920, kH = 1080;

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

// Reads the exported DmaBuf through a fresh mmap, exactly as the
// layerclient does, and reports how many distinct bytes it contains plus
// the first texel. Uninitialized memory shows up as a low distinct-byte
// count or as a constant fill.
struct BufStats {
    size_t distinctBytes;
    uint32_t firstPixel;
    bool changed;
};

BufStats inspectFd(int fd, size_t size, uint32_t stride, uint32_t w, uint32_t h) {
    BufStats stats{0, 0, false};
    void* ptr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        return stats;
    }
    const auto* p = static_cast<const uint8_t*>(ptr);

    bool seen[256] = {};
    size_t distinct = 0;
    for (size_t i = 0; i < size; ++i) {
        if (!seen[p[i]]) {
            seen[p[i]] = true;
            ++distinct;
        }
    }
    stats.distinctBytes = distinct;
    stats.firstPixel = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    munmap(ptr, size);
    return stats;
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    VulkanContext ctx;
    if (!ctx.init()) {
        std::printf("SKIP: no Vulkan device available\n");
        return 77;
    }

    WallpaperEngine::Render::DmaBufBuffer buffer;
    if (!ctx.setResolution(kW, kH, buffer)) {
        std::printf("SKIP: could not allocate an exportable DmaBuf\n");
        return 77;
    }
    std::printf("exported DmaBuf: %ux%u stride=%u size=%zu fd=%d\n",
                kW, kH, buffer.stride, buffer.size, buffer.fd);

    GpuQuadCompositor gpu;
    if (!gpu.init(&ctx, kW, kH)) {
        std::printf("SKIP: GpuQuadCompositor failed to initialize\n");
        return 77;
    }

    // textureIndex is the slot the compositor resolves; UINT32_MAX-style
    // negative slots are not used here, so a plain layer with the default
    // white texture is enough to prove the blit lands.
    const auto makeLayer = [](uint32_t texIndex) {
        GpuLayer layer;
        layer.textureIndex = texIndex;
        layer.centerX = static_cast<float>(kW) / 2.0f;
        layer.centerY = static_cast<float>(kH) / 2.0f;
        layer.width = static_cast<float>(kW);
        layer.height = static_cast<float>(kH);
        layer.opacity = 1.0f;
        layer.blendMode = 0;
        layer.deformed = false;
        return layer;
    };

    const float clearA[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float clearB[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    GpuGrainParams noGrain{};

    // Frame 1: black clear, no layers at all. Whatever lands in the
    // exported buffer therefore comes from the clear, not from geometry.
    std::vector<GpuLayer> layersA;
    const bool drewA = gpu.renderFrame(layersA, {}, clearA, noGrain, 0.0f);
    const bool blittedA = drewA && gpu.blitIntoShared();
    check(drewA, "renderFrame succeeded (frame 1)");
    check(blittedA, "blitIntoShared succeeded (frame 1)");

    const BufStats a = inspectFd(buffer.fd, buffer.size, buffer.stride, kW, kH);
    std::printf("  frame1 distinctBytes=%zu firstPixel=0x%08x\n",
                a.distinctBytes, a.firstPixel);

    // A solid clear means few distinct byte values is EXPECTED and correct.
    // What must not happen is the buffer keeping whatever garbage the
    // allocation handed back, so compare against the next frame instead.
    (void)makeLayer;

    // Frame 2: different clear color. The exported buffer MUST change.
    std::vector<GpuLayer> layersB;
    const bool drewB = gpu.renderFrame(layersB, {}, clearB, noGrain, 1.0f);
    const bool blittedB = drewB && gpu.blitIntoShared();
    check(blittedB, "blitIntoShared succeeded (frame 2)");

    const BufStats b = inspectFd(buffer.fd, buffer.size, buffer.stride, kW, kH);
    std::printf("  frame2 distinctBytes=%zu firstPixel=0x%08x\n",
                b.distinctBytes, b.firstPixel);

    check(a.firstPixel != b.firstPixel,
          "exported DmaBuf changes between frames (blit actually lands)");


    // Compare against readback(), which the existing probe already covers.
    // If readback shows a correct image but the DmaBuf does not, the defect
    // is isolated to blitIntoShared().
    QImage viaReadback;
    if (gpu.readback(viaReadback)) {
        const QRgb rb = viaReadback.pixel(0, 0);
        std::printf("  readback pixel(0,0)=0x%08x  dmabuf pixel(0,0)=0x%08x\n",
                    (unsigned)rb, b.firstPixel);
        const int dr = qRed(rb), db = qRed((QRgb)b.firstPixel);
        // Frame 2 clear color is (0.25, 0.5, 0.75) -> red channel ~64.
        check(std::abs(dr - db) <= 8,
              "blitIntoShared pixel matches readback pixel (channels aligned)");
    } else {
        std::printf("WARN: readback() unavailable, skipping cross-check\n");
    }

    // ---- Textured frame ----
    // Clear-color cases above never touch the texture path, but every
    // real wallpaper does. A scene whose layers carry actual images is the
    // case that has to work, so cover it explicitly.
    QImage tex(64, 64, QImage::Format_RGBA8888);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            tex.setPixel(x, y, qRgba(20 + x * 3, 40 + y * 2, 200, 255));
        }
    }
    const uint32_t texIndex = gpu.getOrCreateTexture(tex, 9001);
    check(texIndex != UINT32_MAX, "getOrCreateTexture succeeded");
    if (texIndex == UINT32_MAX) {
        std::printf("  lastError: %s\n", gpu.lastError().c_str());
    } else {
        GpuLayer textured;
        textured.textureIndex = texIndex;
        textured.centerX = static_cast<float>(kW) / 2.0f;
        textured.centerY = static_cast<float>(kH) / 2.0f;
        textured.width = static_cast<float>(kW);
        textured.height = static_cast<float>(kH);
        textured.opacity = 1.0f;
        textured.blendMode = 0;

        std::vector<GpuLayer> layersC{textured};
        const bool drewC = gpu.renderFrame(layersC, {}, clearA, noGrain, 2.0f);
        const bool blittedC = drewC && gpu.blitIntoShared();
        check(drewC, "renderFrame succeeded (textured frame)");
        check(blittedC, "blitIntoShared succeeded (textured frame)");

        const BufStats c = inspectFd(buffer.fd, buffer.size, buffer.stride, kW, kH);
        std::printf("  textured distinctBytes=%zu firstPixel=0x%08x\n",
                    c.distinctBytes, c.firstPixel);
        check(c.firstPixel != a.firstPixel,
              "textured frame reaches the exported DmaBuf (texture path works)");
        check(c.distinctBytes > 4,
              "textured frame has real image content in the exported DmaBuf");

        QImage texReadback;
        if (gpu.readback(texReadback)) {
            std::printf("  readback tex pixel(0,0)=0x%08x dmabuf=0x%08x\n",
                        (unsigned)texReadback.pixel(0, 0), c.firstPixel);
        }
    }

    if (g_failures) {
        std::printf("blit_shared_probe: %d assertion(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("blit_shared_probe: all assertions passed\n");
    return 0;
}
