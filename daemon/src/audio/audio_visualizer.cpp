#include "audio_visualizer.h"
#include <QDebug>
#include <QProcess>
#include <algorithm>
#include <cmath>
#include <complex>

namespace WallpaperEngine::Audio {

AudioVisualizer::AudioVisualizer() : m_bandCount(8), m_waveSize(2048) {}

AudioVisualizer::~AudioVisualizer() { stopLiveCapture(); }

void AudioVisualizer::update() {
    if (m_rawAudio.empty() || m_volume <= 0.0f) {
        std::fill(m_spectrum.begin(), m_spectrum.end(), 0.0f);
        std::fill(m_waveform.begin(), m_waveform.end(), 0.0f);
        return;
    }
    // Size the window buffers on first update so all writes below stay
    // in-bounds.
    if (m_waveform.size() != static_cast<size_t>(m_waveSize)) {
        m_waveform.resize(m_waveSize, 0.0f);
    }
    if (m_spectrum.size() != static_cast<size_t>(m_bandCount)
        && m_spectrum.size() != static_cast<size_t>(m_waveSize / 2)) {
        m_spectrum.assign(m_bandCount, 0.0f);
    }

    if (m_isLive) {
        // Live mode: window = the most recent samples of the capture ring.
        const int avail = static_cast<int>(m_rawAudio.size());
        const int start = std::max(0, avail - m_waveSize);
        for (int i = 0; i < m_waveSize; ++i) {
            const int idx = start + i;
            m_waveform[i] = idx < avail ? static_cast<float>(m_rawAudio[idx]) / 32768.0f : 0.0f;
        }
        computeFFT();
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
    // Fallback when FFT band data is unavailable.
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
    if (m_waveform.size() != static_cast<size_t>(m_waveSize)) return;
    size_t n = m_waveform.size();
    std::vector<std::complex<float>> buf(n);
    // Hann window + copy
    for (size_t i = 0; i < n; ++i) {
        float w = 0.5f * (1.0f - std::cos(2.0f * 3.1415926535f * static_cast<float>(i) / static_cast<float>(n - 1)));
        buf[i] = std::complex<float>(m_waveform[i] * w, 0.0f);
    }
    fftRadix2(buf);
    // Magnitude spectrum (first half)
    std::vector<float> full(n / 2);
    for (size_t i = 0; i < n / 2; ++i) {
        float mag = std::abs(buf[i]) / static_cast<float>(n);
        // Log scale
        full[i] = std::clamp(mag * 50.0f, 0.0f, 1.0f);
    }
    // Aggregate into the bandCount-band summary (what getBand serves)
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
    // m_spectrum may be empty before update() ever ran (fresh daemon,
    // no capture): reading it would be out-of-bounds.
    if (band < 0 || band >= m_bandCount || band >= static_cast<int>(m_spectrum.size())) {
        return 0.0f;
    }
    return m_spectrum[band];
}

int AudioVisualizer::getWaveform(float* outBuffer, int maxSamples) const {
    const int count = std::min({maxSamples, m_waveSize, static_cast<int>(m_waveform.size())});
    if (count > 0) {
        std::memcpy(outBuffer, m_waveform.data(), static_cast<size_t>(count) * sizeof(float));
    }
    return count;
}

void AudioVisualizer::setVolume(float volume) {
    m_volume = std::clamp(volume, 0.0f, 1.0f);
}

bool AudioVisualizer::startLiveCapture(int sampleRate, int channels) {
    stopLiveCapture();

    // Resolve the default sink (PipeWire/Pulse). Recording a sink captures
    // its monitor — system output only, never a microphone — and also hears
    // the wallpaper's own OST playback.
    QProcess pactl;
    pactl.start(QStringLiteral("pactl"), QStringList{QStringLiteral("get-default-sink")});
    if (!pactl.waitForFinished(2000)) {
        qWarning() << "AudioVisualizer: pactl not available, live capture disabled";
        return false;
    }
    const QString sink = QString::fromUtf8(pactl.readAllStandardOutput()).trimmed();
    if (sink.isEmpty()) {
        qWarning() << "AudioVisualizer: no default sink found";
        return false;
    }

    // Primary: pw-record --raw (PipeWire native; parec hangs on this setup).
    QStringList args;
    args << QStringLiteral("--raw")
         << QStringLiteral("--format=s16")
         << QStringLiteral("--rate=%1").arg(sampleRate)
         << QStringLiteral("--channels=%1").arg(channels)
         << QStringLiteral("--target=%1").arg(sink)
         << QStringLiteral("-");
    if (m_captureProcess.start(QStringLiteral("pw-record"), args, 3000)) {
        // live
    } else {
        // Fallback: parec reading the sink's monitor source directly.
        qWarning() << "AudioVisualizer: pw-record unavailable, falling back to parec";
        const QString monitor = sink + QStringLiteral(".monitor");
        QStringList parecArgs;
        parecArgs << QStringLiteral("--format=s16le")
                  << QStringLiteral("--rate=%1").arg(sampleRate)
                  << QStringLiteral("--channels=%1").arg(channels)
                  << QStringLiteral("--device=%1").arg(monitor);
        if (!m_captureProcess.start(QStringLiteral("parec"), parecArgs, 3000)) {
            qWarning() << "AudioVisualizer: no capture backend available";
            stopLiveCapture();
            return false;
        }
    }

    m_sampleRate = sampleRate;
    m_channels = channels;
    m_rawAudio.assign(m_waveSize, 0);

    m_isLive = true;
    QObject::connect(m_captureProcess.process(), &QProcess::readyReadStandardOutput, m_captureProcess.process(), [this]() {
        QProcess* proc = m_captureProcess.process();
        if (!proc) return;
        const QByteArray chunk = proc->readAllStandardOutput();
        const int16_t* samples = reinterpret_cast<const int16_t*>(chunk.constData());
        onLiveData(std::vector<int16_t>(samples, samples + chunk.size() / sizeof(int16_t)));
    });
    qInfo() << "AudioVisualizer: live monitor capture started for sink" << sink;
    return true;
}

void AudioVisualizer::stopLiveCapture() {
    m_captureProcess.terminate();
    m_isLive = false;
}

bool AudioVisualizer::isLive() const { return m_isLive; }

void AudioVisualizer::onLiveData(const std::vector<int16_t>& chunk) {
    if (chunk.empty()) return;
    // Append to ring buffer. Keep a fixed-size window so the FFT path stays
    // predictable even when the capture process runs for a long time.
    m_rawAudio.insert(m_rawAudio.end(), chunk.begin(), chunk.end());
    if (m_rawAudio.size() > 8192) {
        m_rawAudio.erase(m_rawAudio.begin(), m_rawAudio.begin() + (m_rawAudio.size() - 8192));
    }
}

} // namespace WallpaperEngine::Audio
