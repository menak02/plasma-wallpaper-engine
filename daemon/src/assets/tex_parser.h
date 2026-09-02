#pragma once

#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <span>
#include <QCache>

namespace WallpaperEngine::Assets {

// Format codes match the actual Wallpaper Engine binary format.
// Verified against Almamu's reference implementation (linux-wallpaperengine).
enum class TextureFormat : uint32_t {
    ARGB8888 = 0,
    RGB888 = 1,
    RGB565 = 2,
    // 3 = unused
    DXT5 = 4,
    // 5 = unused
    DXT3 = 6,
    DXT1 = 7,
    RG88 = 8,
    R8 = 9,
    RG1616F = 10,
    R16F = 11,
    BC7 = 12,
    RGBA1010102 = 13,
    RGBA16161616F = 14,
    RGB161616F = 15,
    Unknown = 0xFFFFFFFF
};

struct Mipmap {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> data;
};

struct TexImage {
    TextureFormat format = TextureFormat::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    uint32_t flags = 0;
    std::vector<Mipmap> mipmaps;
};

class TexParser {
public:
    static bool parse(std::span<const uint8_t> bytes, TexImage& outImage);
};

} // namespace WallpaperEngine::Assets
