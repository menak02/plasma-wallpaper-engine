#include "video_decoder.h"
#include <iostream>

extern "C" {
#include <libavutil/imgutils.h>
}

namespace WallpaperEngine::Assets {

VideoDecoder::VideoDecoder() {}

VideoDecoder::~VideoDecoder() {
    close();
}

void VideoDecoder::close() {
    if (m_codecCtx) { avcodec_flush_buffers(m_codecCtx); }
    if (m_frame) { av_frame_free(&m_frame); m_frame = nullptr; }
    if (m_rgbFrame) { av_frame_free(&m_rgbFrame); m_rgbFrame = nullptr; }
    if (m_rgbBuffer) { av_free(m_rgbBuffer); m_rgbBuffer = nullptr; }
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); m_codecCtx = nullptr; }
    if (m_formatCtx) {
        avformat_close_input(&m_formatCtx);
        m_formatCtx = nullptr;
    }
    m_open = false;
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

    // Single thread for verifier/daemon stability; 0 auto uses thread-pool that races on multi-decoder
    m_codecCtx->thread_count = 1;

    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        std::cerr << "VideoDecoder: Failed to open codec" << std::endl;
        close();
        return false;
    }

    m_width = m_codecCtx->width;
    m_height = m_codecCtx->height;
    m_sourceHeight = m_codecCtx->height;

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
                      m_frame->data, m_frame->linesize, 0, m_sourceHeight,
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

double VideoDecoder::framesPerSecond() const {
    if (!m_open || !m_formatCtx || m_videoStreamIdx < 0) return 30.0;
    AVStream* st = m_formatCtx->streams[m_videoStreamIdx];
    AVRational fr = st->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) fr = st->r_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) return 30.0;
    const double fps = av_q2d(fr);
    return (fps > 0.0 && fps <= 240.0) ? fps : 30.0;
}

} // namespace WallpaperEngine::Assets
