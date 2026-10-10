#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "common/managed_process.h"

namespace WallpaperEngine::Audio {

/**
 * Audio Visualizer - Extracts FFT spectrum data for wallpaper effects.
 *
 * Uses FFmpeg's FFT filter to extract frequency bands from audio.
 * Results are exposed to wallpapers via JS engine properties.
 */
class AudioVisualizer {
public:
    AudioVisualizer();
    ~AudioVisualizer();

    /**
     * Get spectrum data for a specific band.
     * @param band Index (0 = bass, 1 = low mid, etc.)
     * @return Amplitude 0.0f to 1.0f
     */
    float getBand(int band) const;

    /**
     * Get waveform data.
     * @param outBuffer Output buffer (must be >= waveSize samples)
     * @param maxSamples Maximum samples to write
     * @return Actual samples written
     */
    int getWaveform(float* outBuffer, int maxSamples) const;

    /**
     * Update visualization (call each frame).
     */
    void update();

    /**
     * Get number of frequency bands available.
     */
    int getBandCount() const { return m_bandCount; }

    /**
     * Get waveform size.
     */
    int getWaveSize() const { return m_waveSize; }

    /**
     * Get sample rate.
     */
    int getSampleRate() const { return m_sampleRate; }

    /**
     * Get current playback position in seconds.
     */
    float getCurrentTime() const { return m_currentTime; }

    /**
     * Get total duration in seconds.
     */
    float getDuration() const { return m_duration; }

    /**
     * Get volume (0.0 to 1.0).
     */
    float getVolume() const { return m_volume; }

    /**
     * Set volume (0.0 to 1.0).
     */
    void setVolume(float volume);

    // Live capture of the default sink's monitor via parec (PulseAudio/
    // PipeWire). Records system output only — never a microphone — and also
    // hears the wallpaper's own OST playback.
    bool startLiveCapture(int sampleRate = 44100, int channels = 1);
    void stopLiveCapture();
    bool isLive() const;

private:
    void computeFFT();
    void computeBands();
    void onLiveData(const std::vector<int16_t>& chunk);

    // Audio data
    std::vector<int16_t> m_rawAudio;
    int m_sampleRate = 44100;
    int m_channels = 2;
    float m_volume = 1.0f;

    // FFT results
    std::vector<float> m_spectrum;
    std::vector<float> m_waveform;
    int m_bandCount = 8;
    int m_waveSize = 2048;

    // Timing
    float m_currentTime = 0.0f;
    float m_duration = 0.0f;
    size_t m_playbackPos = 0;

    // Live capture state: owned subprocess for the default sink monitor.
    Common::ManagedProcess m_captureProcess;
    bool m_isLive = false;
};

} // namespace WallpaperEngine::Audio
