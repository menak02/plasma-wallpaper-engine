#include "video_decoder.h"
#include <cstring>
#include <iostream>

extern "C" {
#include <libavutil/imgutils.h>
}

namespace WallpaperEngine::Assets {

// File-scope struct for AVIO custom I/O context
struct AvioContextData {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

// AVIO callback: read from memory buffer
static int avioReadPacket(void* opaque, uint8_t* buf, int buf_size) {
    auto* avioCtx = static_cast<AVIOContext*>(opaque);
    auto* data = static_cast<uint8_t*>(avioCtx->opaque);
    size_t pos = static_cast<size_t>(avioCtx->pos);
    // avioCtx->opaque is AVIOContext itself, we stored data pointer in a custom struct
    // Actually we need a different approach - use the AVIOContext's userdata
    return 0; // placeholder
}

VideoDecoder::VideoDecoder() {}

VideoDecoder::~VideoDecoder() {
    close();
}

void VideoDecoder::close() {
    if (m_frame) { av_frame_free(&m_frame); m_frame = nullptr; }
    if (m_rgbFrame) { av_frame_free(&m_rgbFrame); m_rgbFrame = nullptr; }
    if (m_rgbBuffer) { av_free(m_rgbBuffer); m_rgbBuffer = nullptr; }
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); m_codecCtx = nullptr; }
    // With CUSTOM_IO, avformat_close_input does NOT free AVIOContext
    // Close formatCtx first (stops I/O), then free AVIO ourselves
    if (m_formatCtx) { avformat_close_input(&m_formatCtx); m_formatCtx = nullptr; }
    if (m_avioCtx) {
        // avio_alloc_context used av_malloc for buffer; avio_context_free calls av_free
        avio_context_free(&m_avioCtx);
        m_avioCtx = nullptr;
    }
    m_avioBuffer = nullptr; // was freed by avio_context_free
    if (m_avioOpaque) { delete static_cast<AvioContextData*>(m_avioOpaque); m_avioOpaque = nullptr; }
    m_open = false;
}

bool VideoDecoder::openFromData(const uint8_t* data, size_t size, int targetWidth, int targetHeight) {
    close();
    m_dataCopy.assign(data, data + size);

    // Create AVIOContext for custom I/O
    constexpr int avioBufSize = 32768;
    m_avioBuffer = static_cast<unsigned char*>(av_malloc(avioBufSize));

    // CRITICAL: use m_dataCopy.data() not original 'data' pointer —
    // 'data' points into texImg.mipmaps which gets freed after resolveTexture returns
    auto* ctxData = new AvioContextData{m_dataCopy.data(), m_dataCopy.size(), 0};
    m_avioOpaque = ctxData; // track for cleanup in close()

    m_avioCtx = avio_alloc_context(
        m_avioBuffer, avioBufSize, 0, ctxData,
        [](void* opaque, uint8_t* buf, int buf_size) -> int {
            auto* d = static_cast<AvioContextData*>(opaque);
            size_t remaining = d->size - d->pos;
            if (remaining == 0) return AVERROR_EOF;
            size_t toRead = std::min(remaining, static_cast<size_t>(buf_size));
            std::memcpy(buf, d->data + d->pos, toRead);
            d->pos += toRead;
            return static_cast<int>(toRead);
        },
        nullptr, // write
        [](void* opaque, int64_t pos, int whence) -> int64_t {
            auto* d = static_cast<AvioContextData*>(opaque);
            if (whence == AVSEEK_SIZE) {
                return static_cast<int64_t>(d->size);
            }
            d->pos = static_cast<size_t>(pos);
            return static_cast<int64_t>(d->pos);
        }
    );

    m_formatCtx = avformat_alloc_context();
    m_formatCtx->pb = m_avioCtx;
    m_formatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

    // Open input
    int ret = avformat_open_input(&m_formatCtx, nullptr, nullptr, nullptr);
    if (ret < 0) {
        std::cerr << "VideoDecoder: Failed to open input: " << ret << std::endl;
        close();
        return false;
    }

    return openInternal(targetWidth, targetHeight);
}

bool VideoDecoder::openFromFile(const std::string& path, int targetWidth, int targetHeight) {
    close();

    int ret = avformat_open_input(&m_formatCtx, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::cerr << "VideoDecoder: Failed to open file: " << path << std::endl;
        return false;
    }

    return openInternal(targetWidth, targetHeight);
}

bool VideoDecoder::openInternal(int targetWidth, int targetHeight) {
    int ret = avformat_find_stream_info(m_formatCtx, nullptr);
    if (ret < 0) {
        std::cerr << "VideoDecoder: Failed to find stream info" << std::endl;
        close();
        return false;
    }

    // Find video stream
    m_videoStreamIdx = -1;
    for (unsigned i = 0; i < m_formatCtx->nb_streams; ++i) {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIdx = static_cast<int>(i);
            break;
        }
    }
    if (m_videoStreamIdx < 0) {
        std::cerr << "VideoDecoder: No video stream found" << std::endl;
        close();
        return false;
    }

    AVStream* videoStream = m_formatCtx->streams[m_videoStreamIdx];
    const AVCodec* codec = avcodec_find_decoder(videoStream->codecpar->codec_id);
    if (!codec) {
        std::cerr << "VideoDecoder: No decoder found for codec id "
                  << videoStream->codecpar->codec_id << std::endl;
        close();
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_codecCtx, videoStream->codecpar);

    // Enable multi-threading
    m_codecCtx->thread_count = 0; // auto-detect

    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        std::cerr << "VideoDecoder: Failed to open codec" << std::endl;
        close();
        return false;
    }

    m_width = m_codecCtx->width;
    m_height = m_codecCtx->height;

    std::cerr << "VideoDecoder: Opened " << m_width << "x" << m_height
              << " " << codec->name << std::endl;

    // Allocate frames
    m_frame = av_frame_alloc();
    m_rgbFrame = av_frame_alloc();

    // Setup SWS context for format conversion
    m_swsCtx = sws_getContext(
        m_width, m_height, m_codecCtx->pix_fmt,
        targetWidth > 0 ? targetWidth : m_width,
        targetHeight > 0 ? targetHeight : m_height,
        AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!m_swsCtx) {
        std::cerr << "VideoDecoder: Failed to create SWS context" << std::endl;
        close();
        return false;
    }

    // Allocate RGB frame buffer
    int outW = targetWidth > 0 ? targetWidth : m_width;
    int outH = targetHeight > 0 ? targetHeight : m_height;
    m_width = outW;
    m_height = outH;

    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, outW, outH, 1);
    m_rgbBuffer = static_cast<uint8_t*>(av_malloc(numBytes));
    av_image_fill_arrays(m_rgbFrame->data, m_rgbFrame->linesize,
                         m_rgbBuffer, AV_PIX_FMT_RGBA, outW, outH, 1);

    m_open = true;
    return true;
}

QImage VideoDecoder::decodeNextFrame() {
    if (!m_open) return {};

    AVPacket* packet = av_packet_alloc();
    QImage result;

    while (av_read_frame(m_formatCtx, packet) >= 0) {
        if (packet->stream_index != m_videoStreamIdx) {
            av_packet_unref(packet);
            continue;
        }

        int ret = avcodec_send_packet(m_codecCtx, packet);
        av_packet_unref(packet);

        if (ret < 0) {
            continue;
        }

        ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == 0) {
            // Convert to RGBA
            sws_scale(m_swsCtx,
                      m_frame->data, m_frame->linesize, 0, m_height,
                      m_rgbFrame->data, m_rgbFrame->linesize);

            // Copy to QImage
            QImage img(m_rgbFrame->data[0], m_width, m_height,
                       m_rgbFrame->linesize[0], QImage::Format_RGBA8888);
            result = img.copy().convertToFormat(QImage::Format_ARGB32);
            break;
        } else if (ret == AVERROR(EAGAIN)) {
            continue;
        } else {
            break;
        }
    }

    av_packet_free(&packet);
    return result;
}

void VideoDecoder::seekToStart() {
    if (!m_open || !m_formatCtx) return;
    av_seek_frame(m_formatCtx, m_videoStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codecCtx);
}

} // namespace WallpaperEngine::Assets
