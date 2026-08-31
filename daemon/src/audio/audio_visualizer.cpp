#include "audio_visualizer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace WallpaperEngine::Audio {

AudioVisualizer::AudioVisualizer() : m_bandCount(8), m_waveSize(2048) {}

AudioVisualizer::~AudioVisualizer() {}

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
        // No audio or muted - reset
        std::fill(m_spectrum.begin(), m_spectrum.end(), 0.0f);
        std::fill(m_waveform.begin(), m_waveform.end(), 0.0f);
        return;
    }

    // Simple waveform extraction (in real implementation, would use FFT)
    // For now, just copy a window of raw audio
    int windowStart = m_playbackPos % m_rawAudio.size();
    int windowEnd = std::min(windowStart + m_waveSize, (int)m_rawAudio.size());

    for (int i = 0; i < m_waveSize; ++i) {
        if (windowStart + i < windowEnd) {
            // Convert int16 to float (-1.0 to 1.0)
            m_waveform[i] = static_cast<float>(m_rawAudio[windowStart + i]) / 32768.0f;
        } else {
            m_waveform[i] = 0.0f;
        }
    }

    // Advance playback position
    m_playbackPos = (m_playbackPos + m_waveSize / m_channels) % m_rawAudio.size();
    m_currentTime += static_cast<float>(m_waveSize) / (m_sampleRate * m_channels);
    if (m_currentTime >= m_duration) {
        m_currentTime = 0.0f;
        m_playbackPos = 0;
    }

    // Compute simple frequency bands from waveform energy
    computeBands();
}

void AudioVisualizer::computeBands() {
    // Simple energy-based band extraction
    // In a real implementation, this would use FFT
    int bandSize = m_waveSize / m_bandCount;

    for (int band = 0; band < m_bandCount; ++band) {
        float energy = 0.0f;
        int count = 0;

        int start = band * bandSize;
        int end = std::min(start + bandSize, m_waveSize);

        for (int i = start; i < end; ++i) {
            energy += m_waveform[i] * m_waveform[i];
            count++;
        }

        // Normalize to 0.0 - 1.0
        m_spectrum[band] = std::sqrt(energy / count) * m_volume;
    }
}

void AudioVisualizer::computeFFT() {
    // Placeholder for actual FFT implementation
    // Would use KissFFT or similar library
    // For now, use energy-based approximation
    computeBands();
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

} // namespace WallpaperEngine::Audio
