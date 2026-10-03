// Headless GPU deform-pipeline regression test.
//
// Renders an offscreen scene through GpuQuadCompositor with a mixed layer
// set — one plain quad followed by one mesh-deformed quad — and validates
// the composited pixels against the CPU MeshDeformer::deformVertices math.
//
// Observable: a bright/dark texel boundary at texture v = 0.5 (screen row
// H/2 when undeformed). Mesh deformation displaces interior grid rows by
// offset = sin(phase + u*2pi) * strength * 12 * sin(v*pi); at the center
// (u=v=0.5) with strength 1 the boundary shifts by -12px at the chosen
// time. The probe asserts:
//   plain path  -> boundary at H/2 (self-calibration of the measurement),
//   deform path -> boundary matches the CPU-deformed grid row.
// It also renders one particle and one solid-texture quad to lock in the
// color channel order, Y orientation, and particle glow path.
//
// Historical regressions covered (all previously silent):
// 1. vkCmdBindVertexBuffers was passed a single VkBuffer* for a 2-binding
//    bind: binding 1 (instances) read stack garbage -> zero instances ->
//    every quad collapsed to an invisible point;
// 2. the corner strip was never written into the (re)allocated vertex
//    buffer, so binding 0 read zeros after every reallocation;
// 3. plain/deform layers share one vertex buffer across two instance
//    streams — the stream binding must be switched between draws and both
//    loops (packing and drawing) must skip the same layers;
// 4. vertex shaders used GL-style Y flip, wrong for Vulkan's Y-down NDC.
//
// Probe exits with code 77 when no Vulkan device is available so GPU-less
// CI still passes. Run headless with QT_QPA_PLATFORM=offscreen (set here).

#include "../daemon/src/render/gpu_quad_compositor.h"
#include "../daemon/src/render/mesh_renderer.h"
#include "../daemon/src/vulkan/vulkan_context.h"

#include <QGuiApplication>
#include <QImage>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using WallpaperEngine::Render::GpuGrainParams;
using WallpaperEngine::Render::GpuLayer;
using WallpaperEngine::Render::GpuParticle;
using WallpaperEngine::Render::GpuQuadCompositor;
using WallpaperEngine::Render::MeshDeformer;
using WallpaperEngine::Render::VulkanContext;

namespace {

constexpr uint32_t kW = 256, kH = 256;
constexpr uint32_t kTexSize = 64;

// GREEN texture, top texel half bright, bottom half dark: a crisp
// horizontal boundary at texture v = 0.5, detectable on the G channel.
QImage makeBoundaryTexture() {
    QImage img(kTexSize, kTexSize, QImage::Format_RGBA8888);
    for (uint32_t y = 0; y < kTexSize; ++y) {
        const uint8_t v = (y < kTexSize / 2) ? 220 : 40;
        for (uint32_t x = 0; x < kTexSize; ++x)
            img.setPixel(x, y, qRgba(0, v, 0, 255));
    }
    return img;
}

QImage makeSolid(uint8_t r, uint8_t g, uint8_t b) {
    QImage img(8, 8, QImage::Format_RGBA8888);
    // QColor overload is format-aware; the uint overload writes the packed
    // value raw and scrambles channels on RGBA8888.
    img.fill(QColor(r, g, b));
    return img;
}

// Screen row (center column) where bright turns dark: first row with
// G <= 128 after at least one row with G > 128. -1 if not found.
float measureBoundary(const QImage& canvas) {
    const int cx = canvas.width() / 2;
    bool seenBright = false;
    for (int y = 0; y < canvas.height(); ++y) {
        const int g = qGreen(canvas.pixel(cx, y));
        if (g > 128) seenBright = true;
        else if (seenBright) return static_cast<float>(y);
    }
    return -1.0f;
}

// CPU expectation: displaced screen row of the grid vertex at (u=v=0.5).
// generateDefaultGrid places v=0.5 at local y=0; deformVertices adds
// sin(direction)*offset; the layer center sits at screen (W/2, H/2).
float expectedBoundaryRow(float time, float speed, float strength, float direction) {
    std::vector<WallpaperEngine::Render::MeshVertex> verts;
    std::vector<WallpaperEngine::Render::MeshTriangle> idx;
    MeshDeformer::generateDefaultGrid(16, 16, float(kW), float(kH), verts, idx);
    MeshDeformer::deformVertices(verts, time, speed, strength, direction);
    for (const auto& v : verts) {
        if (std::fabs(v.position.x()) < 1e-4f && std::fabs(v.uv.y() - 0.5f) < 1e-4f)
            return v.position.y() + float(kH) / 2.0f;
    }
    return -1.0f;
}

struct FrameResult {
    QImage canvas;
    bool rendered = false, readBack = false;
};

FrameResult renderProbe(GpuQuadCompositor& gpu, uint32_t redTex, uint32_t boundaryTex,
                        bool deformed, float time, float speed, float strength, float direction) {
    const float clearColor[4] = {0.f, 0.f, 0.f, 1.f};
    std::vector<GpuLayer> layers;

    GpuLayer bg; // plain red background, full screen
    bg.textureIndex = redTex;
    bg.centerX = kW / 2.f; bg.centerY = kH / 2.f;
    bg.width = float(kW);  bg.height = float(kH);
    layers.push_back(bg);

    GpuLayer fg; // green boundary layer
    fg.textureIndex = boundaryTex;
    fg.centerX = kW / 2.f; fg.centerY = kH / 2.f;
    fg.width = float(kW);  fg.height = float(kH);
    fg.deformed = deformed;
    fg.deformSpeed = speed;
    fg.deformStrength = deformed ? strength : 0.0f;
    fg.deformDirection = direction;
    layers.push_back(fg);

    FrameResult out;
    out.rendered = gpu.renderFrame(layers, {}, clearColor, GpuGrainParams{}, time);
    out.readBack = out.rendered && gpu.readback(out.canvas);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    VulkanContext ctx;
    if (!ctx.init()) {
        std::puts("SKIP: no Vulkan device available");
        return 77;
    }

    GpuQuadCompositor gpu;
    if (!gpu.init(&ctx, kW, kH)) {
        std::printf("FAIL: gpu init: %s\n", gpu.lastError().c_str());
        return 1;
    }

    const uint32_t redTex = gpu.getOrCreateTexture(makeSolid(255, 0, 0), 1);
    const uint32_t boundaryTex = gpu.getOrCreateTexture(makeBoundaryTexture(), 2);
    if (redTex == UINT32_MAX || boundaryTex == UINT32_MAX) {
        std::puts("FAIL: texture upload");
        return 1;
    }

    // Channel-order lock: a solid-red texture must read back red (R at byte
    // 0 of RGBA8888). Catches any future R8G8B8A8/B8G8R8A8 mixup in the
    // upload path.
    {
        const float black[4] = {0.f, 0.f, 0.f, 1.f};
        std::vector<GpuLayer> redOnly;
        GpuLayer bg;
        bg.textureIndex = redTex;
        bg.centerX = kW / 2.f; bg.centerY = kH / 2.f;
        bg.width = float(kW);  bg.height = float(kH);
        redOnly.push_back(bg);
        QImage c;
        if (!gpu.renderFrame(redOnly, {}, black, GpuGrainParams{}, 0.0f) || !gpu.readback(c)) {
            std::printf("FAIL: red-only render/readback: %s\n", gpu.lastError().c_str());
            return 1;
        }
        const QRgb tl = c.pixel(0, 0);
        if (qRed(tl) < 250 || qGreen(tl) != 0 || qBlue(tl) != 0) {
            std::printf("FAIL: channel order wrong, red texel = %08X\n", uint(tl));
            return 1;
        }
    }

    // Particle lock: one white particle at center must be bright.
    {
        const float black[4] = {0.f, 0.f, 0.f, 1.f};
        std::vector<GpuParticle> parts;
        GpuParticle p;
        p.centerX = kW / 2.f; p.centerY = kH / 2.f; p.size = 64.f;
        p.r = p.g = p.b = p.a = 1.f;
        parts.push_back(p);
        QImage c;
        if (!gpu.renderFrame({}, parts, black, GpuGrainParams{}, 0.0f) || !gpu.readback(c)) {
            std::printf("FAIL: particle render/readback: %s\n", gpu.lastError().c_str());
            return 1;
        }
        const QRgb ctr = c.pixel(kW / 2, kH / 2);
        if (qGreen(ctr) < 200) {
            std::printf("FAIL: particle not drawn, center = %08X\n", uint(ctr));
            return 1;
        }
    }

    // Deform params: phase = time*speed*2.5 = pi/2 -> sin(phase + pi) = -1,
    // strength 1 -> offset = -12px along +Y (direction = pi/2).
    const float time = 3.14159265f / 2.0f / 2.5f; // 0.62832
    const float speed = 1.0f;
    const float strength = 1.0f;
    const float direction = 3.14159265f / 2.0f;

    // --- Sanity: plain path boundary must sit at H/2 = 128 ---
    FrameResult plain = renderProbe(gpu, redTex, boundaryTex, /*deformed=*/false,
                                    time, speed, strength, direction);
    if (!plain.readBack) {
        std::printf("FAIL: plain renderFrame/readback: %s\n", gpu.lastError().c_str());
        return 1;
    }
    const float plainRow = measureBoundary(plain.canvas);
    if (plainRow < 0 || std::fabs(plainRow - float(kH) / 2.0f) > 3.0f) {
        std::printf("FAIL: plain boundary at %.1f, expected %.1f\n",
                    plainRow, float(kH) / 2.0f);
        return 1;
    }

    // --- Deformed path must match the CPU grid math ---
    FrameResult deform = renderProbe(gpu, redTex, boundaryTex, /*deformed=*/true,
                                     time, speed, strength, direction);
    if (!deform.readBack) {
        std::printf("FAIL: deform renderFrame/readback: %s\n", gpu.lastError().c_str());
        return 1;
    }
    const float deformRow = measureBoundary(deform.canvas);
    const float expectedRow = expectedBoundaryRow(time, speed, strength, direction);

    gpu.cleanup();
    ctx.cleanup();

    if (deformRow < 0 || expectedRow < 0) {
        std::puts("FAIL: could not measure deformed boundary");
        return 1;
    }
    if (std::fabs(deformRow - expectedRow) > 6.0f) {
        std::printf("FAIL: GPU deform row %.1f != CPU math %.1f (diff %.1f > 6)\n",
                    deformRow, expectedRow, std::fabs(deformRow - expectedRow));
        return 1;
    }
    std::printf("PASS: GPU deform matches CPU math (plain %.1f, deform %.1f, expected %.1f)\n",
                plainRow, deformRow, expectedRow);
    return 0;
}
