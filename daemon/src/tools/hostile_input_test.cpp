// Hostile-input safety test.
//
// Feeds the fixtures in tests/fixtures/hostile/ through the real parser
// entry points and asserts each one fails safely — no crash, no oversized
// allocation — per RED_TEAM_REPORT_2026-10-15.md P1. Run under ASan in CI
// to catch residual out-of-bounds access.
// Usage: plasma-wallpaper-engine-hostile-test <fixtures-dir>

#include <QImage>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "assets/dxt_decoder.h"
#include "assets/pkg_reader.h"
#include "assets/tex_parser.h"
#include "assets/video_decoder.h"

using namespace WallpaperEngine::Assets;

namespace {

int failures = 0;

void expect(bool cond, const std::string& what) {
    printf("%s: %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

std::vector<uint8_t> readFileBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

// C3: hostile mip dimensions must not wrap width*height*4 or overflow the
// output buffer; the decoder has to refuse instead.
void testOverflowTex(const std::filesystem::path& dir) {
    const auto bytes = readFileBytes(dir / "overflow-dims.tex");
    expect(!bytes.empty(), "overflow-dims.tex fixture present");

    TexImage img;
    const bool parsed = TexParser::parse(bytes, img);
    expect(parsed, "overflow-dims.tex parses to a TexImage");

    for (size_t i = 0; i < img.mipmaps.size(); ++i) {
        const std::vector<uint8_t> rgba = DxtDecoder::decodeToRgba(img, i);
        expect(rgba.empty(),
               "overflow-dims.tex decode refused (no wrap-around allocation)");
    }

    // Direct decoder check with a fabricated hostile mip.
    TexImage hostile;
    hostile.format = TextureFormat::DXT1;
    hostile.mipmaps.push_back(Mipmap{65536, 65536, std::vector<uint8_t>(16, 0)});
    expect(DxtDecoder::decodeToRgba(hostile, 0).empty(), "decoder rejects 65536x65536 DXT1 mip");

    TexImage huge;
    huge.format = TextureFormat::DXT1;
    huge.mipmaps.push_back(Mipmap{0xFFFFFFFF, 0xFFFFFFFF, std::vector<uint8_t>(16, 0)});
    expect(DxtDecoder::decodeToRgba(huge, 0).empty(), "decoder rejects 0xFFFFFFFF^2 DXT1 mip");
}

// H2: TEXB0001 uncompressed size must be capped like TEXB0003/0004.
void testUncappedTex(const std::filesystem::path& dir) {
    const auto bytes = readFileBytes(dir / "texb1-uncapped.tex");
    expect(!bytes.empty(), "texb1-uncapped.tex fixture present");

    TexImage img;
    const bool parsed = TexParser::parse(bytes, img);
    expect(!parsed || img.mipmaps.empty(),
           "texb1-uncapped.tex rejected (no 4 GiB allocation)");
}

// Raw-entry size mismatch: compSize > uncompSize must not overflow the mip
// buffer (heap write overflow before the fix).
void testTexSizeMismatch(const std::filesystem::path& dir) {
    const auto bytes = readFileBytes(dir / "texb1-size-mismatch.tex");
    expect(!bytes.empty(), "texb1-size-mismatch.tex fixture present");

    TexImage img;
    const bool parsed = TexParser::parse(bytes, img);
    expect(parsed, "texb1-size-mismatch.tex parses");
    bool sized = false;
    for (const auto& mip : img.mipmaps) {
        if (mip.data.size() <= 16) sized = true;
    }
    expect(sized, "mip payload clamped to declared size (no overflow)");
}

// PKG entry length/offset must be validated against the real file size.
void testPkg(const std::filesystem::path& dir) {
    PkgReader pkg;
    expect(pkg.open(dir / "huge-entry.pkg"), "huge-entry.pkg opens");

    expect(!pkg.hasFile("huge.bin"), "4 GiB entry dropped");
    expect(!pkg.hasFile("wrap.bin"), "wrapping offset+length entry dropped");
    expect(pkg.hasFile("small.txt"), "valid entry kept");

    expect(pkg.readFile("huge.bin").empty(), "readFile(huge.bin) returns empty");
    expect(pkg.readFile("wrap.bin").empty(), "readFile(wrap.bin) returns empty");
    const auto small = pkg.readFile("small.txt");
    expect(small.size() == 4 && std::string(small.begin(), small.end()) == "DATA",
           "readFile(small.txt) returns payload");
}

// H5: AVIO seeks outside the buffer must be refused, not wrapped into an
// out-of-bounds read.
void testSeekBomb(const std::filesystem::path& dir, const char* name) {
    const auto bytes = readFileBytes(dir / name);
    expect(!bytes.empty(), std::string(name) + " fixture present");

    VideoDecoder decoder;
    const bool opened = decoder.openFromData(bytes.data(), bytes.size(), 0, 0);
    if (opened) {
        const QImage frame = decoder.decodeNextFrame();
        expect(frame.isNull(), std::string(name) + " opened but yields no frame");
    } else {
        expect(true, std::string(name) + " rejected at open");
    }
}

} // namespace

int main(int argc, char* argv[]) {
    const std::filesystem::path dir = argc > 1 ? argv[1] : "tests/fixtures/hostile";
    if (!std::filesystem::exists(dir)) {
        printf("FAIL: fixtures dir not found: %s\n", dir.c_str());
        return 1;
    }

    testOverflowTex(dir);
    testUncappedTex(dir);
    testTexSizeMismatch(dir);
    testPkg(dir);
    testSeekBomb(dir, "seek-bomb-negative.mp4");
    testSeekBomb(dir, "seek-bomb-oversize.mp4");
    testSeekBomb(dir, "seek-bomb-stco.mp4");
    testSeekBomb(dir, "seek-bomb-co64.mp4");

    printf(failures == 0 ? "ALL HOSTILE FIXTURES HANDLED SAFELY\n" : "%d CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
