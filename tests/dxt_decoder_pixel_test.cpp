#include "assets/dxt_decoder.h"
#include "assets/tex_parser.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace WallpaperEngine::Assets;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::exit(1);
    }
    std::cout << "PASS: " << msg << std::endl;
}

static TexImage makeImage(uint32_t w, uint32_t h, TextureFormat fmt, std::vector<uint8_t> data) {
    TexImage img;
    img.format = fmt;
    img.width = w;
    img.height = h;
    Mipmap mip;
    mip.width = w;
    mip.height = h;
    mip.data = std::move(data);
    img.mipmaps.push_back(std::move(mip));
    return img;
}

static bool pixelIs(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t x, uint32_t y,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t* p = rgba.data() + (y * w + x) * 4;
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

int main() {
    // 1. DXT1 solid colour: c0 = red 565, c1 = green 565 (c0 > c1 -> 4-opaque-colour
    // mode), every 2-bit index 0 -> every pixel decodes to c0 = (255,0,0,255).
    {
        uint8_t block[8] = {0x00, 0xF8, 0xE0, 0x07, 0x00, 0x00, 0x00, 0x00};
        auto img = makeImage(4, 4, TextureFormat::DXT1, std::vector<uint8_t>(block, block + 8));
        auto rgba = DxtDecoder::decodeToRgba(img, 0);
        check(rgba.size() == 4 * 4 * 4, "DXT1 solid colour: 4x4 output is 64 bytes");
        bool allRed = true;
        for (uint32_t y = 0; y < 4 && allRed; ++y)
            for (uint32_t x = 0; x < 4; ++x)
                allRed = pixelIs(rgba, 4, x, y, 255, 0, 0, 255);
        check(allRed, "DXT1 solid colour: all 16 pixels == (255,0,0,255)");
        check(pixelIs(rgba, 4, 3, 3, 255, 0, 0, 255), "DXT1 solid colour: bottom-right pixel red");
    }

    // 2. DXT1 4-colour mode transparency: c0 = black, c1 = red (c0 < c1) makes the
    // palette {c0, c1, avg, transparent}; indices 0,1,2,3 on the first row must
    // decode to opaque black, opaque red, opaque 50% grey-red, and (0,0,0,0).
    {
        uint8_t block[8] = {0x00, 0x00, 0x00, 0xF8, 0xE4, 0x00, 0x00, 0x00};
        auto img = makeImage(4, 4, TextureFormat::DXT1, std::vector<uint8_t>(block, block + 8));
        auto rgba = DxtDecoder::decodeToRgba(img, 0);
        check(rgba.size() == 4 * 4 * 4, "DXT1 transparency: 4x4 output is 64 bytes");
        check(pixelIs(rgba, 4, 0, 0, 0, 0, 0, 255), "DXT1 transparency: index 0 -> (0,0,0,255)");
        check(pixelIs(rgba, 4, 1, 0, 255, 0, 0, 255), "DXT1 transparency: index 1 -> (255,0,0,255)");
        check(pixelIs(rgba, 4, 2, 0, 127, 0, 0, 255), "DXT1 transparency: index 2 -> avg (127,0,0,255)");
        check(pixelIs(rgba, 4, 3, 0, 0, 0, 0, 0), "DXT1 transparency: index 3 -> (0,0,0,0)");
    }

    // 3. DXT3 4-bit explicit alpha: row 0 nibbles 0x0,0xF,0x5,0xA -> alphas
    // 0, 255, 85, 170 (nibble * 17) over an opaque red colour block.
    {
        uint8_t block[16] = {
            0xF0, 0xA5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // alpha rows
            0x00, 0xF8, 0xE0, 0x07, 0x00, 0x00, 0x00, 0x00  // DXT1 colour: red, index 0
        };
        auto img = makeImage(4, 4, TextureFormat::DXT3, std::vector<uint8_t>(block, block + 16));
        auto rgba = DxtDecoder::decodeToRgba(img, 0);
        check(rgba.size() == 4 * 4 * 4, "DXT3 alpha: 4x4 output is 64 bytes");
        check(pixelIs(rgba, 4, 0, 0, 255, 0, 0, 0), "DXT3 alpha: nibble 0x0 -> alpha 0");
        check(pixelIs(rgba, 4, 1, 0, 255, 0, 0, 255), "DXT3 alpha: nibble 0xF -> alpha 255");
        check(pixelIs(rgba, 4, 2, 0, 255, 0, 0, 85), "DXT3 alpha: nibble 0x5 -> alpha 85");
        check(pixelIs(rgba, 4, 3, 0, 255, 0, 0, 170), "DXT3 alpha: nibble 0xA -> alpha 170");
        check(pixelIs(rgba, 4, 0, 1, 255, 0, 0, 0), "DXT3 alpha: row 1 nibble 0x0 -> alpha 0");
    }

    // 4. DXT5 interpolated alpha: a0=200, a1=100 (a0 > a1) interpolates over 7;
    // indices 0..3 on row 0 -> 200, 100, 185 ((6*200+1*100)/7), 171 ((5*200+2*100)/7)
    // over an opaque blue colour block.
    {
        uint8_t block[16] = {
            200, 100, 0x88, 0x06, 0x00, 0x00, 0x00, 0x00, // a0, a1, 48-bit index table
            0x1F, 0x00, 0xE0, 0x07, 0x00, 0x00, 0x00, 0x00 // DXT1 colour: blue, index 0
        };
        auto img = makeImage(4, 4, TextureFormat::DXT5, std::vector<uint8_t>(block, block + 16));
        auto rgba = DxtDecoder::decodeToRgba(img, 0);
        check(rgba.size() == 4 * 4 * 4, "DXT5 alpha: 4x4 output is 64 bytes");
        check(pixelIs(rgba, 4, 0, 0, 0, 0, 255, 200), "DXT5 alpha: index 0 -> alpha 200");
        check(pixelIs(rgba, 4, 1, 0, 0, 0, 255, 100), "DXT5 alpha: index 1 -> alpha 100");
        check(pixelIs(rgba, 4, 2, 0, 0, 0, 255, 185), "DXT5 alpha: index 2 -> alpha 185");
        check(pixelIs(rgba, 4, 3, 0, 0, 0, 255, 171), "DXT5 alpha: index 3 -> alpha 171");
    }

    // 5. Edge case: 6x2 image (neither dimension fills a 4x4 block). Two DXT1
    // blocks wide, one tall; block 0 is red, block 1 is blue. Pixels must land at
    // y * width + x across the partial block, and only x < 6 / y < 2 are copied.
    {
        uint8_t blocks[16] = {
            0x00, 0xF8, 0xE0, 0x07, 0x00, 0x00, 0x00, 0x00, // block 0: red, index 0
            0x1F, 0x00, 0xE0, 0x07, 0x00, 0x00, 0x00, 0x00  // block 1: blue, index 0
        };
        auto img = makeImage(6, 2, TextureFormat::DXT1, std::vector<uint8_t>(blocks, blocks + 16));
        auto rgba = DxtDecoder::decodeToRgba(img, 0);
        check(rgba.size() == 6 * 2 * 4, "partial block 6x2: output is 48 bytes");
        check(pixelIs(rgba, 6, 0, 0, 255, 0, 0, 255), "partial block 6x2: (0,0) red from block 0");
        check(pixelIs(rgba, 6, 3, 0, 255, 0, 0, 255), "partial block 6x2: (3,0) red at block 0 edge");
        check(pixelIs(rgba, 6, 4, 0, 0, 0, 255, 255), "partial block 6x2: (4,0) blue at block 1 start");
        check(pixelIs(rgba, 6, 5, 1, 0, 0, 255, 255), "partial block 6x2: (5,1) blue at clipped corner");
        check(pixelIs(rgba, 6, 0, 1, 255, 0, 0, 255), "partial block 6x2: (0,1) red on row stride");
    }

    std::cout << "dxt_decoder_pixel_test: all assertions passed" << std::endl;
    return 0;
}
