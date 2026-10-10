#include "dxt_decoder.h"
#include <cstring>
#include <iostream>
#include <cmath>
#include <QImage>

namespace WallpaperEngine::Assets {

// Convert IEEE 754 half-float (16-bit) to float
static inline float halfFloatToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exponent = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;

    if (exponent == 0) {
        // Denormalized
        float val = std::ldexp(static_cast<float>(mantissa), -24);
        return sign ? -val : val;
    } else if (exponent == 31) {
        // Inf/NaN
        if (mantissa == 0) {
            return sign ? -INFINITY : INFINITY;
        }
        return NAN;
    } else {
        float val = std::ldexp(static_cast<float>(mantissa) + 1024.0f, static_cast<int>(exponent) - 25);
        return sign ? -val : val;
    }
}

// Convert half-float to 8-bit [0,255]
static inline uint8_t halfToUint8(uint16_t h) {
    float f = halfFloatToFloat(h);
    if (std::isnan(f) || f < 0.0f) return 0;
    if (f > 1.0f) return 255;
    return static_cast<uint8_t>(f * 255.0f + 0.5f);
}

static inline uint32_t packColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8)  |
           (static_cast<uint32_t>(r));
}

static inline void decode565(uint16_t color, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = ((color >> 11) & 0x1F) * 255 / 31;
    g = ((color >> 5)  & 0x3F) * 255 / 63;
    b = (color & 0x1F) * 255 / 31;
}

void DxtDecoder::decodeDxt1Block(const uint8_t* block, uint32_t* outPixels, uint32_t stride) {
    uint16_t c0 = block[0] | (block[1] << 8);
    uint16_t c1 = block[2] | (block[3] << 8);

    uint8_t r[4], g[4], b[4], a[4];
    decode565(c0, r[0], g[0], b[0]); a[0] = 255;
    decode565(c1, r[1], g[1], b[1]); a[1] = 255;

    if (c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3;
        g[2] = (2 * g[0] + g[1]) / 3;
        b[2] = (2 * b[0] + b[1]) / 3;
        a[2] = 255;

        r[3] = (r[0] + 2 * r[1]) / 3;
        g[3] = (g[0] + 2 * g[1]) / 3;
        b[3] = (b[0] + 2 * b[1]) / 3;
        a[3] = 255;
    } else {
        r[2] = (r[0] + r[1]) / 2;
        g[2] = (g[0] + g[1]) / 2;
        b[2] = (b[0] + b[1]) / 2;
        a[2] = 255;

        r[3] = 0; g[3] = 0; b[3] = 0; a[3] = 0;
    }

    uint32_t colors[4] = {
        packColor(r[0], g[0], b[0], a[0]),
        packColor(r[1], g[1], b[1], a[1]),
        packColor(r[2], g[2], b[2], a[2]),
        packColor(r[3], g[3], b[3], a[3])
    };

    uint32_t table = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint8_t idx = (table >> ((y * 4 + x) * 2)) & 0x03;
            outPixels[y * stride + x] = colors[idx];
        }
    }
}

void DxtDecoder::decodeDxt3Block(const uint8_t* block, uint32_t* outPixels, uint32_t stride) {
    decodeDxt1Block(block + 8, outPixels, stride);

    for (int y = 0; y < 4; ++y) {
        uint16_t alphaRow = block[y * 2] | (block[y * 2 + 1] << 8);
        for (int x = 0; x < 4; ++x) {
            uint8_t alpha = ((alphaRow >> (x * 4)) & 0x0F) * 17;
            uint32_t color = outPixels[y * stride + x] & 0x00FFFFFF;
            outPixels[y * stride + x] = color | (static_cast<uint32_t>(alpha) << 24);
        }
    }
}

void DxtDecoder::decodeDxt5Block(const uint8_t* block, uint32_t* outPixels, uint32_t stride) {
    decodeDxt1Block(block + 8, outPixels, stride);

    uint8_t a0 = block[0];
    uint8_t a1 = block[1];
    uint8_t alphaTable[8];

    alphaTable[0] = a0;
    alphaTable[1] = a1;

    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i) {
            alphaTable[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        }
    } else {
        for (int i = 1; i <= 4; ++i) {
            alphaTable[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        }
        alphaTable[6] = 0;
        alphaTable[7] = 255;
    }

    uint64_t alphaBits = 0;
    for (int i = 0; i < 6; ++i) {
        alphaBits |= static_cast<uint64_t>(block[2 + i]) << (i * 8);
    }

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint8_t alphaIdx = (alphaBits >> ((y * 4 + x) * 3)) & 0x07;
            uint8_t alpha = alphaTable[alphaIdx];
            uint32_t color = outPixels[y * stride + x] & 0x00FFFFFF;
            outPixels[y * stride + x] = color | (static_cast<uint32_t>(alpha) << 24);
        }
    }
}

std::vector<uint8_t> DxtDecoder::decodeToRgba(const TexImage& image, size_t mipmapIndex) {
    if (mipmapIndex >= image.mipmaps.size()) return {};

    const auto& mip = image.mipmaps[mipmapIndex];
    uint32_t width = mip.width;
    uint32_t height = mip.height;

    if (width == 0 || height == 0 || mip.data.empty()) return {};

    // Crafted .tex headers can claim arbitrary dimensions; width * height * 4
    // wraps in 32-bit math, allocating a tiny buffer that the decode loop then
    // writes a full image into. Reject anything beyond 128M pixels outright.
    if (static_cast<uint64_t>(width) * height > (1ull << 27)) return {};

    // Detect MP4/video data: ftyp box at offset 4 (ISO base media file format)
    if (mip.data.size() >= 8) {
        const uint8_t* d = mip.data.data();
        if (d[4] == 'f' && d[5] == 't' && d[6] == 'y' && d[7] == 'p') {
            std::cerr << "DxtDecoder: MP4/video texture detected ("
                      << width << "x" << height << ") — video playback not supported, skipping" << std::endl;
            return {};
        }
    }

    std::vector<uint8_t> outRgba(static_cast<size_t>(width) * height * 4);
    uint32_t* outPixels = reinterpret_cast<uint32_t*>(outRgba.data());

    // Compute DXT block sizes for format detection
    uint32_t blockWidth = (width + 3) / 4;
    uint32_t blockHeight = (height + 3) / 4;
    size_t expectedDxt1Size = static_cast<size_t>(blockWidth) * blockHeight * 8;
    size_t expectedDxt3Size = static_cast<size_t>(blockWidth) * blockHeight * 16;
    size_t expectedDxt5Size = expectedDxt3Size;

    // Determine DXT format from header or size-based auto-detection
    bool isDxt1 = (image.format == TextureFormat::DXT1);
    bool isDxt3 = (image.format == TextureFormat::DXT3);
    bool isDxt5 = (image.format == TextureFormat::DXT5);

    if (!isDxt1 && !isDxt3 && !isDxt5) {
        // Unknown format: auto-detect from data size
        // CRITICAL: Check DXT sizes BEFORE uncompressed sizes to avoid false R8/RGB matches
        if (mip.data.size() == expectedDxt1Size) {
            isDxt1 = true;
        } else if (mip.data.size() == expectedDxt5Size) {
            // DXT3 and DXT5 have the same compressed size; default to DXT5 (more common)
            isDxt5 = true;
        }
    }

    // 1. DXT Block Decoding (highest priority — compressed formats have unique sizes)
    if (isDxt1 || isDxt3 || isDxt5) {
        const uint8_t* srcData = mip.data.data();
        size_t srcOffset = 0;

        for (uint32_t by = 0; by < blockHeight; ++by) {
            for (uint32_t bx = 0; bx < blockWidth; ++bx) {
                uint32_t blockPixels[16] = {0};

                if (isDxt1) {
                    if (srcOffset + 8 > mip.data.size()) break;
                    decodeDxt1Block(srcData + srcOffset, blockPixels, 4);
                    srcOffset += 8;
                } else if (isDxt3) {
                    if (srcOffset + 16 > mip.data.size()) break;
                    decodeDxt3Block(srcData + srcOffset, blockPixels, 4);
                    srcOffset += 16;
                } else if (isDxt5) {
                    if (srcOffset + 16 > mip.data.size()) break;
                    decodeDxt5Block(srcData + srcOffset, blockPixels, 4);
                    srcOffset += 16;
                }

                for (uint32_t py = 0; py < 4; ++py) {
                    uint32_t y = by * 4 + py;
                    if (y >= height) continue;

                    for (uint32_t px = 0; px < 4; ++px) {
                        uint32_t x = bx * 4 + px;
                        if (x >= width) continue;

                        outPixels[y * width + x] = blockPixels[py * 4 + px];
                    }
                }
            }
        }
        return outRgba;
    }

    // 2. Explicit format matches (header says what it is)
    if (image.format == TextureFormat::ARGB8888 || image.format == TextureFormat::RGBA1010102) {
        if (mip.data.size() >= outRgba.size()) {
            std::memcpy(outRgba.data(), mip.data.data(), outRgba.size());
            return outRgba;
        }
    }

    if (image.format == TextureFormat::RGB888) {
        if (mip.data.size() >= width * height * 3) {
            const uint8_t* src = mip.data.data();
            for (uint32_t i = 0; i < width * height; ++i) {
                outPixels[i] = packColor(src[i * 3], src[i * 3 + 1], src[i * 3 + 2], 255);
            }
            return outRgba;
        }
    }

    if (image.format == TextureFormat::R8) {
        if (mip.data.size() >= width * height) {
            const uint8_t* src = mip.data.data();
            for (uint32_t i = 0; i < width * height; ++i) {
                outPixels[i] = packColor(src[i], src[i], src[i], 255);
            }
            return outRgba;
        }
    }

    if (image.format == TextureFormat::RG88) {
        if (mip.data.size() >= width * height * 2) {
            const uint8_t* src = mip.data.data();
            for (uint32_t i = 0; i < width * height; ++i) {
                outPixels[i] = packColor(src[i * 2], src[i * 2 + 1], 0, 255);
            }
            return outRgba;
        }
    }

    if (image.format == TextureFormat::RGB565) {
        if (mip.data.size() >= width * height * 2) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(mip.data.data());
            for (uint32_t i = 0; i < width * height; ++i) {
                uint16_t c = src[i];
                uint8_t r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
                uint8_t g = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
                uint8_t b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
                outPixels[i] = packColor(r, g, b, 255);
            }
            return outRgba;
        }
    }

    // Half-float formats (16-bit float per channel)
    if (image.format == TextureFormat::RG1616F) {
        // 2 channels: R=half, G=half → RGBA8888
        if (mip.data.size() >= width * height * 4) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(mip.data.data());
            for (uint32_t i = 0; i < width * height; ++i) {
                uint8_t r = halfToUint8(src[i * 2]);
                uint8_t g = halfToUint8(src[i * 2 + 1]);
                outPixels[i] = packColor(r, g, g, 255);
            }
            return outRgba;
        }
    }

    if (image.format == TextureFormat::R16F) {
        // 1 channel: R=half → RGBA8888
        if (mip.data.size() >= width * height * 2) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(mip.data.data());
            for (uint32_t i = 0; i < width * height; ++i) {
                uint8_t r = halfToUint8(src[i]);
                outPixels[i] = packColor(r, r, r, 255);
            }
            return outRgba;
        }
    }

    if (image.format == TextureFormat::RGB161616F) {
        // 3 channels: R=half, G=half, B=half → RGBA8888
        if (mip.data.size() >= width * height * 6) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(mip.data.data());
            for (uint32_t i = 0; i < width * height; ++i) {
                uint8_t r = halfToUint8(src[i * 3]);
                uint8_t g = halfToUint8(src[i * 3 + 1]);
                uint8_t b = halfToUint8(src[i * 3 + 2]);
                outPixels[i] = packColor(r, g, b, 255);
            }
            return outRgba;
        }
    }

    if (image.format == TextureFormat::RGBA16161616F) {
        // 4 channels: R=half, G=half, B=half, A=half → RGBA8888
        if (mip.data.size() >= width * height * 8) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(mip.data.data());
            for (uint32_t i = 0; i < width * height; ++i) {
                uint8_t r = halfToUint8(src[i * 4]);
                uint8_t g = halfToUint8(src[i * 4 + 1]);
                uint8_t b = halfToUint8(src[i * 4 + 2]);
                uint8_t a = halfToUint8(src[i * 4 + 3]);
                outPixels[i] = packColor(r, g, b, a);
            }
            return outRgba;
        }
    }

    // 3. Unknown format: try embedded image (JPEG/PNG)
    if (mip.data.size() > 4) {
        QImage directImg;
        if (directImg.loadFromData(mip.data.data(), static_cast<int>(mip.data.size()))) {
            directImg = directImg.convertToFormat(QImage::Format_RGBA8888);
            if (static_cast<uint32_t>(directImg.width()) == width && static_cast<uint32_t>(directImg.height()) == height) {
                std::memcpy(outRgba.data(), directImg.constBits(), outRgba.size());
                return outRgba;
            } else {
                std::vector<uint8_t> scaledRgba(directImg.sizeInBytes());
                std::memcpy(scaledRgba.data(), directImg.constBits(), directImg.sizeInBytes());
                return scaledRgba;
            }
        }
    }

    // 4. Unknown format: size-based uncompressed auto-detection
    // Only reach here if DXT didn't match and format is Unknown
    if (mip.data.size() == outRgba.size()) {
        // Exact 4 bytes/pixel match — treat as ARGB/RGBA
        std::memcpy(outRgba.data(), mip.data.data(), outRgba.size());
        return outRgba;
    }
    if (mip.data.size() == width * height * 3) {
        const uint8_t* src = mip.data.data();
        for (uint32_t i = 0; i < width * height; ++i) {
            outPixels[i] = packColor(src[i * 3], src[i * 3 + 1], src[i * 3 + 2], 255);
        }
        return outRgba;
    }
    if (mip.data.size() == width * height * 2) {
        const uint16_t* src = reinterpret_cast<const uint16_t*>(mip.data.data());
        for (uint32_t i = 0; i < width * height; ++i) {
            uint16_t c = src[i];
            uint8_t r = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
            uint8_t g = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
            uint8_t b = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
            outPixels[i] = packColor(r, g, b, 255);
        }
        return outRgba;
    }
    if (mip.data.size() == width * height * 1) {
        const uint8_t* src = mip.data.data();
        for (uint32_t i = 0; i < width * height; ++i) {
            outPixels[i] = packColor(src[i], src[i], src[i], 255);
        }
        return outRgba;
    }

    // 5. Last resort: if data is exactly outRgba.size(), just copy
    if (mip.data.size() >= outRgba.size()) {
        std::memcpy(outRgba.data(), mip.data.data(), outRgba.size());
        return outRgba;
    }

    return outRgba;
}

} // namespace WallpaperEngine::Assets
