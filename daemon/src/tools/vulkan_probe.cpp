// Vulkan-path regression probe.
//
// Renders synthetic patterns through the daemon's real GPU path
// (VulkanContext::uploadSceneImage -> renderFrame -> dma-buf export) and
// verifies the exported buffer pixels, so CI stops certifying a path the
// daemon does not execute. Catches:
//   - C2: RGBA/BGRA channel swap on the GPU upload path
//   - C5: per-output copies truncated to the legacy buffer extent
//   - C1: staging-buffer use-after-free across upload/render cycles
// Usage: plasma-wallpaper-engine-vulkan-probe [--dump-dir DIR]

#include <QImage>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "vulkan/vulkan_context.h"

using WallpaperEngine::Render::DmaBufBuffer;
using WallpaperEngine::Render::VulkanContext;

namespace {

struct MappedBuffer {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

MappedBuffer mapBuffer(const DmaBufBuffer& buf) {
    MappedBuffer m;
    if (buf.fd < 0 || buf.size == 0) return m;
    void* p = mmap(nullptr, buf.size, PROT_READ, MAP_SHARED, buf.fd, 0);
    if (p == MAP_FAILED) return m;
    m.data = static_cast<const uint8_t*>(p);
    m.size = buf.size;
    return m;
}

void unmapBuffer(MappedBuffer& m) {
    if (m.data) munmap(const_cast<uint8_t*>(m.data), m.size);
    m.data = nullptr;
    m.size = 0;
}

// Exported buffers are DRM_FORMAT_ARGB8888: bytes B,G,R,A per pixel.
void pixelAt(const MappedBuffer& m, const DmaBufBuffer& buf, uint32_t x, uint32_t y, uint8_t out[4]) {
    const uint8_t* p = m.data + size_t(y) * buf.stride + size_t(x) * 4;
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
}

int failures = 0;

void expect(bool cond, const QString& what) {
    if (cond) {
        printf("PASS: %s\n", qPrintable(what));
    } else {
        printf("FAIL: %s\n", qPrintable(what));
        ++failures;
    }
}

// Every pixel of the exported buffer must match the expected BGRA color.
void checkSolid(const QString& name, const DmaBufBuffer& buf, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    MappedBuffer m = mapBuffer(buf);
    if (!m.data) {
        expect(false, name + ": mmap of exported dma-buf failed");
        return;
    }
    int bad = 0;
    uint32_t badX = 0, badY = 0;
    uint8_t got[4];
    for (uint32_t y = 0; y < buf.height && bad == 0; ++y) {
        for (uint32_t x = 0; x < buf.width; ++x) {
            pixelAt(m, buf, x, y, got);
            if (got[0] != b || got[1] != g || got[2] != r || got[3] != a) {
                bad = 1;
                badX = x;
                badY = y;
                break;
            }
        }
    }
    if (bad) {
        pixelAt(m, buf, badX, badY, got);
        printf("  first mismatch at (%u,%u): got BGRA=(%u,%u,%u,%u) want (%u,%u,%u,%u)\n",
               badX, badY, got[0], got[1], got[2], got[3], b, g, r, a);
    }
    expect(bad == 0, QString("%1: solid fill %2x%3").arg(name).arg(buf.width).arg(buf.height));
    unmapBuffer(m);
}

void dumpPng(const QString& dir, const QString& name, const DmaBufBuffer& buf) {
    if (dir.isEmpty()) return;
    MappedBuffer m = mapBuffer(buf);
    if (!m.data) return;
    QImage img(buf.width, buf.height, QImage::Format_ARGB32);
    for (uint32_t y = 0; y < buf.height; ++y) {
        const uint8_t* srcRow = m.data + size_t(y) * buf.stride;
        uint8_t* dstRow = img.scanLine(static_cast<int>(y));
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        for (uint32_t x = 0; x < buf.width * 4; x += 4) {
            dstRow[x] = srcRow[x + 3];
            dstRow[x + 1] = srcRow[x + 2];
            dstRow[x + 2] = srcRow[x + 1];
            dstRow[x + 3] = srcRow[x];
        }
#else
        std::memcpy(dstRow, srcRow, size_t(buf.width) * 4);
#endif
    }
    img.save(dir + "/" + name + ".png");
    unmapBuffer(m);
}

std::vector<uint8_t> makeSolid(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = 255;
    }
    return px;
}

} // namespace

int main(int argc, char* argv[]) {
    QString dumpDir;
    QStringList args;
    for (int i = 1; i < argc; ++i) args << argv[i];
    const int dumpIdx = args.indexOf("--dump-dir");
    if (dumpIdx >= 0 && dumpIdx + 1 < args.size()) dumpDir = args.at(dumpIdx + 1);

    VulkanContext ctx;
    if (!ctx.init()) {
        printf("FAIL: VulkanContext::init (no usable Vulkan device)\n");
        return 1;
    }

    // Legacy buffer plus two per-output targets; probe-b is taller than the
    // legacy buffer so a legacy-extent copy (C5) leaves its lower half stale.
    DmaBufBuffer legacy;
    if (!ctx.setResolution(320, 240, legacy)) {
        printf("FAIL: setResolution(320x240)\n");
        return 1;
    }
    DmaBufBuffer outA, outB;
    expect(ctx.setResolutionForOutput("probe-a", 320, 240, outA), "setResolutionForOutput probe-a 320x240");
    expect(ctx.setResolutionForOutput("probe-b", 200, 500, outB), "setResolutionForOutput probe-b 200x500");

    struct Target {
        QString name;
        const DmaBufBuffer* buf;
    };
    const std::vector<Target> targets = {
        {"legacy", &legacy},
        {"probe-a", &outA},
        {"probe-b", &outB},
    };

    // Frame 1: solid red -> channel order (C2) and full-extent fill (C5).
    auto red = makeSolid(320, 240, 255, 0, 0);
    expect(ctx.uploadSceneImage(320, 240, red), "uploadSceneImage red 320x240");
    ctx.renderFrame(0.0f);
    ctx.waitFrameFence();
    for (const auto& t : targets) {
        checkSolid(t.name + " red", *t.buf, 255, 0, 0, 255);
        dumpPng(dumpDir, t.name + "_red", *t.buf);
    }

    // Frames 2..9: alternate colors through repeated upload+render cycles.
    // Exercises staging destroy/recreate against pending submits (C1).
    for (int frame = 2; frame <= 9; ++frame) {
        const bool blue = (frame % 2 == 0);
        auto px = makeSolid(320, 240, blue ? 0 : 255, 0, blue ? 255 : 0);
        if (!ctx.uploadSceneImage(320, 240, px)) {
            expect(false, QString("uploadSceneImage cycle frame %1").arg(frame));
            break;
        }
        ctx.renderFrame(float(frame));
        ctx.waitFrameFence();
    }
    for (const auto& t : targets) {
        checkSolid(t.name + " post-cycle", *t.buf, 255, 0, 0, 255);
    }

    // Frame 10: vertical gradient catches row-order/stride corruption.
    std::vector<uint8_t> grad(size_t(320) * 240 * 4);
    for (uint32_t y = 0; y < 240; ++y) {
        const uint8_t v = uint8_t((y * 255) / 239);
        for (uint32_t x = 0; x < 320; ++x) {
            uint8_t* p = grad.data() + (size_t(y) * 320 + x) * 4;
            p[0] = v;
            p[1] = 0;
            p[2] = uint8_t(255 - v);
            p[3] = 255;
        }
    }
    expect(ctx.uploadSceneImage(320, 240, grad), "uploadSceneImage gradient 320x240");
    ctx.renderFrame(10.0f);
    ctx.waitFrameFence();
    for (const auto& t : targets) {
        MappedBuffer m = mapBuffer(*t.buf);
        if (!m.data) {
            expect(false, t.name + " gradient: mmap failed");
            continue;
        }
        uint8_t top[4], bottom[4];
        pixelAt(m, *t.buf, t.buf->width / 2, 0, top);
        pixelAt(m, *t.buf, t.buf->width / 2, t.buf->height - 1, bottom);
        // Row 0 is the blue end of the gradient (v=0), last row the red end.
        const bool ok = top[0] > 200 && top[2] < 55 && bottom[2] > 200 && bottom[0] < 55;
        if (!ok) {
            printf("  top BGRA=(%u,%u,%u) bottom BGRA=(%u,%u,%u)\n",
                   top[0], top[1], top[2], bottom[0], bottom[1], bottom[2]);
        }
        expect(ok, t.name + " gradient orientation");
        dumpPng(dumpDir, t.name + "_gradient", *t.buf);
        unmapBuffer(m);
    }

    printf(failures == 0 ? "ALL PROBES PASSED\n" : "%d PROBE(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
