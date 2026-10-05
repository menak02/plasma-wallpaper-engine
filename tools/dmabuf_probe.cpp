// Diagnostic helper: read the live daemon's exported DmaBuf over D-Bus and
// report whether it contains a real rendered frame or a flat fill.
//
// This is the same read path the layerclient uses (mmap the exported fd),
// so it reflects exactly what reaches the screen. Not part of the build.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QVariantMap>
#include <QString>

#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <set>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const char* out = argc > 1 ? argv[1] : "DP-4";

    QDBusInterface iface("org.plasmawallpaperengine.Daemon", "/WallpaperEngine",
                         "org.plasmawallpaperengine.Daemon",
                         QDBusConnection::sessionBus());

    QDBusReply<QVariantMap> info =
        iface.call("getBufferInfoForOutput", QString::fromLatin1(out));
    if (!info.isValid()) {
        std::printf("info failed: %s\n", qPrintable(info.error().message()));
        return 1;
    }
    const QVariantMap m = info.value();
    const int w = m["width"].toInt();
    const int h = m["height"].toInt();
    const int stride = m["stride"].toInt();
    const qulonglong size = m["size"].toULongLong();
    std::printf("info: %dx%d stride=%d size=%llu\n", w, h, stride, size);

    QDBusReply<QDBusUnixFileDescriptor> fr =
        iface.call("getBufferFdForOutput", QString::fromLatin1(out));
    if (!fr.isValid()) {
        std::printf("fd call failed: %s\n", qPrintable(fr.error().message()));
        return 1;
    }
    const int fd = fr.value().fileDescriptor();
    std::printf("fd=%d\n", fd);
    if (fd < 0) {
        return 1;
    }

    void* p = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    const auto* b = static_cast<const unsigned char*>(p);

    std::printf("first 8 px (BGRA): ");
    for (int k = 0; k < 8; ++k) {
        std::printf("(%d,%d,%d,%d) ", b[k*4], b[k*4+1], b[k*4+2], b[k*4+3]);
    }
    std::printf("\n");

    // Distinct 32-bit pixels across the whole frame: the single number that
    // separates "rendered scene" from "flat fill".
    std::set<uint32_t> pixels;
    for (int y = 0; y < h; y += 2) {
        const auto* row = b + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; x += 2) {
            const auto* px = row + static_cast<size_t>(x) * 4;
            pixels.insert(uint32_t(px[0]) | (uint32_t(px[1]) << 8)
                          | (uint32_t(px[2]) << 16) | (uint32_t(px[3]) << 24));
            if (pixels.size() > 400000) break;
        }
        if (pixels.size() > 400000) break;
    }
    std::printf("distinct colors: %zu  -> %s\n", pixels.size(),
           pixels.size() <= 2 ? "FLAT FILL (bug)" : "has image content");

    for (int y = 0; y < h; y += h / 3) {
        const auto* row = b + static_cast<size_t>(y) * stride;
        const auto mid = static_cast<size_t>(stride / 2);
        std::printf("row %4d: (%d,%d,%d,%d)  mid:(%d,%d,%d,%d)\n", y,
               row[0], row[1], row[2], row[3],
               row[mid], row[mid+1], row[mid+2], row[mid+3]);
    }

    munmap(p, size);
    return 0;
}
