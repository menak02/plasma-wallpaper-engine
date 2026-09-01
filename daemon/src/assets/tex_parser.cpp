#include "tex_parser.h"
#include "dxt_decoder.h"
#include <cstring>
#include <iostream>
#include <lz4.h>
#include <QImage>
#include <QByteArray>

namespace WallpaperEngine::Assets {

class ByteStreamReader {
public:
    ByteStreamReader(std::span<const uint8_t> data) : m_data(data), m_offset(0) {}

    bool readBytes(void* dest, size_t size) {
        if (m_offset + size > m_data.size()) {
            return false;
        }
        std::memcpy(dest, m_data.data() + m_offset, size);
        m_offset += size;
        return true;
    }

    uint32_t readUInt32() {
        uint32_t val = 0;
        readBytes(&val, sizeof(val));
        return val;
    }

    int32_t readInt32() {
        int32_t val = 0;
        readBytes(&val, sizeof(val));
        return val;
    }

    bool readMagic(char* outMagic, size_t len = 9) {
        return readBytes(outMagic, len);
    }

    void skip(size_t count) {
        m_offset = std::min(m_offset + count, m_data.size());
    }

    size_t remaining() const {
        return m_data.size() > m_offset ? m_data.size() - m_offset : 0;
    }

    size_t offset() const { return m_offset; }
    void setOffset(size_t off) { m_offset = std::min(off, m_data.size()); }
    const uint8_t* currentPtr() const { return m_data.data() + m_offset; }

private:
    std::span<const uint8_t> m_data;
    size_t m_offset = 0;
};

bool TexParser::parse(std::span<const uint8_t> bytes, TexImage& outImage) {
    if (bytes.size() < 32) {
        return false;
    }

    // Fast Path: Direct standard Image (PNG / JPEG / WebP)
    QImage directImg;
    if (directImg.loadFromData(bytes.data(), static_cast<int>(bytes.size()))) {
        directImg = directImg.convertToFormat(QImage::Format_RGBA8888);
        outImage.width = directImg.width();
        outImage.height = directImg.height();
        outImage.textureWidth = directImg.width();
        outImage.textureHeight = directImg.height();
        outImage.format = TextureFormat::ARGB8888;
        Mipmap mip;
        mip.width = directImg.width();
        mip.height = directImg.height();
        mip.data.resize(directImg.sizeInBytes());
        std::memcpy(mip.data.data(), directImg.constBits(), directImg.sizeInBytes());
        outImage.mipmaps.push_back(std::move(mip));
        return true;
    }

    ByteStreamReader file(bytes);
    char magic[9] = {0};

    // Header 1: TEXV0005
    if (!file.readMagic(magic, 9) || std::strncmp(magic, "TEXV0005", 8) != 0) {
        // Embedded image offset scan fallback (PNG / JPEG / WebP)
        const uint8_t pngMagic[] = {0x89, 'P', 'N', 'G'};
        const uint8_t jpgMagic[] = {0xFF, 0xD8, 0xFF};

        size_t foundOffset = std::string::npos;
        for (size_t offset = 0; offset + 4 < bytes.size(); ++offset) {
            if (std::memcmp(bytes.data() + offset, pngMagic, 4) == 0 ||
                std::memcmp(bytes.data() + offset, jpgMagic, 3) == 0) {
                foundOffset = offset;
                break;
            }
        }

        if (foundOffset != std::string::npos) {
            size_t payloadLen = bytes.size() - foundOffset;

            // Check if PNG and locate IEND chunk boundary (IEND + 4 bytes CRC) to trim trailing concatenated images
            if (std::memcmp(bytes.data() + foundOffset, pngMagic, 4) == 0) {
                const uint8_t iendMagic[] = {'I', 'E', 'N', 'D'};
                for (size_t i = foundOffset; i + 8 <= bytes.size(); ++i) {
                    if (std::memcmp(bytes.data() + i, iendMagic, 4) == 0) {
                        payloadLen = (i + 8) - foundOffset;
                        break;
                    }
                }
            }

            QImage offsetImg;
            if (offsetImg.loadFromData(bytes.data() + foundOffset, static_cast<int>(payloadLen))) {
                offsetImg = offsetImg.convertToFormat(QImage::Format_RGBA8888);
                outImage.width = offsetImg.width();
                outImage.height = offsetImg.height();
                outImage.textureWidth = offsetImg.width();
                outImage.textureHeight = offsetImg.height();
                outImage.format = TextureFormat::ARGB8888;
                Mipmap mip;
                mip.width = offsetImg.width();
                mip.height = offsetImg.height();
                mip.data.resize(offsetImg.sizeInBytes());
                std::memcpy(mip.data.data(), offsetImg.constBits(), offsetImg.sizeInBytes());
                outImage.mipmaps.push_back(std::move(mip));
                return true;
            }
        }

        return false;
    }

    // Header 2: TEXI0001
    if (!file.readMagic(magic, 9) || std::strncmp(magic, "TEXI0001", 8) != 0) {
        return false;
    }

    outImage.format = static_cast<TextureFormat>(file.readUInt32());
    outImage.flags = file.readUInt32();
    outImage.textureWidth = file.readUInt32();
    outImage.textureHeight = file.readUInt32();
    outImage.width = file.readUInt32();
    outImage.height = file.readUInt32();
    file.skip(4); // Reserved (color / extra)

    // Search for Body Tag (TEXB0001, TEXB0002, TEXB0003, TEXB0004)
    char bodyTag[9] = {0};
    if (!file.readMagic(bodyTag, 9)) {
        return false;
    }

    uint32_t imageCount = file.readUInt32();

    if (std::strncmp(bodyTag, "TEXB0003", 8) == 0) {
        for (uint32_t imgIdx = 0; imgIdx < imageCount; ++imgIdx) {
            int32_t fif = file.readInt32();

            if (fif == 2 || fif == 13) {
                // FreeImage JPEG (2) or PNG (13) stream
                uint32_t fmt = file.readUInt32();
                Q_UNUSED(fmt);
                uint32_t mipWidth = file.readUInt32();
                uint32_t mipHeight = file.readUInt32();
                file.skip(8); // flags & extra
                uint32_t compSize = file.readUInt32();

                if (compSize == 0) continue;
                if (compSize > file.remaining()) compSize = static_cast<uint32_t>(file.remaining());

                std::vector<uint8_t> rawPayload(compSize);
                file.readBytes(rawPayload.data(), compSize);

                size_t imgOffset = 0;
                const uint8_t pngMagic[] = {0x89, 'P', 'N', 'G'};
                const uint8_t jpgMagic[] = {0xFF, 0xD8, 0xFF};
                for (size_t k = 0; k + 4 <= rawPayload.size(); ++k) {
                    if (std::memcmp(rawPayload.data() + k, pngMagic, 4) == 0 ||
                        std::memcmp(rawPayload.data() + k, jpgMagic, 3) == 0) {
                        imgOffset = k;
                        break;
                    }
                }

                QImage decoded;
                if (decoded.loadFromData(rawPayload.data() + imgOffset, static_cast<int>(rawPayload.size() - imgOffset))) {
                    decoded = decoded.convertToFormat(QImage::Format_RGBA8888);
                    outImage.width = decoded.width();
                    outImage.height = decoded.height();
                    outImage.textureWidth = decoded.width();
                    outImage.textureHeight = decoded.height();
                    Mipmap mip;
                    mip.width = decoded.width();
                    mip.height = decoded.height();
                    mip.data.resize(decoded.sizeInBytes());
                    std::memcpy(mip.data.data(), decoded.constBits(), decoded.sizeInBytes());
                    outImage.format = TextureFormat::ARGB8888;
                    outImage.mipmaps.push_back(std::move(mip));
                } else {
                    Mipmap mip;
                    mip.width = mipWidth;
                    mip.height = mipHeight;
                    mip.data = std::move(rawPayload);
                    outImage.mipmaps.push_back(std::move(mip));
                }
            } else {
                // LZ4 block stream inside TEXB0003
                int32_t compFormat = file.readInt32();
                uint32_t mipWidth = file.readUInt32();
                uint32_t mipHeight = file.readUInt32();
                uint32_t numChunks = file.readUInt32();
                Q_UNUSED(numChunks);
                uint32_t uncompSize = file.readUInt32();
                uint32_t compSize = file.readUInt32();

                if (mipWidth == 0 || mipHeight == 0 || compSize == 0 || compSize > file.remaining()) {
                    continue;
                }
                // Validate uncompressed size to prevent excessive memory allocation
                if (uncompSize < 0 || uncompSize > 100 * 1024 * 1024) { // 100 MB
                    continue;
                }

                Mipmap mip;
                mip.width = mipWidth;
                mip.height = mipHeight;
                mip.data.resize(uncompSize > 0 ? uncompSize : compSize);

                if (uncompSize > 0 && compSize != uncompSize) {
                    int decomp = LZ4_decompress_safe(
                        reinterpret_cast<const char*>(file.currentPtr()),
                        reinterpret_cast<char*>(mip.data.data()),
                        static_cast<int>(compSize),
                        static_cast<int>(uncompSize)
                    );
                    file.skip(compSize);
                    if (decomp < 0) continue;
                } else {
                    file.readBytes(mip.data.data(), compSize);
                }

                // Check for embedded PNG/JPEG payload
                if (mip.data.size() > 4 && ((mip.data[0] == 0xFF && mip.data[1] == 0xD8) || (mip.data[0] == 0x89 && mip.data[1] == 'P'))) {
                    QImage decoded;
                    if (decoded.loadFromData(mip.data.data(), static_cast<int>(mip.data.size()))) {
                        decoded = decoded.convertToFormat(QImage::Format_RGBA8888);
                        mip.width = decoded.width();
                        mip.height = decoded.height();
                        mip.data.resize(decoded.sizeInBytes());
                        std::memcpy(mip.data.data(), decoded.constBits(), decoded.sizeInBytes());
                        outImage.format = TextureFormat::ARGB8888;
                    }
                }
                // NOTE: In TEXB0003, the field we read as compFormat is actually
                // Almamu's "compression" flag (0=none, 1=LZ4), NOT a texture format.
                // Texture format comes from the TEXI header only. Do NOT override.

                outImage.mipmaps.push_back(std::move(mip));
            }
        }
    } else if (std::strncmp(bodyTag, "TEXB0004", 8) == 0) {
        // TEXB0004 has two extra fields after imageCount (per Almamu's parseContainer):
        //   freeImageFormat (uint32, FIF enum)
        //   isVideoMp4 (uint32, 1 if MP4 video)
        uint32_t fif = file.readUInt32();
        uint32_t isVideoMp4 = file.readUInt32();
        Q_UNUSED(fif);
        Q_UNUSED(isVideoMp4);

        // CRITICAL: Almamu's logic — if FIF is not MP4, downgrade to TEXB0003
        // structure (no per-mipmap extras). Only actual MP4 video textures keep
        // the TEXB0004 per-mipmap extras (extra1, extra2, json, extra3).
        bool hasTexb4Extras = (isVideoMp4 == 1);

        // TEXB0003-style structure (no per-mipmap extras):
        //   For each image:
        //     For each mipmap:
        //       width, height, compression, uncompressedSize, compressedSize, data
        // TEXB0004-style structure (MP4 video only):
        //   For each image:
        //     mipmapCount
        //     For each mipmap:
        //       extra1, extra2, json, extra3, width, height, compression, ...
        if (hasTexb4Extras) {
            // True TEXB0004 with per-mipmap extras (MP4 video)
            for (uint32_t imgIdx = 0; imgIdx < imageCount; ++imgIdx) {
                uint32_t mipmapCount = file.readUInt32();

                for (uint32_t mipIdx = 0; mipIdx < mipmapCount; ++mipIdx) {
                    file.skip(4); // extra1
                    file.skip(4); // extra2
                    // Null-terminated JSON string - check for path traversal
                    bool hasPathTraversal = false;
                    char prev = 0;
                    while (file.remaining() > 0) {
                        uint8_t ch = *file.currentPtr();
                        file.skip(1);
                        if (ch == 0) break;
                        if (ch == '.' && prev == '.') {
                            hasPathTraversal = true;
                        }
                        prev = ch;
                    }

                    if (hasPathTraversal) {
                        // Skip the rest of this mipmap: extra3, width, height, compression, uncompressedSize, compressedSize
                        file.skip(4); // extra3
                        file.skip(4); // width
                        file.skip(4); // height
                        file.skip(4); // compression
                        file.skip(4); // uncompressedSize
                        file.skip(4); // compressedSize
                        continue;
                    }

                    file.skip(4); // extra3

                    uint32_t mipWidth = file.readUInt32();
                    uint32_t mipHeight = file.readUInt32();
                    uint32_t compression = file.readUInt32();
                    int32_t uncompressedSize = file.readInt32();
                    int32_t compressedSize = file.readInt32();

                    if (compression == 0) uncompressedSize = compressedSize;
                    // Validate uncompressed size to prevent excessive memory allocation and negative values
                    if (uncompressedSize < 0 || uncompressedSize > 100 * 1024 * 1024) {
                        continue;
                    }
                    if (mipWidth == 0 || mipHeight == 0 || compressedSize <= 0) continue;
                    if (static_cast<uint32_t>(compressedSize) > file.remaining()) continue;

                    Mipmap mip;
                    mip.width = mipWidth;
                    mip.height = mipHeight;

                    if (compression == 1 && uncompressedSize > 0) {
                        mip.data.resize(static_cast<uint32_t>(uncompressedSize));
                        LZ4_decompress_safe(
                            reinterpret_cast<const char*>(file.currentPtr()),
                            reinterpret_cast<char*>(mip.data.data()),
                            compressedSize, uncompressedSize);
                        file.skip(static_cast<uint32_t>(compressedSize));
                    } else {
                        uint32_t dataSize = static_cast<uint32_t>(compressedSize);
                        mip.data.resize(dataSize);
                        file.readBytes(mip.data.data(), dataSize);
                    }

                    outImage.mipmaps.push_back(std::move(mip));
                }
            }
        } else {
            // Downgraded to TEXB0003 structure: NO per-mipmap extras.
            // Per-Almamu: for each image → for each mipmap:
            //   width, height, compression, uncompressedSize, compressedSize, data
            for (uint32_t imgIdx = 0; imgIdx < imageCount; ++imgIdx) {
                uint32_t mipmapCount = file.readUInt32();

                for (uint32_t mipIdx = 0; mipIdx < mipmapCount; ++mipIdx) {
                    uint32_t mipWidth = file.readUInt32();
                    uint32_t mipHeight = file.readUInt32();
                    uint32_t compression = file.readUInt32();
                    int32_t uncompressedSize = file.readInt32();
                    int32_t compressedSize = file.readInt32();

                    if (compression == 0) uncompressedSize = compressedSize;
                    // Validate uncompressed size to prevent excessive memory allocation and negative values
                    if (uncompressedSize < 0 || uncompressedSize > 100 * 1024 * 1024) {
                        continue;
                    }
                    if (mipWidth == 0 || mipHeight == 0 || compressedSize <= 0) continue;
                    if (static_cast<uint32_t>(compressedSize) > file.remaining()) continue;

                    Mipmap mip;
                    mip.width = mipWidth;
                    mip.height = mipHeight;

                    if (compression == 1 && uncompressedSize > 0) {
                        mip.data.resize(static_cast<uint32_t>(uncompressedSize));
                        LZ4_decompress_safe(
                            reinterpret_cast<const char*>(file.currentPtr()),
                            reinterpret_cast<char*>(mip.data.data()),
                            compressedSize, uncompressedSize);
                        file.skip(static_cast<uint32_t>(compressedSize));
                    } else {
                        uint32_t dataSize = static_cast<uint32_t>(compressedSize);
                        mip.data.resize(dataSize);
                        file.readBytes(mip.data.data(), dataSize);
                    }

                    outImage.mipmaps.push_back(std::move(mip));
                }
            }
        }
    } else {
        // TEXB0001 / TEXB0002
        for (uint32_t imgIdx = 0; imgIdx < imageCount; ++imgIdx) {
            uint32_t compFormat = file.readUInt32();
            uint32_t mipWidth = file.readUInt32();
            uint32_t mipHeight = file.readUInt32();
            uint32_t compression = file.readUInt32();
            uint32_t uncompSize = file.readUInt32();
            uint32_t compSize = file.readUInt32();

            if (compSize == 0 || compSize > file.remaining()) continue;

            Mipmap mip;
            mip.width = mipWidth;
            mip.height = mipHeight;
            mip.data.resize(uncompSize > 0 ? uncompSize : compSize);

            if (compression == 1) {
                LZ4_decompress_safe(
                    reinterpret_cast<const char*>(file.currentPtr()),
                    reinterpret_cast<char*>(mip.data.data()),
                    static_cast<int>(compSize),
                    static_cast<int>(uncompSize)
                );
                file.skip(compSize);
            } else {
                file.readBytes(mip.data.data(), compSize);
            }

            if (compFormat != 0) {
                outImage.format = static_cast<TextureFormat>(compFormat);
            }
            outImage.mipmaps.push_back(std::move(mip));
        }
    }

    if (!outImage.mipmaps.empty()) {
        if (outImage.width == 0) outImage.width = outImage.mipmaps[0].width;
        if (outImage.height == 0) outImage.height = outImage.mipmaps[0].height;
        if (outImage.textureWidth == 0) outImage.textureWidth = outImage.mipmaps[0].width;
        if (outImage.textureHeight == 0) outImage.textureHeight = outImage.mipmaps[0].height;
        (void)outImage; // suppress unused in release — verbose log removed for batch spam
        return true;
    }

    return false;
}

} // namespace WallpaperEngine::Assets
