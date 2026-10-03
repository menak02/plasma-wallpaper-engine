#include "audio_player.h"
#include <QFile>
#include <QDir>
#include <iostream>
#include <algorithm>

#include <Qt>

namespace WallpaperEngine::Audio {

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent) {
    connect(&m_audioActivityTimer, &QTimer::timeout, this, &AudioPlayer::checkOtherAudioActivity);
    m_audioActivityTimer.setSingleShot(true);
    // Probe is lazy: only activated when audio playback actually starts
    // (OST or audio-reactive scene). See startAudioActivityProbe().
}

AudioPlayer::~AudioPlayer() {
    stopAudioActivityProbe();
    stop();
    cleanupTempFile();
}

void AudioPlayer::cleanupTempFile() {
    if (!m_tempFilePath.empty()) {
        QFile::remove(QString::fromStdString(m_tempFilePath));
        m_tempFilePath.clear();
    }
}

bool AudioPlayer::startPlaybackProcess() {
    if (m_tempFilePath.empty()) return false;

    QString tempPath = QString::fromStdString(m_tempFilePath);
    int effectiveVolume = (m_isMuted || m_enginePaused || mutedAudioOnPause) ? 0 : m_volume;

    QStringList args;
    args << QStringLiteral("-nodisp")
         << QStringLiteral("-loop") << QStringLiteral("0")
         << QStringLiteral("-volume") << QString::number(effectiveVolume)
         << tempPath;

    if (m_process.start(QStringLiteral("ffplay"), args, 1000)) {
        return true;
    }

    if (m_process.start(QStringLiteral("pw-play"), {tempPath}, 1000)) {
        return true;
    }

    return false;
}

void AudioPlayer::stopPlaybackProcess() {
    m_process.terminate();
}

bool AudioPlayer::play(const std::vector<uint8_t>& audioBytes, const std::string& extension) {
    stop();
    cleanupTempFile();

    if (audioBytes.empty() || m_isMuted) return false;

    QString tempPath = QDir::tempPath() + QStringLiteral("/plasma_we_bgm.") + QString::fromStdString(extension);
    QFile tempFile(tempPath);
    if (!tempFile.open(QIODevice::WriteOnly)) {
        return false;
    }

    tempFile.write(reinterpret_cast<const char*>(audioBytes.data()), audioBytes.size());
    tempFile.close();
    m_tempFilePath = tempPath.toStdString();

    if (!startPlaybackProcess()) {
        std::cerr << "AudioPlayer: failed to start playback process" << std::endl;
        cleanupTempFile();
        return false;
    }

    std::cout << "AudioPlayer: Playing background OST (" << audioBytes.size()
              << " bytes, Volume: " << (m_isMuted ? 0 : m_volume) << "%)" << std::endl;
    // Start audio activity probing so the mute-on-other-audio feature can
    // detect when another application starts playing sound.
    setAudioActive(true);
    return true;
}

void AudioPlayer::setAudioActive(bool active) {
    if (active) {
        startAudioActivityProbe();
    } else {
        stopAudioActivityProbe();
    }
}

void AudioPlayer::startAudioActivityProbe() {
    if (!m_muteOnOtherAudio) {
        return;
    }
    // Reset backoff on activation so the first probe uses the base interval.
    m_audioProbeIntervalMs = m_audioProbeBaseIntervalMs;
    m_audioActivityTimer.start(m_audioProbeIntervalMs);
}

void AudioPlayer::stopAudioActivityProbe() {
    m_audioActivityTimer.stop();
    m_audioCheckProcess.terminate(200);
}

void AudioPlayer::checkOtherAudioActivity() {
    if (!m_muteOnOtherAudio || m_isMuted || m_isPaused || m_enginePaused) {
        // Reschedule the next probe regardless so the timer keeps running
        // while audio is active (see startAudioActivityProbe).
        scheduleAudioProbe();
        return;
    }

    if (m_audioCheckProcess.isRunning()) {
        // A probe is already in flight; wait for it to finish before
        // launching another. The single-shot timer will fire again only
        // after we reschedule below.
        return;
    }

    // Start a one-shot probe. If pactl is unavailable or slow to start,
    // ManagedProcess::start returns false and we back off.
    if (!m_audioCheckProcess.start(QStringLiteral("pactl"),
                                   {QStringLiteral("list"),
                                    QStringLiteral("sink-inputs")},
                                   m_audioProbeTimeoutMs)) {
        // Back off on failure so a broken/missing pactl does not spin.
        m_audioProbeIntervalMs = std::min(m_audioProbeIntervalMs * 2,
                                          m_audioProbeMaxIntervalMs);
        scheduleAudioProbe();
        return;
    }

    // Wait for the probe to finish with a bounded timeout. If it times out
    // we treat it as "no other audio" and back off.
    if (!m_audioCheckProcess.process()->waitForFinished(m_audioProbeTimeoutMs)) {
        m_audioProbeIntervalMs = std::min(m_audioProbeIntervalMs * 2,
                                          m_audioProbeMaxIntervalMs);
        m_audioCheckProcess.terminate(200);
        scheduleAudioProbe();
        return;
    }

    // Probe succeeded — reset backoff to base interval.
    m_audioProbeIntervalMs = m_audioProbeBaseIntervalMs;

    QString output = QString::fromUtf8(
        m_audioCheckProcess.process()->readAllStandardOutput());

    // Count active running audio streams excluding ourselves
    int runningCount = 0;
    QStringList lines = output.split(QLatin1Char('\n'));
    for (const auto& line : lines) {
        if (line.contains(QStringLiteral("State: RUNNING"))) {
            runningCount++;
        }
    }

    // If another audio source is active (> 1 because the OST client counts as 1).
    if (runningCount > 1 && !m_temporarilyMutedByOtherAudio) {
        stopPlaybackProcess();
        m_temporarilyMutedByOtherAudio = true;
        std::cout << "AudioPlayer: Ducked wallpaper audio (other application is playing sound)" << std::endl;
    } else if (runningCount <= 1 && m_temporarilyMutedByOtherAudio) {
        m_temporarilyMutedByOtherAudio = false;
        if (!m_isMuted && !m_isPaused && !m_enginePaused) {
            startPlaybackProcess();
        }
        std::cout << "AudioPlayer: Resumed wallpaper audio" << std::endl;
    }

    m_audioCheckProcess.terminate(200);
    scheduleAudioProbe();
}

void AudioPlayer::scheduleAudioProbe() {
    m_audioActivityTimer.start(m_audioProbeIntervalMs);
}

void AudioPlayer::pause() {
    m_isPaused = true;
    stopPlaybackProcess();
}

void AudioPlayer::resume() {
    m_isPaused = false;
    if (!m_isMuted && !m_enginePaused) {
        startPlaybackProcess();
    }
}

void AudioPlayer::stop() {
    m_isPaused = false;
    m_enginePaused = false;
    m_temporarilyMutedByOtherAudio = false;
    stopPlaybackProcess();
    cleanupTempFile();
    // No audio is active anymore; stop probing.
    setAudioActive(false);
}

void AudioPlayer::setVolume(int volumePercent) {
    m_volume = std::clamp(volumePercent, 0, 100);
    if (!m_isMuted && !m_isPaused && !m_enginePaused) {
        stopPlaybackProcess();
        startPlaybackProcess();
    }
}

void AudioPlayer::setMuted(bool muted) {
    m_isMuted = muted;
    if (muted) {
        stopPlaybackProcess();
    } else if (!m_isPaused && !m_enginePaused) {
        startPlaybackProcess();
    }
}

void AudioPlayer::setMuteOnOtherAudio(bool enabled) {
    m_muteOnOtherAudio = enabled;
}

void AudioPlayer::setMuteOnFullscreen(bool enabled) {
    m_muteOnFullscreen = enabled;
}

void AudioPlayer::setEnginePaused(bool paused) {
    m_enginePaused = paused;
    if (paused) {
        stopPlaybackProcess();
    } else if (!m_isMuted && !m_isPaused) {
        startPlaybackProcess();
    }
}

// Pause gate integration: ambient pause state the pause gate can set
// so audio muting tracks engine pause independently from user mute toggles.
void AudioPlayer::setMuteAudioOnPause(bool enabled) {
    mutedAudioOnPause = enabled;
    // Re-evaluate current engine pause state with new setting.
    if (m_enginePaused && !m_isMuted && !m_isPaused) {
        startPlaybackProcess();
    }
}
} // namespace WallpaperEngine::Audio
