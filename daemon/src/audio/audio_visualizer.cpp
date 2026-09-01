#include "audio_visualizer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <complex>
#if __has_include(<QAudioSource>)
#include <QAudioSource>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QIODevice>
#endif

namespace WallpaperEngine::Audio {

struct AudioVisualizer::LiveImpl {
#if __has_include(<QAudioSource>)
    QAudioSource* source = nullptr;
    QIODevice* device = nullptr;
#endif
};

AudioVisualizer::AudioVisualizer() : m_bandCount(8), m_waveSize(2048) {}

AudioVisualizer::~AudioVisualizer() { stopLiveCapture(); }

void AudioVisualizer::init(const std::vector<uint8_t>& audioBytes, int sampleRate, int channels) {
    m_sampleRate = sampleRate;
    m_channels = channels;

    // Convert audio bytes to PCM (simple 16-bit conversion)
    m_rawAudio.resize(audioBytes.size() / 2);
    if (!audioBytes.empty()) {
        std::memcpy(m_rawAudio.data(), audioBytes.data(), audioBytes.size());
    }

    // Calculate duration
    m_duration = static_cast<float>(m_rawAudio.size()) / (m_sampleRate * m_channels);
    m_currentTime = 0.0f;
    m_playbackPos = 0;

    // Allocate spectrum and waveform buffers
    m_spectrum.resize(m_waveSize / 2, 0.0f);
    m_waveform.resize(m_waveSize, 0.0f);
}

void AudioVisualizer::update() {
    if (m_rawAudio.empty() || m_volume <= 0.0f) {
        std::fill(m_spectrum.begin(), m_spectrum.end(), 0.0f);
        std::fill(m_waveform.begin(), m_waveform.end(), 0.0f);
        return;
    }
    int windowStart = static_cast<int>(m_playbackPos % m_rawAudio.size());
    int windowEnd = std::min(windowStart + m_waveSize, static_cast<int>(m_rawAudio.size()));
    for (int i = 0; i < m_waveSize; ++i) {
        if (windowStart + i < windowEnd) m_waveform[i] = static_cast<float>(m_rawAudio[windowStart + i]) / 32768.0f;
        else m_waveform[i] = 0.0f;
    }
    m_playbackPos = (m_playbackPos + m_waveSize / m_channels) % m_rawAudio.size();
    m_currentTime += static_cast<float>(m_waveSize) / (m_sampleRate * m_channels);
    if (m_currentTime >= m_duration) { m_currentTime = 0.0f; m_playbackPos = 0; }
    computeFFT();
}

void AudioVisualizer::computeBands() {
    // If we have FFT spectrum, map linear bins to logarithmic bands
    if (m_spectrum.size() == static_cast<size_t>(m_waveSize / 2)) {
        // Check if spectrum was filled by FFT (non-zero beyond simple energy)
        // Aggregate FFT bins into bands using logarithmic spacing
        for (int band = 0; band < m_bandCount; ++band) {
            // Logarithmic band boundaries: 0..1024
            float lowFrac = static_cast<float>(band) / m_bandCount;
            float highFrac = static_cast<float>(band + 1) / m_bandCount;
            // Exponential mapping for more low-freq resolution
            int binLow = static_cast<int>(std::pow(2.0f, lowFrac * 10.0f) - 1.0f);
            int binHigh = static_cast<int>(std::pow(2.0f, highFrac * 10.0f) - 1.0f);
            binLow = std::clamp(binLow, 0, m_waveSize / 2 - 1);
            binHigh = std::clamp(binHigh, binLow + 1, m_waveSize / 2);
            float sum = 0.0f;
            for (int b = binLow; b < binHigh; ++b) sum += m_spectrum[b];
            float avg = sum / (binHigh - binLow);
            // Compress dynamic range
            m_spectrum[band] = std::clamp(std::sqrt(avg) * m_volume * 2.0f, 0.0f, 1.0f);
        }
        return;
    }
    // Fallback: energy-based
    int bandSize = m_waveSize / m_bandCount;
    for (int band = 0; band < m_bandCount; ++band) {
        float energy = 0.0f;
        int count = 0;
        int start = band * bandSize;
        int end = std::min(start + bandSize, m_waveSize);
        for (int i = start; i < end; ++i) { energy += m_waveform[i] * m_waveform[i]; count++; }
        m_spectrum[band] = std::sqrt(energy / count) * m_volume;
    }
}

static void fftRadix2(std::vector<std::complex<float>>& data) {
    size_t n = data.size();
    // bit reversal
    size_t j = 0;
    for (size_t i = 1; i < n; ++i) {
        size_t bit = n >> 1;
        while (j & bit) { j ^= bit; bit >>= 1; }
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = 2.0f * 3.1415926535f / static_cast<float>(len);
        std::complex<float> wlen(std::cos(ang), -std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                std::complex<float> u = data[i + k];
                std::complex<float> v = data[i + k + len/2] * w;
                data[i + k] = u + v;
                data[i + k + len/2] = u - v;
                w *= wlen;
            }
        }
    }
}

void AudioVisualizer::computeFFT() {
    if (m_waveform.empty()) return;
    size_t n = m_waveform.size();
    std::vector<std::complex<float>> buf(n);
    // Hann window + copy
    for (size_t i = 0; i < n; ++i) {
        float w = 0.5f * (1.0f - std::cos(2.0f * 3.1415926535f * static_cast<float>(i) / static_cast<float>(n - 1)));
        buf[i] = std::complex<float>(m_waveform[i] * w, 0.0f);
    }
    fftRadix2(buf);
    // Magnitude spectrum (first half)
    m_spectrum.resize(n / 2);
    for (size_t i = 0; i < n / 2; ++i) {
        float mag = std::abs(buf[i]) / static_cast<float>(n);
        // Log scale
        m_spectrum[i] = std::clamp(mag * 50.0f, 0.0f, 1.0f);
    }
    // Now aggregate into bands (reuse computeBands path after resize)
    // Keep full spectrum for getBand per-bin? But getBand expects bandCount bands.
    // Preserve full spectrum then compute band summary into first bandCount entries
    std::vector<float> full = m_spectrum;
    m_spectrum.assign(m_bandCount, 0.0f);
    for (int band = 0; band < m_bandCount; ++band) {
        float lowFrac = static_cast<float>(band) / m_bandCount;
        float highFrac = static_cast<float>(band + 1) / m_bandCount;
        int binLow = static_cast<int>(std::pow(2.0f, lowFrac * 10.0f) - 1.0f);
        int binHigh = static_cast<int>(std::pow(2.0f, highFrac * 10.0f) - 1.0f);
        binLow = std::clamp(binLow, 0, static_cast<int>(full.size() - 1));
        binHigh = std::clamp(binHigh, binLow + 1, static_cast<int>(full.size()));
        float sum = 0.0f;
        for (int b = binLow; b < binHigh; ++b) sum += full[b];
        float avg = sum / (binHigh - binLow);
        m_spectrum[band] = std::clamp(std::sqrt(avg) * m_volume * 3.0f, 0.0f, 1.0f);
    }
}

float AudioVisualizer::getBand(int band) const {
    if (band < 0 || band >= m_bandCount) {
        return 0.0f;
    }
    return m_spectrum[band];
}

int AudioVisualizer::getWaveform(float* outBuffer, int maxSamples) const {
    int count = std::min(maxSamples, m_waveSize);
    std::memcpy(outBuffer, m_waveform.data(), count * sizeof(float));
    return count;
}

void AudioVisualizer::setVolume(float volume) {
    m_volume = std::clamp(volume, 0.0f, 1.0f);
}

bool AudioVisualizer::startLiveCapture(int sampleRate, int channels) {
#if __has_include(<QAudioSource>)
    stopLiveCapture();
    m_isLive = true;
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_live = new LiveImpl();
    QAudioFormat fmt;
    fmt.setSampleRate(sampleRate);
    fmt.setChannelCount(channels);
    fmt.setSampleFormat(QAudioFormat::Int16);
    QAudioDevice dev = QMediaDevices::defaultAudioInput();
    if (dev.isNull()) return false;
    m_live->source = new QAudioSource(dev, fmt);
    m_live->device = m_live->source->start();
    if (!m_live->device) { stopLiveCapture(); return false; }
    // Prime buffers
    m_rawAudio.assign(m_waveSize, 0);
    return true;
#else
    Q_UNUSED(sampleRate); Q_UNUSED(channels);
    return false;
#endif
}

void AudioVisualizer::stopLiveCapture() {
#if __has_include(<QAudioSource>)
    if (m_live) {
        if (m_live->source) { m_live->source->stop(); delete m_live->source; }
        delete m_live; m_live = nullptr;
    }
#endif
    m_isLive = false;
}

bool AudioVisualizer::isLive() const { return m_isLive; }

void AudioVisualizer::onLiveData(const std::vector<int16_t>& chunk) {
    if (chunk.empty()) return;
    // Append to ring buffer
    m_rawAudio.insert(m_rawAudio.end(), chunk.begin(), chunk.end());
    if (m_rawAudio.size() > 8192) m_rawAudio.erase(m_rawAudio.begin(), m_rawAudio.begin() + (m_rawAudio.size() - 8192));
}

} // namespace WallpaperEngine::Audio
