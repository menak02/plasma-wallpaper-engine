// Diagnostic: reproduce the live wallpaper's GPU corruption at real
// dimensions. Not part of ctest — this exists to answer one question:
// does a full-screen textured layer render correctly at 1920x1080 output with
// a 3840x2160 source texture (the shape of wallpaper 2374244268), or does it
// collapse to the clear colour the way the on-screen wallpaper does?
//
// Run: QT_QPA_PLATFORM=offscreen ./build/probes/real_size_probe

#include "../daemon/src/render/gpu_quad_compositor.h"
#include "../daemon/src/vulkan/vulkan_context.h"

#include <QGuiApplication>
#include <QImage>

#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <set>

using namespace WallpaperEngine::Render;

namespace {

constexpr uint32_t kOutW = 1920, kOutH = 1080;
constexpr uint32_t kTexW = 3840, kTexH = 2160;

size_t distinctColors(const QImage& img)
{
    std::set<uint32_t> px;
    for (int y = 0; y < img.height(); y += 2) {
        for (int x = 0; x < img.width(); x += 2) {
            px.insert(uint32_t(img.pixel(x, y)));
            if (px.size() > 300000) return px.size();
        }
    }
    return px.size();
}

// A gradient with per-pixel variation, so a wrong UV or a channel swap shows
// up as a distinct-colour count far below the real thing.
QImage makeBigTexture()
{
    QImage img(kTexW, kTexH, QImage::Format_RGBA8888);
    for (int y = 0; y < kTexH; ++y) {
        for (int x = 0; x < kTexW; ++x) {
            img.setPixel(x, y, qRgba(uint8_t(x % 256), uint8_t(y % 256),
                                     uint8_t((x + y) % 256), 255));
        }
    }
    return img;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    VulkanContext ctx;
    if (!ctx.init()) {
        std::puts("SKIP: no Vulkan device");
        return 77;
    }

    DmaBufBuffer buf;
    if (!ctx.setResolution(kOutW, kOutH, buf)) {
        std::puts("SKIP: no exportable DmaBuf");
        return 77;
    }

    GpuQuadCompositor gpu;
    if (!gpu.init(&ctx, kOutW, kOutH)) {
        std::printf("FAIL: gpu.init: %s\n", gpu.lastError().c_str());
        return 1;
    }

    const float clear[4] = {0.7f, 0.7f, 0.7f, 1.0f};  // scene clear is grey
    GpuGrainParams noGrain{};

    // Baseline: empty frame should be exactly the clear colour.
    QImage empty;
    if (gpu.renderFrame({}, {}, clear, noGrain, 0.0f) && gpu.readback(empty)) {
        std::printf("empty frame: distinct=%zu pixel(0,0)=0x%08x\n",
                    distinctColors(empty), (unsigned)empty.pixel(0, 0));
    }

    const QImage tex = makeBigTexture();
    std::printf("texture: %dx%d distinct=%zu\n",
                tex.width(), tex.height(), distinctColors(tex));

    const uint32_t idx = gpu.getOrCreateTexture(tex, 4242);
    if (idx == UINT32_MAX) {
        std::printf("FAIL: getOrCreateTexture: %s\n", gpu.lastError().c_str());
        return 1;
    }
    std::printf("texture index = %u\n", idx);

    GpuLayer layer;
    layer.textureIndex = idx;
    layer.centerX = kOutW / 2.0f;
    layer.centerY = kOutH / 2.0f;
    layer.width = float(kOutW);
    layer.height = float(kOutH);
    layer.opacity = 1.0f;
    layer.blendMode = 0;  // translucent, as buildGpuFrame produces

    QImage out;
    if (!gpu.renderFrame({layer}, {}, clear, noGrain, 0.0f) || !gpu.readback(out)) {
        std::printf("FAIL: render/readback: %s\n", gpu.lastError().c_str());
        return 1;
    }

    const size_t colors = distinctColors(out);
    std::printf("layered frame: distinct=%zu\n", colors);
    std::printf("  pixel(0,0)     =0x%08x\n", (unsigned)out.pixel(0, 0));
    std::printf("  pixel(960,540) =0x%08x\n", (unsigned)out.pixel(kOutW / 2, kOutH / 2));
    std::printf("  pixel(1919,1079)=0x%08x\n", (unsigned)out.pixel(kOutW - 1, kOutH - 1));
    std::printf("  clear would be  0x%08x\n",
                (unsigned)qRgba(179, 179, 179, 255));

    // Expected: the source texture is 2x the output, so the centre of the
    // output samples texture (1920,1080) = (192, 108) = (192,108,44).
    std::printf("  expected centre=0x%08x\n", (unsigned)qRgba(192, 108, 44, 255));

    const bool flat = (colors <= 4);
    std::printf("%s: %zu distinct colours%s\n",
                flat ? "CORRUPT (flat/clear-only)" : "OK (textured)",
                colors, flat ? " — layer did not draw" : "");

    if (flat) {
        return 1;
    }

    // readback() is known-good; the live client reads the exported DmaBuf
    // that blitIntoShared() writes instead. Compare the two at real size,
    // because that is the only path the desktop actually consumes.
    if (gpu.renderFrame({layer}, {}, clear, noGrain, 1.0f) && gpu.blitIntoShared()) {
        auto* p = static_cast<const unsigned char*>(
            mmap(nullptr, buf.size, PROT_READ, MAP_SHARED, buf.fd, 0));
        if (p != MAP_FAILED) {
            std::set<uint32_t> dm;
            for (uint32_t y = 0; y < kOutH; y += 2) {
                const auto* row = p + static_cast<size_t>(y) * buf.stride;
                for (uint32_t x = 0; x < kOutW; x += 2) {
                    const auto* px = row + static_cast<size_t>(x) * 4;
                    // dmabuf is B8G8R8A8; readback QImage is RGBA8888
                    dm.insert(uint32_t(px[2]) | (uint32_t(px[1]) << 8)
                             | (uint32_t(px[0]) << 16) | (0xFFu << 24));
                    if (dm.size() > 300000) break;
                }
                if (dm.size() > 300000) break;
            }
            std::printf("dmabuf after blit: distinct=%zu pixel(960,540)=0x%08x\n",
                        dm.size(),
                        (unsigned)(uint32_t(p[(size_t)(kOutH/2)*buf.stride + (kOutW/2)*4 + 2])
                                 | (uint32_t(p[(size_t)(kOutH/2)*buf.stride + (kOutW/2)*4 + 1]) << 8)
                                 | (uint32_t(p[(size_t)(kOutH/2)*buf.stride + (kOutW/2)*4 + 0]) << 16)
                                 | (0xFFu << 24)));
            std::printf("  readback  centre=0x%08x\n", (unsigned)out.pixel(kOutW/2, kOutH/2));
            munmap(const_cast<unsigned char*>(p), buf.size);
        }
    }
    return 0;
}
