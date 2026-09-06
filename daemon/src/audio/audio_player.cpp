#include "audio_player.h"
#include <QFile>
#include <QDir>
#include <iostream>

#include <Qt>

namespace WallpaperEngine::Audio {

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent) {
    connect(&m_audioActivityTimer, &QTimer::timeout, this, &AudioPlayer::checkOtherAudioActivity);
    m_audioActivityTimer.setInterval(1000); // Check every 1s
    m_audioActivityTimer.start();
}

AudioPlayer::~AudioPlayer() {
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
    return true;
}

void AudioPlayer::checkOtherAudioActivity() {
    if (!m_muteOnOtherAudio || m_isMuted || m_isPaused || m_enginePaused) {
        return;
    }

    if (!m_audioCheckProcess.isRunning()) {
        // Start a one-shot probe only when the timer fires and no probe is
        // already in flight. This bounds the process churn instead of
        // launching a new pactl every second unconditionally.
        if (m_audioCheckProcess.start(QStringLiteral("pactl"),
                                      {QStringLiteral("list"),
                                       QStringLiteral("sink-inputs")},
                                      2000)) {
            return;
        }
    }

    QString output = QString::fromUtf8(m_audioCheckProcess.process()->readAllStandardOutput());

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
