#include "assets/dxt_decoder.h"
#include "assets/tex_parser.h"
#include <cassert>
#include <cstring>
#include <iostream>

using namespace WallpaperEngine::Assets;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::exit(1);
    }
    std::cout << "PASS: " << msg << std::endl;
}

static TexImage makeDxt(double w, double h, TextureFormat fmt, std::vector<uint8_t> data) {
    TexImage img;
    img.format = fmt;
    img.width = static_cast<uint32_t>(w);
    img.height = static_cast<uint32_t>(h);
    Mipmap mip;
    mip.width = static_cast<uint32_t>(w);
    mip.height = static_cast<uint32_t>(h);
    mip.data = std::move(data);
    img.mipmaps.push_back(std::move(mip));
    return img;
}

int main() {
    // 1. Solid-color DXT1 block: all indices 0 -> every pixel is c0 (opaque).
    {
        uint16_t c0 = (31 << 11) | (63 << 5) | 31; // white in 565
        uint16_t c1 = 0;                          // black
        uint8_t block[8];
        block[0] = c0 & 0xFF; block[1] = c0 >> 8;
        block[2] = c1 & 0xFF; block[3] = c1 >> 8;
        for (int i = 4; i < 8; ++i) block[i] = 0; // all index 0

        auto img = makeDxt(4, 4, TextureFormat::DXT1,
                            std::vector<uint8_t>(block, block + 8));
        auto rgba = DxtDecoder::decodeToRgba(img, 0);
        check(rgba.size() == 4 * 4 * 4, "DXT1 output size == 4*4*4");
        const uint8_t* p = rgba.data();
        for (int i = 0; i < 16; ++i) {
            check(p[i * 4 + 0] == 255 && p[i * 4 + 1] == 255 &&
                  p[i * 4 + 2] == 255 && p[i * 4 + 3] == 255,
                  "DXT1 all-index-0 solid white opaque");
        }
    }

    // 2. DXT1 4-color mode transparency: c0 < c1 yields a transparent color 3.
    {
        uint16_t c0 = 0x0000;       // black (index 0)
        uint16_t c1 = 0xF800;       // red-ish, so c0 < c1 -> 4-color mode
        uint8_t block[8];
        block[0] = c0 & 0xFF; block[1] = c0 >> 8;
        block[2] = c1 & 0xFF; block[3] = c1 >> 8;
        // every index = 3 -> color[3] = transparent black
        block[4] = 0xFF; block[5] = 0xFF; block[6] = 0xFF; block[7] = 0xFF;

        auto img = makeDxt(4, 4, TextureFormat::DXT1,
                            std::vector<uint8_t>(block, block + 8));
        auto rgba = DxtDecoder::decodeToRgba(img, 0);
        check(rgba.size() == 64, "DXT1 transparent output size == 64");
        const uint8_t* p = rgba.data();
        for (int i = 0; i < 16; ++i) {
            check(p[i * 4 + 3] == 0, "DXT1 c0<c1 index-3 pixel alpha == 0");
        }
    }

    // 3. DXT3 explicit 4-bit alpha overrides DXT1 alpha.
    {
        uint16_t c0 = (31 << 11) | (63 << 5) | 31; // white
        uint16_t c1 = 0;
        uint8_t block[16] = {0};
        // alpha is in block[0..7]; color block follows at block[8..15]
        block[8] = c0 & 0xFF; block[9] = c0 >> 8;
        block[10] = c1 & 0xFF; block[11] = c1 >> 8;
        // color indices stay 0
        // alpha row 0 = 0x1111 -> each pixel alpha = 1*17 = 17
        block[0] = 0x11; block[1] = 0x11;
        // remaining rows 0

        auto img = makeDxt(4, 4, TextureFormat::DXT3,
                            std::vector<uint8_t>(block, block + 16));
        auto rgba = DxtDecoder::decodeToRgba(img, 0);
        check(rgba.size() == 64, "DXT3 output size == 64");
        const uint8_t* p = rgba.data();
        // top row (py=0): alpha = 17
        for (int x = 0; x < 4; ++x) {
            check(p[x * 4 + 3] == 17, "DXT3 top-row alpha == 17");
        }
        // bottom rows: alpha 0
        for (int py = 1; py < 4; ++py) {
            for (int x = 0; x < 4; ++x) {
                check(p[(py * 4 + x) * 4 + 3] == 0, "DXT3 non-top-row alpha == 0");
            }
        }
    }

    // 4. DXT5 interpolated alpha: a0=255, a1=0 -> gradient midpoint 127 at index 4.
    {
        uint8_t block[16] = {0};
        uint16_t c0 = (31 << 11) | (63 << 5) | 31; // white
        uint16_t c1 = 0;
        block[8] = c0 & 0xFF; block[9] = c0 >> 8;
        block[10] = c1 & 0xFF; block[11] = c1 >> 8;

        block[0] = 255; // a0
        block[1] = 0;   // a1
        // alpha index bits: choose index 3 -> (4*255 + 3*0)/7 = 145? use index 4 midpoint
        // We'll set all 3-bit groups to index 4 -> ((7-3)*255 + 3*0)/7 = 4*255/7
        // Wait: alphaTable[i+1] for i=3 -> ((7-3)*a0 + 3*a1)/7 = 4*255/7 = 145.
        uint64_t bits = 0;
        for (int i = 0; i < 16; ++i) {
            bits |= static_cast<uint64_t>(4) << (i * 3); // index 3 -> alphaTable[4]
        }
        // alphaTable[4] is alphaTable[3+1] -> ((7-3)*255+3*0)/7 = 145
        for (int i = 0; i < 6; ++i) {
            block[2 + i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
        }

        auto img = makeDxt(4, 4, TextureFormat::DXT5,
                            std::vector<uint8_t>(block, block + 16));
        auto rgba = DxtDecoder::decodeToRgba(img, 0);
        check(rgba.size() == 64, "DXT5 output size == 64");
        const uint8_t* p = rgba.data();
        for (int i = 0; i < 16; ++i) {
            check(p[i * 4 + 3] == 145, "DXT5 interpolated alpha == 145");
        }
    }

    // 5. Auto-detect: Unknown format + DXT1-sized data decodes as DXT1.
    {
        uint16_t c0 = (31 << 11) | (63 << 5) | 31;
        uint16_t c1 = 0;
        uint8_t block[8];
        block[0] = c0 & 0xFF; block[1] = c0 >> 8;
        block[2] = c1 & 0xFF; block[3] = c1 >> 8;
        for (int i = 4; i < 8; ++i) block[i] = 0;

        auto img = makeDxt(4, 4, TextureFormat::Unknown,
                            std::vector<uint8_t>(block, block + 8));
        auto rgba = DxtDecoder::decodeToRgba(img, 0);
        check(rgba.size() == 64, "auto-detect DXT1 output size == 64");
        const uint8_t* p = rgba.data();
        check(p[3] == 255 && p[0] == 255, "auto-detect DXT1 solid white opaque");
    }

    std::cout << "dxt_decoder_test: all tests passed" << std::endl;
    return 0;
}
