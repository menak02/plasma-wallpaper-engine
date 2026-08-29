#pragma once

#include <vector>
#include <cstdint>
#include <span>
#include "tex_parser.h"

namespace WallpaperEngine::Assets {

class DxtDecoder {
public:
    // Decompresses a TexImage mipmap into standard 32-bit RGBA (4 bytes per pixel)
    static std::vector<uint8_t> decodeToRgba(const TexImage& image, size_t mipLevel = 0);

private:
    static void decodeDxt1Block(const uint8_t* block, uint32_t* outPixels, uint32_t stride);
    static void decodeDxt3Block(const uint8_t* block, uint32_t* outPixels, uint32_t stride);
    static void decodeDxt5Block(const uint8_t* block, uint32_t* outPixels, uint32_t stride);
};

} // namespace WallpaperEngine::Assets
