#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>

#if __has_include(<QImage>)
#include <QImage>
#else
// Stub for QImage when Qt is not available
// Define types that are used in the QImage stub
using QRgb = uint32_t;
using qsizetype = ptrdiff_t;
typedef unsigned char uchar;

class QImage {
public:
    enum Format {
        Format_ARGB32,
        Format_RGBA8888,
        Format_RGBX8888,
        Format_RGB888,
        // Add more formats if needed
    };

    QImage() = default;
    explicit QImage(int width, int height, Format format = Format_ARGB32) {
        m_width = width;
        m_height = height;
        m_format = format;
        // Allocate buffer for the image data (assuming 4 bytes per pixel for RGBA8888)
        m_data.resize(width * height * 4);
    }

    bool isNull() const { return m_data.empty(); }

    int width() const { return m_width; }
    int height() const { return m_height; }

    Format format() const { return m_format; }

    QImage convertToFormat(Format format) const {
        // For simplicity, we just change the format without converting data.
        // In a real implementation, we would convert the pixel data.
        QImage result(m_width, m_height, format);
        // Note: We are not actually converting the image data.
        // This is a stub, so we assume the data is already in the desired format or that the caller handles it.
        return result;
    }

    int sizeInBytes() const {
        return static_cast<int>(m_data.size());
    }

    bool loadFromData(const uchar* data, int size) {
        // For simplicity, we just copy the data if the size matches our expected size.
        // We don't actually parse the image format (PNG, JPEG, etc.).
        if (size == static_cast<int>(m_data.size())) {
            m_data.assign(data, data + size);
            return true;
        }
        return false;
    }

    const uchar* constBits() const {
        return m_data.data();
    }
    uchar* bits() {
        return m_data.data();
    }

    void fill(int value) {
        std::fill(m_data.begin(), m_data.end(), static_cast<uchar>(value));
    }

    void save(const std::string&, const std::string&) {
        // Stub: do nothing
    }

    void copy() {
        // Stub: do nothing
    }

    QRgb pixel(int x, int y) const {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height) {
            return 0;
        }
        int index = (y * m_width + x) * 4;
        // Assuming RGBA8888: we return the ARGB value as a QRgb (which is uint32_t, ARGB)
        uint8_t r = m_data[index];
        uint8_t g = m_data[index+1];
        uint8_t b = m_data[index+2];
        uint8_t a = m_data[index+3];
        return (a << 24) | (r << 16) | (g << 8) | b;
    }

    qsizetype bytesPerLine() const { return static_cast<qsizetype>(m_width * 4); }; // assuming 4 bytes per pixel
private:
    int m_width = 0;
    int m_height = 0;
    Format m_format = Format_ARGB32;
    std::vector<uchar> m_data;
};
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace WallpaperEngine::Assets {

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    // Open video from raw data (MP4 bytes in memory)
    bool openFromData(const uint8_t* data, size_t size, int targetWidth, int targetHeight);

    // Open video from file path
    bool openFromFile(const std::string& path, int targetWidth, int targetHeight);

    // Decode next frame → QImage (Format_ARGB32). Returns null QImage if no new frame.
    QImage decodeNextFrame();

    // Seek to beginning (loop)
    void seekToStart();

    // Properties
    int width() const { return m_width; }
    int height() const { return m_height; }
    bool isOpen() const { return m_open; }

private:
    bool openInternal(int targetWidth, int targetHeight);
    void close();

    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    SwsContext* m_swsCtx = nullptr;
    AVFrame* m_frame = nullptr;
    AVFrame* m_rgbFrame = nullptr;
    uint8_t* m_rgbBuffer = nullptr;
    int m_videoStreamIdx = -1;
    int m_width = 0;
    int m_height = 0;
    bool m_open = false;

    // For memory-backed input
    std::vector<uint8_t> m_dataCopy;
    AVIOContext* m_avioCtx = nullptr;
    unsigned char* m_avioBuffer = nullptr;
    void* m_avioOpaque = nullptr; // AvioContextData for custom I/O
};

} // namespace WallpaperEngine::Assets