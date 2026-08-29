#pragma once

#include <cstdint>
#include <vector>
#include <QImage>
#include <string>

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
