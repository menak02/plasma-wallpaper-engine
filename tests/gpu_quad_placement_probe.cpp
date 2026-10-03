// Headless GPU quad-placement regression test.
//
// The other two GPU probes (gpu_deform_probe, blit_shared_probe) each cover
// ONE layer set: the deform probe puts two full-screen quads at the same
// center, the blit probe puts a single full-screen quad. Both therefore pass
// even when per-instance data is fed to the vertex stage incorrectly -- if
// every quad lands on the same pixel or collapses, a full-screen scene still
// looks correct. The live wallpaper failure is exactly the case neither
// covers: a scene with SEVERAL quads at DISTINCT screen positions, where the
// exported frame degenerates to a single flat color.
//
// This probe renders N textured quads at KNOWN, DISTINCT screen positions and
// asserts that each quad's pixels land in its own expected screen rect, and
// that nothing bleeds into the gaps between them. Distinct solid-color
// textures make a misplaced or collapsed instance stream unambiguous: each
// rect has a unique expected color, and the clear color shows through
// everywhere else.
//
// It also covers three neighbours of that property that regress silently:
//   * a full-screen textured BACKGROUND plus overlays (the real scene shape),
//     asserting the frame is not a constant color;
//   * a mesh-deformed layer interleaved with plain layers (two instance
//     streams in one command buffer, instance-stream rebinding);
//   * a second frame with MORE layers than the first, which forces the vertex
//     buffer to be reallocated -- the corner strip must be rewritten after
//     every reallocation or every quad collapses to a point;
//   * the descriptor pool growing past 64 sets while a texture slot from an
//     earlier scene is still in use -- the live flat-color failure, where the
//     pool rebuild orphaned every pre-existing descriptor and every layer that
//     reused a known layer id sampled garbage;
//   * a reused layer id whose layer image changed dimensions (a VkImage is
//     immutable, so the slot needs a new image, not a re-upload);
//   * init() twice on one compositor, which must not leave a destroyed vertex
//     buffer handle and a dangling mapping behind.
//
// Exits 77 when no Vulkan device is available so GPU-less CI still passes.
// Run headless with QT_QPA_PLATFORM=offscreen (set here).

#include "../daemon/src/render/gpu_quad_compositor.h"
#include "../daemon/src/vulkan/vulkan_context.h"

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QSet>

#include <cstdint>
#include <cstdio>
#include <vector>

using WallpaperEngine::Render::GpuGrainParams;
using WallpaperEngine::Render::GpuLayer;
using WallpaperEngine::Render::GpuParticle;
using WallpaperEngine::Render::GpuQuadCompositor;
using WallpaperEngine::Render::VulkanContext;

namespace {

// Clear color used everywhere: an unmistakable magenta so any quad that bleeds
// outside its rect, or fails to draw at all, is visible as a wrong color.
constexpr float kClear[4] = {0.5f, 0.0f, 0.5f, 1.0f};

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Solid-color texture. Solid (not a gradient) so LINEAR filtering is a no-op
// and interior samples land on the exact expected byte value.
QImage makeSolid(uint8_t r, uint8_t g, uint8_t b) {
    QImage img(8, 8, QImage::Format_RGBA8888);
    img.fill(QColor(r, g, b));
    return img;
}

// Non-solid texture: a per-texel pattern with >100 distinct byte values. A
// full-screen quad covering this must NOT read back as one flat color.
QImage makePattern(uint32_t size) {
    QImage img(int(size), int(size), QImage::Format_RGBA8888);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            img.setPixel(int(x), int(y),
                         qRgba(uint8_t((x * 7 + 11) & 0xFF),
                               uint8_t((y * 5 + 37) & 0xFF),
                               uint8_t((x * y) & 0xFF),
                               255));
        }
    }
    return img;
}

struct Rect {
    float cx, cy, w, h;   // center + size in screen px
    QRgb expected;        // color the whole rect interior must be
};

Rect makeRect(float cx, float cy, float w, float h, QRgb expected) {
    return Rect{cx, cy, w, h, expected};
}

GpuLayer makeLayer(uint32_t tex, const Rect& r, uint32_t blendMode = 0, bool deformed = false) {
    GpuLayer l;
    l.textureIndex = tex;
    l.centerX = r.cx;
    l.centerY = r.cy;
    l.width = r.w;
    l.height = r.h;
    l.rotationRad = 0.0f;
    l.opacity = 1.0f;
    l.blendMode = blendMode;
    l.deformed = deformed;
    // deformStrength 0 would collapse the grid onto the undeformed quad and
    // hide placement bugs; use a real displacement.
    l.deformSpeed = 1.0f;
    l.deformStrength = deformed ? 1.0f : 0.0f;
    l.deformDirection = 0.0f;
    return l;
}

bool nearColor(QRgb got, QRgb want, int tol = 6) {
    return std::abs(qRed(got) - qRed(want)) <= tol &&
           std::abs(qGreen(got) - qGreen(want)) <= tol &&
           std::abs(qBlue(got) - qBlue(want)) <= tol &&
           std::abs(qAlpha(got) - qAlpha(want)) <= tol;
}

// Sample the middle of the quad and four inset corners: all must be the
// quad's own color. Inset by 3px so LINEAR filtering at the edges is ignored.
bool rectInteriorMatches(const QImage& c, const Rect& r) {
    const int x0 = int(r.cx - r.w / 2.0f);
    const int x1 = int(r.cx + r.w / 2.0f);
    const int y0 = int(r.cy - r.h / 2.0f);
    const int y1 = int(r.cy + r.h / 2.0f);
    const int inset = 3;
    if (x0 + inset >= x1 - inset || y0 + inset >= y1 - inset) return false;
    const QRgb samples[] = {
        c.pixel(int(r.cx), int(r.cy)),
        c.pixel(x0 + inset, y0 + inset),
        c.pixel(x1 - 1 - inset, y0 + inset),
        c.pixel(x0 + inset, y1 - 1 - inset),
        c.pixel(x1 - 1 - inset, y1 - 1 - inset),
    };
    for (QRgb s : samples) {
        if (!nearColor(s, r.expected)) return false;
    }
    return true;
}

// The clear color must show through in a ring around each quad, proving the
// quads are the right size and are not smeared across the frame.
bool surroundingsAreClear(const QImage& c, const Rect& r, QRgb clearPixel, int margin = 6) {
    const int x0 = int(r.cx - r.w / 2.0f) - margin;
    const int x1 = int(r.cx + r.w / 2.0f) + margin;
    const int y0 = int(r.cy - r.h / 2.0f) - margin;
    const int y1 = int(r.cy + r.h / 2.0f) + margin;
    if (x0 < 0 || y0 < 0 || x1 >= c.width() || y1 >= c.height()) return false;
    // Ring: walk the margin frame.
    for (int x = x0; x <= x1; ++x) {
        if (!nearColor(c.pixel(x, y0), clearPixel)) return false;
        if (!nearColor(c.pixel(x, y1), clearPixel)) return false;
    }
    for (int y = y0; y <= y1; ++y) {
        if (!nearColor(c.pixel(x0, y), clearPixel)) return false;
        if (!nearColor(c.pixel(x1, y), clearPixel)) return false;
    }
    return true;
}

size_t distinctColors(const QImage& c) {
    QSet<QRgb> s;
    for (int y = 0; y < c.height(); y += 2)
        for (int x = 0; x < c.width(); x += 2) s.insert(c.pixel(x, y));
    return size_t(s.size());
}

void reportFirstMismatch(const QImage& c, const Rect& r, const char* what) {
    const int x0 = int(r.cx - r.w / 2.0f);
    const int x1 = int(r.cx + r.w / 2.0f);
    const int y0 = int(r.cy - r.h / 2.0f);
    const int y1 = int(r.cy + r.h / 2.0f);
    std::printf("      %s: rect(%d,%d)-(%d,%d) expect %08X, center got %08X,"
                " top-left-inset got %08X\n",
                what, x0, y0, x1, y1, uint(r.expected),
                uint(c.pixel(int(r.cx), int(r.cy))),
                uint(c.pixel(x0 + 3, y0 + 3)));
}

// ---------- Case 1: distinct positions, distinct textures ----------
void caseDistinctPlacement(GpuQuadCompositor& gpu) {
    const uint32_t W = 512, H = 384;
    std::puts("--- case 1: 6 quads at distinct positions, distinct textures ---");

    struct Spec { float cx, cy; QRgb color; };
    const Spec specs[] = {
        { 96.f,  96.f, qRgba(255,   0,   0, 255) },
        {416.f,  96.f, qRgba(  0, 255,   0, 255) },
        { 96.f, 288.f, qRgba(  0,   0, 255, 255) },
        {416.f, 288.f, qRgba(255, 255,   0, 255) },
        {256.f, 192.f, qRgba(  0, 255, 255, 255) },
        {256.f,  64.f, qRgba(255,   0, 255, 255) },
    };

    std::vector<Rect> rects;
    std::vector<GpuLayer> layers;
    int slot = 10;
    for (const Spec& s : specs) {
        const uint8_t r = uint8_t(qRed(s.color)), g = uint8_t(qGreen(s.color)), b = uint8_t(qBlue(s.color));
        const uint32_t tex = gpu.getOrCreateTexture(makeSolid(r, g, b), slot++);
        if (tex == UINT32_MAX) {
            std::printf("FAIL: texture upload (slot %d)\n", slot - 1);
            ++g_failures;
            return;
        }
        rects.push_back(makeRect(s.cx, s.cy, 64.f, 64.f, s.color));
        layers.push_back(makeLayer(tex, rects.back()));
    }

    QImage c;
    if (!gpu.renderFrame(layers, {}, kClear, GpuGrainParams{}, 0.0f) || !gpu.readback(c)) {
        std::printf("FAIL: renderFrame/readback: %s\n", gpu.lastError().c_str());
        ++g_failures;
        return;
    }

    const QRgb clearPixel = c.pixel(4, 4);
    check(nearColor(clearPixel, qRgba(128, 0, 128, 255), 4),
          "clear color fills the untouched corner");
    if (!nearColor(clearPixel, qRgba(128, 0, 128, 255), 4))
        std::printf("      corner pixel = %08X\n", uint(clearPixel));

    for (size_t i = 0; i < rects.size(); ++i) {
        const bool inside = rectInteriorMatches(c, rects[i]);
        char label[96];
        std::snprintf(label, sizeof(label), "quad %zu lands in its own screen rect (color %08X)", i, uint(rects[i].expected));
        check(inside, label);
        if (!inside) reportFirstMismatch(c, rects[i], "quad");

        std::snprintf(label, sizeof(label), "quad %zu surroundings stay clear-colored", i);
        const bool clear = surroundingsAreClear(c, rects[i], clearPixel);
        check(clear, label);
        if (!clear) reportFirstMismatch(c, rects[i], "surroundings");
    }
    std::printf("      distinct colors in frame: %zu\n", distinctColors(c));
}

// ---------- Case 2: full-screen textured background + overlays ----------
// This is the shape of the live wallpaper that degenerated to a flat fill.
void caseFullScreenBackground(GpuQuadCompositor& gpu) {
    const uint32_t W = 1920, H = 1080;
    std::puts("--- case 2: 1920x1080 full-screen patterned background + 4 overlays ---");

    const uint32_t bgTex = gpu.getOrCreateTexture(makePattern(256), 20);
    if (bgTex == UINT32_MAX) {
        std::puts("FAIL: background texture upload");
        ++g_failures;
        return;
    }
    const Rect bg{W / 2.0f, H / 2.0f, float(W), float(H), 0};
    std::vector<GpuLayer> layers;
    layers.push_back(makeLayer(bgTex, bg));

    // Overlays at known positions, drawn back-to-front, each a solid color so
    // their placement is unambiguous on top of the pattern.
    struct Spec { float cx, cy; QRgb color; };
    const Spec specs[] = {
        {480.f, 270.f, qRgba(255, 0, 0, 255) },
        {1440.f, 270.f, qRgba(0, 255, 0, 255) },
        {480.f, 810.f, qRgba(0, 0, 255, 255) },
        {1440.f, 810.f, qRgba(255, 255, 255, 255) },
    };
    std::vector<Rect> overlays;
    int slot = 21;
    for (const Spec& s : specs) {
        const uint32_t tex = gpu.getOrCreateTexture(
            makeSolid(uint8_t(qRed(s.color)), uint8_t(qGreen(s.color)), uint8_t(qBlue(s.color))),
            slot++);
        if (tex == UINT32_MAX) {
            std::puts("FAIL: overlay texture upload");
            ++g_failures;
            return;
        }
        overlays.push_back(makeRect(s.cx, s.cy, 200.f, 120.f, s.color));
        layers.push_back(makeLayer(tex, overlays.back()));
    }

    QImage c;
    if (!gpu.renderFrame(layers, {}, kClear, GpuGrainParams{}, 0.0f) || !gpu.readback(c)) {
        std::printf("FAIL: renderFrame/readback: %s\n", gpu.lastError().c_str());
        ++g_failures;
        return;
    }
    if (c.width() != int(W) || c.height() != int(H)) {
        std::printf("FAIL: canvas is %dx%d, expected %ux%u\n", c.width(), c.height(), W, H);
        ++g_failures;
        return;
    }

    const size_t colors = distinctColors(c);
    std::printf("      distinct colors in frame: %zu\n", colors);
    // The live bug: the whole 1920x1080 buffer was ONE constant color.
    check(colors > 16, "full-screen textured background is NOT a flat constant color");

    // Background must be visible in the gaps between the overlays.
    const QRgb gap = c.pixel(W / 2, 40);
    std::printf("      background sample at (%u,40) = %08X\n", W / 2, uint(gap));
    check(!nearColor(gap, qRgba(128, 0, 128, 255), 4),
          "background texture is visible where no overlay covers it");

    for (size_t i = 0; i < overlays.size(); ++i) {
        char label[96];
        std::snprintf(label, sizeof(label), "overlay %zu lands in its own screen rect (color %08X)", i, uint(overlays[i].expected));
        const bool inside = rectInteriorMatches(c, overlays[i]);
        check(inside, label);
        if (!inside) reportFirstMismatch(c, overlays[i], "overlay");
    }
}

// ---------- Case 3: plain + deformed layers interleaved ----------
// Two instance streams share one vertex buffer; the stream binding must be
// switched between draws and the placement of the plain quads must survive.
void caseMixedDeform(GpuQuadCompositor& gpu) {
    const uint32_t W = 512, H = 384;
    std::puts("--- case 3: plain quad, deformed quad, plain quad ---");

    const uint32_t redTex  = gpu.getOrCreateTexture(makeSolid(255, 0, 0), 30);
    const uint32_t defTex  = gpu.getOrCreateTexture(makeSolid(0, 255, 0), 31);
    const uint32_t blueTex = gpu.getOrCreateTexture(makeSolid(0, 0, 255), 32);
    if (redTex == UINT32_MAX || defTex == UINT32_MAX || blueTex == UINT32_MAX) {
        std::puts("FAIL: texture upload");
        ++g_failures;
        return;
    }

    const Rect r1{128.f, 192.f, 96.f, 96.f, qRgba(255, 0, 0, 255)};
    const Rect r2{256.f, 192.f, 96.f, 96.f, qRgba(0, 255, 0, 255)};
    const Rect r3{384.f, 192.f, 96.f, 96.f, qRgba(0, 0, 255, 255)};
    std::vector<GpuLayer> layers;
    layers.push_back(makeLayer(redTex, r1));
    layers.push_back(makeLayer(defTex, r2, 0, /*deformed=*/true));
    layers.push_back(makeLayer(blueTex, r3));

    QImage c;
    if (!gpu.renderFrame(layers, {}, kClear, GpuGrainParams{}, 0.0f) || !gpu.readback(c)) {
        std::printf("FAIL: renderFrame/readback: %s\n", gpu.lastError().c_str());
        ++g_failures;
        return;
    }

    for (const Rect* r : {&r1, &r3}) {
        char label[96];
        std::snprintf(label, sizeof(label), "plain quad at x=%.0f survives the deform stream switch", r->cx);
        const bool inside = rectInteriorMatches(c, *r);
        check(inside, label);
        if (!inside) reportFirstMismatch(c, *r, "plain quad");
    }
    // The deformed quad is displaced by up to +-12px, so only assert it covers
    // a wide band around its center (its exact edges are deformed by design).
    const QRgb ctr = c.pixel(256, 192);
    check(nearColor(ctr, qRgba(0, 255, 0, 255), 10),
          "deformed quad covers its center");
    if (!nearColor(ctr, qRgba(0, 255, 0, 255), 10))
        std::printf("      deformed center = %08X\n", uint(ctr));
}

// ---------- Case 4: vertex-buffer reallocation between frames ----------
// The second frame needs more instance bytes than the first, so the vertex
// buffer is reallocated. The corner strip lives in that buffer and must be
// rewritten after every reallocation; if it is not, binding 0 reads zeros and
// every quad collapses to a point (frame renders as a flat clear color).
void caseVertexGrowth(GpuQuadCompositor& gpu) {
    const uint32_t W = 640, H = 480;
    std::puts("--- case 4: second frame grows the instance stream (vertex realloc) ---");

    // Frame A: one small quad.
    const uint32_t teal = gpu.getOrCreateTexture(makeSolid(0, 255, 255), 40);
    // Frame B: one deformed layer = 256 * 48 bytes of instances, forcing the
    // buffer to grow well past frame A's footprint.
    const uint32_t green = gpu.getOrCreateTexture(makeSolid(0, 255, 0), 41);
    const uint32_t red = gpu.getOrCreateTexture(makeSolid(255, 0, 0), 42);
    if (teal == UINT32_MAX || green == UINT32_MAX || red == UINT32_MAX) {
        std::puts("FAIL: texture upload");
        ++g_failures;
        return;
    }

    const Rect a{320.f, 240.f, 128.f, 128.f, qRgba(0, 255, 255, 255)};
    QImage cA;
    if (!gpu.renderFrame({makeLayer(teal, a)}, {}, kClear, GpuGrainParams{}, 0.0f) || !gpu.readback(cA)) {
        std::printf("FAIL: frame A renderFrame/readback: %s\n", gpu.lastError().c_str());
        ++g_failures;
        return;
    }
    check(rectInteriorMatches(cA, a), "frame A quad lands in its screen rect");
    if (!rectInteriorMatches(cA, a)) reportFirstMismatch(cA, a, "frame A");

    // Frame B: much larger instance stream -> ensureVertexCapacity reallocates.
    const Rect b{320.f, 240.f, 400.f, 300.f, qRgba(0, 255, 0, 255)};
    QImage cB;
    if (!gpu.renderFrame({makeLayer(green, b, 0, /*deformed=*/true)},
                         {}, kClear, GpuGrainParams{}, 0.0f) || !gpu.readback(cB)) {
        std::printf("FAIL: frame B renderFrame/readback: %s\n", gpu.lastError().c_str());
        ++g_failures;
        return;
    }
    const QRgb bctr = cB.pixel(320, 240);
    std::printf("      frame B center = %08X\n", uint(bctr));
    check(nearColor(bctr, qRgba(0, 255, 0, 255), 12),
          "frame B deformed quad covers its center AFTER vertex buffer realloc");
}

// ---------- Case 5: descriptor pool growth across a scene switch ----------
// THE LIVE FAILURE. The compositor's texture cache is per-SceneCompositor and
// keyed by layer id, so it survives scene loads: the daemon restores one
// wallpaper at construction, the client asks for another, and every switch
// adds more slots. Once the cache outgrows its descriptor pool the pool is
// torn down and reallocated. That must not orphan the descriptors of the
// textures that already exist -- and in the live daemon it did, so every layer
// that reused an already-known layer id sampled an unwritten descriptor set.
// The whole exported frame degenerated to a single flat color while
// buildGpuFrame() still reported a full, correct layer list, which is why the
// defect was invisible to every other probe.
void casePoolGrowthKeepsDescriptors(GpuQuadCompositor& gpu) {
    const uint32_t W = 512, kH_ = 384;
    std::puts("--- case 5: descriptor pool grows, pre-existing slots stay bound ---");

    // A layer the "previous scene" left behind: slot 77, full screen.
    const Rect full{W / 2.0f, kH_ / 2.0f, float(W), float(kH_), 0};
    const uint32_t early = gpu.getOrCreateTexture(makePattern(64), 77);
    if (early == UINT32_MAX) { std::puts("FAIL: early texture"); ++g_failures; return; }
    QImage before;
    if (!gpu.renderFrame({makeLayer(early, full)}, {}, kClear, GpuGrainParams{}, 0.0f) ||
        !gpu.readback(before)) {
        std::puts("FAIL: pre-growth frame");
        ++g_failures;
        return;
    }
    const size_t beforeColors = distinctColors(before);
    std::printf("      before pool growth: distinct colors = %zu\n", beforeColors);
    check(beforeColors > 16, "slot 77 renders textured content before pool growth");

    // Now push the cache well past its 64-set pool so the pool is reallocated.
    // These stand in for the layer ids of every wallpaper the daemon has shown
    // since it started.
    int ok = 1;
    for (int i = 0; i < 80 && ok; ++i) {
        if (gpu.getOrCreateTexture(makeSolid(uint8_t(i * 3), 40, 90), 1000 + i) == UINT32_MAX)
            ok = 0;
    }
    check(ok, "80 additional textures uploaded (descriptor pool forced to grow)");

    // The "next scene" reuses layer id 77 with different content. getOrCreateTexture
    // re-uploads in place; the descriptor set backing that texture index must
    // still point at it.
    const uint32_t again = gpu.getOrCreateTexture(makePattern(64), 77);
    check(again != UINT32_MAX, "re-used layer id re-uploads successfully");
    if (again == UINT32_MAX) return;
    check(again == early, "re-used layer id keeps its texture index");

    QImage after;
    if (!gpu.renderFrame({makeLayer(again, full)}, {}, kClear, GpuGrainParams{}, 1.0f) ||
        !gpu.readback(after)) {
        std::puts("FAIL: post-growth frame");
        ++g_failures;
        return;
    }
    const size_t afterColors = distinctColors(after);
    std::printf("      after pool growth:  distinct colors = %zu, pixel(0,0)=%08X\n",
                afterColors, uint(after.pixel(0, 0)));
    // The defect: the descriptor for this slot was orphaned by the pool
    // rebuild, so the quad sampled garbage and the frame came out as one flat
    // color instead of the pattern.
    check(afterColors > 16,
          "re-used layer id still renders textured content AFTER the pool grew");
    check(nearColor(after.pixel(0, 0), before.pixel(0, 0), 12),
          "re-used layer id still samples the same texture as before the pool grew");
}

// ---------- Case 6: a reused layer id with different image dimensions ----------
// Layer ids are per-scene values that get reused across scenes, so the image
// behind a slot can change size. A VkImage is immutable once created, so the
// slot needs a new image rather than a re-upload: copying a small image into
// the old (large) extent would read past the end of a shrinking staging buffer
// and leave the rest of the texture holding the previous scene's pixels.
void caseSlotResize(GpuQuadCompositor& gpu) {
    const uint32_t W = 512, H = 384;
    std::puts("--- case 6: reused layer id whose layer image changed size ---");

    // "Previous scene": slot 500 is a 256x256 texture with a per-texel pattern
    // (thousands of distinct colors once magnified).
    const uint32_t big = gpu.getOrCreateTexture(makePattern(256), 500);
    if (big == UINT32_MAX) { std::puts("FAIL: large texture"); ++g_failures; return; }
    QImage bigFrame;
    if (!gpu.renderFrame({makeLayer(big, makeRect(W / 2.f, H / 2.f, float(W), float(H), 0))},
                         {}, kClear, GpuGrainParams{}, 0.0f) || !gpu.readback(bigFrame)) {
        std::puts("FAIL: large frame");
        ++g_failures;
        return;
    }
    const size_t bigColors = distinctColors(bigFrame);
    std::printf("      256x256 slot rendered %zu distinct colors\n", bigColors);
    check(bigColors > 16, "256x256 layer renders textured content");

    // "Next scene": same layer id, now an 8x8 SOLID yellow texture. Solid and
    // maximally different from the pattern, so the assertion is unambiguous:
    // if the slot still samples the old VkImage, the quad stays patterned; if
    // it samples a copy of the old image into the old extent, the frame stays
    // patterned; only a correctly resized image makes the frame solid yellow.
    const uint32_t small = gpu.getOrCreateTexture(makeSolid(255, 255, 0), 500);
    check(small != UINT32_MAX, "resized layer re-uploads successfully");
    if (small == UINT32_MAX) return;

    QImage smallFrame;
    if (!gpu.renderFrame({makeLayer(small, makeRect(W / 2.f, H / 2.f, float(W), float(H), 0))},
                         {}, kClear, GpuGrainParams{}, 1.0f) || !gpu.readback(smallFrame)) {
        std::puts("FAIL: small frame");
        ++g_failures;
        return;
    }
    std::printf("      8x8 slot rendered %zu distinct colors, pixel(0,0)=%08X (was %08X)\n",
                distinctColors(smallFrame), uint(smallFrame.pixel(0, 0)), uint(bigFrame.pixel(0, 0)));
    check(rectInteriorMatches(smallFrame,
                              makeRect(W / 2.f, H / 2.f, float(W), float(H), qRgba(255, 255, 0, 255))),
          "reused slot renders the NEW texture across the whole quad");
    check(!nearColor(smallFrame.pixel(0, 0), bigFrame.pixel(0, 0), 4),
          "reused slot does not still sample the old texture");
}

// ---------- Case 7: init() called twice on the same compositor ----------
// cleanup() has to leave no device state behind. If the vertex buffer handle,
// its mapped pointer or its capacity survive, the first frame after the second
// init() binds a destroyed VkBuffer through a dangling mapping and the frame
// comes out as the bare clear color.
void caseReinit(VulkanContext& ctx) {
    const uint32_t W = 512, H = 384;
    std::puts("--- case 7: compositor re-init leaves no stale device state ---");

    GpuQuadCompositor gpu;
    if (!gpu.init(&ctx, W, H)) {
        std::printf("FAIL: first gpu init: %s\n", gpu.lastError().c_str());
        ++g_failures;
        return;
    }
    // Second init() on the same object: this is what cleanup() + re-init does.
    if (!gpu.init(&ctx, W, H)) {
        std::printf("FAIL: second gpu init: %s\n", gpu.lastError().c_str());
        ++g_failures;
        return;
    }

    const uint32_t tex = gpu.getOrCreateTexture(makePattern(64), 900);
    if (tex == UINT32_MAX) { std::puts("FAIL: texture after re-init"); ++g_failures; return; }
    QImage c;
    if (!gpu.renderFrame({makeLayer(tex, makeRect(W / 2.f, H / 2.f, float(W), float(H), 0))},
                         {}, kClear, GpuGrainParams{}, 0.0f) || !gpu.readback(c)) {
        std::puts("FAIL: renderFrame/readback after re-init");
        ++g_failures;
        return;
    }
    const size_t colors = distinctColors(c);
    std::printf("      distinct colors after re-init: %zu\n", colors);
    check(colors > 16, "textured frame renders after re-init (no stale vertex buffer)");
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
    if (!gpu.init(&ctx, 512, 384)) {
        std::printf("FAIL: gpu init: %s\n", gpu.lastError().c_str());
        return 1;
    }

    caseDistinctPlacement(gpu);

    // Case 2 needs a 1920x1080 target, matching the live wallpaper and the
    // resolution the flat-color bug was observed at.
    gpu.setResolution(1920, 1080);
    caseFullScreenBackground(gpu);

    gpu.setResolution(512, 384);
    caseMixedDeform(gpu);
    caseVertexGrowth(gpu);
    casePoolGrowthKeepsDescriptors(gpu);
    caseSlotResize(gpu);
    caseReinit(ctx);

    gpu.cleanup();
    ctx.cleanup();

    if (g_failures) {
        std::printf("gpu_quad_placement_probe: %d assertion(s) FAILED\n", g_failures);
        return 1;
    }
    std::puts("gpu_quad_placement_probe: all assertions passed");
    return 0;
}
