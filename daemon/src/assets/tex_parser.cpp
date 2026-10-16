#include "tex_parser.h"
#include "dxt_decoder.h"
#include <algorithm>
#include <climits>
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
        // TEX integers are little-endian on disk; assemble byte-wise so
        // parsing does not depend on host endianness.
        uint8_t b[4] = {0, 0, 0, 0};
        readBytes(b, sizeof(b));
        return static_cast<uint32_t>(b[0]) |
               (static_cast<uint32_t>(b[1]) << 8) |
               (static_cast<uint32_t>(b[2]) << 16) |
               (static_cast<uint32_t>(b[3]) << 24);
    }

    int32_t readInt32() {
        return static_cast<int32_t>(readUInt32());
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
    // QImage::loadFromData takes int; reject spans it cannot represent.
    if (bytes.size() > static_cast<size_t>(INT_MAX)) {
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
    // Bound the work a crafted header can request: a huge count just spins
    // the parse loop at EOF without consuming input.
    if (imageCount > 256) return false;

    // Ground-truth layout (Almamu parseContainer + parseMipmap): after the
    // body tag and imageCount, TEXB0003 carries one FreeImage format and
    // TEXB0004 adds an MP4 flag — both at container level. Per image comes
    // mipmapCount, then per mip: [TEXB0004 extras] width, height,
    // [0002/0003/0004] compression, uncompressedSize, compressedSize, data.
    // TEXB0001 mips have no compression/uncompressedSize fields. The texture
    // format comes from the TEXI header only — never from mip data.
    bool isVideoMp4 = false;
    if (std::strncmp(bodyTag, "TEXB0004", 8) == 0) {
        file.skip(4); // freeImageFormat
        isVideoMp4 = file.readUInt32() == 1;
    } else if (std::strncmp(bodyTag, "TEXB0003", 8) == 0) {
        file.skip(4); // freeImageFormat
    }
    const bool hasTexb4Extras = isVideoMp4;
    const bool isTexb1 = std::strncmp(bodyTag, "TEXB0001", 8) == 0;

    for (uint32_t imgIdx = 0; imgIdx < imageCount; ++imgIdx) {
        uint32_t mipmapCount = file.readUInt32();
        if (mipmapCount > 256) break;

        for (uint32_t mipIdx = 0; mipIdx < mipmapCount; ++mipIdx) {
            if (hasTexb4Extras) {
                file.skip(4); // extra1
                file.skip(4); // extra2
                // Null-terminated JSON sidecar string
                while (file.remaining() > 0 && *file.currentPtr() != 0) {
                    file.skip(1);
                }
                if (file.remaining() > 0) file.skip(1);
                file.skip(4); // extra3
            }

            uint32_t mipWidth = file.readUInt32();
            uint32_t mipHeight = file.readUInt32();

            uint32_t compression = 0;
            int32_t uncompressedSize = 0;
            if (!isTexb1) {
                compression = file.readUInt32();
                uncompressedSize = file.readInt32();
            }
            int32_t compressedSize = file.readInt32();

            if (compression == 0) uncompressedSize = compressedSize;
            if (mipWidth == 0 || mipHeight == 0 || compressedSize <= 0) continue;
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
                int decomp = LZ4_decompress_safe(
                    reinterpret_cast<const char*>(file.currentPtr()),
                    reinterpret_cast<char*>(mip.data.data()),
                    compressedSize, uncompressedSize);
                file.skip(static_cast<uint32_t>(compressedSize));
                if (decomp < 0) continue;
            } else {
                uint32_t dataSize = static_cast<uint32_t>(compressedSize);
                mip.data.resize(dataSize);
                file.readBytes(mip.data.data(), dataSize);
            }

            // FreeImage PNG/JPEG payloads (TEXB0003 FIF streams) are decoded
            // eagerly and normalized to RGBA8888 so consumers see real pixels.
            if (mip.data.size() > 4 &&
                ((mip.data[0] == 0xFF && mip.data[1] == 0xD8) || (mip.data[0] == 0x89 && mip.data[1] == 'P'))) {
                QImage decoded;
                if (decoded.loadFromData(mip.data.data(), static_cast<int>(mip.data.size()))) {
                    decoded = decoded.convertToFormat(QImage::Format_RGBA8888);
                    mip.width = decoded.width();
                    mip.height = decoded.height();
                    mip.data.resize(decoded.sizeInBytes());
                    std::memcpy(mip.data.data(), decoded.constBits(), decoded.sizeInBytes());
                    outImage.format = TextureFormat::ARGB8888;
                    // Only mip0 defines the image size — later mips would
                    // overwrite the dims with their own (smaller) values.
                    if (outImage.mipmaps.empty()) {
                        outImage.width = decoded.width();
                        outImage.height = decoded.height();
                        outImage.textureWidth = decoded.width();
                        outImage.textureHeight = decoded.height();
                    }
                }
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
