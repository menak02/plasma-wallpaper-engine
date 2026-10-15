#include "audio_player.h"
#include <QFile>
#include <QDir>
#include <QTemporaryFile>
#include <QStandardPaths>
#include <QDebug>
#include <iostream>
#include <csignal>

namespace WallpaperEngine::Audio {

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent) {
    m_process = new QProcess(this);
    m_audioCheckProcess = new QProcess(this);

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

bool AudioPlayer::play(const std::vector<uint8_t>& audioBytes, const std::string& extension) {
    stop();
    cleanupTempFile();

    if (audioBytes.empty() || m_isMuted) return false;

    // H3: QTemporaryFile creates a unique O_EXCL file (no symlink following),
    // so a hostile /tmp/plasma_we_bgm.* symlink can no longer clobber
    // arbitrary files. Sanitize the extension: it comes from scene data.
    QString suffix = QString::fromStdString(extension);
    for (const QChar c : suffix) {
        if (!c.isLetterOrNumber()) {
            suffix.clear();
            break;
        }
    }
    if (suffix.size() > 8) suffix.truncate(8);
    if (suffix.isEmpty()) suffix = QStringLiteral("bin");

    QTemporaryFile tempFile(QDir::tempPath() + QStringLiteral("/plasma_we_bgm.XXXXXX.") + suffix);
    tempFile.setAutoRemove(false);
    if (!tempFile.open()) {
        return false;
    }

    tempFile.write(reinterpret_cast<const char*>(audioBytes.data()), audioBytes.size());
    tempFile.close();
    QString tempPath = tempFile.fileName();
    m_tempFilePath = tempPath.toStdString();

    int effectiveVolume = m_isMuted ? 0 : m_volume;

    QStringList args;
    args << QStringLiteral("-nodisp")
         << QStringLiteral("-loop") << QStringLiteral("0")
         << QStringLiteral("-volume") << QString::number(effectiveVolume)
         << tempPath;

    m_process->start(QStringLiteral("ffplay"), args);
    if (!m_process->waitForStarted(1000)) {
        m_process->start(QStringLiteral("pw-play"), QStringList() << tempPath);
    }

    std::cout << "AudioPlayer: Playing background OST (" << audioBytes.size() 
              << " bytes, Volume: " << effectiveVolume << "%)" << std::endl;
    return true;
}

void AudioPlayer::checkOtherAudioActivity() {
    if (!m_muteOnOtherAudio || m_process->state() != QProcess::Running || m_isMuted) {
        return;
    }

    if (m_audioCheckProcess->state() != QProcess::NotRunning) return;

    // Check PulseAudio / PipeWire sink inputs state
    m_audioCheckProcess->start(QStringLiteral("pactl"), QStringList() << QStringLiteral("list") << QStringLiteral("sink-inputs"));
    m_audioCheckProcess->waitForFinished(500);

    QString output = QString::fromUtf8(m_audioCheckProcess->readAllStandardOutput());
    
    // Count active running audio streams excluding ourselves
    int runningCount = 0;
    QStringList lines = output.split(QLatin1Char('\n'));
    for (const auto& line : lines) {
        if (line.contains(QStringLiteral("State: RUNNING"))) {
            runningCount++;
        }
    }

    // If there is another audio source active (> 1 because ffplay is 1)
    if (runningCount > 1 && !m_temporarilyMutedByOtherAudio) {
        pause();
        m_temporarilyMutedByOtherAudio = true;
        std::cout << "AudioPlayer: Ducked wallpaper audio (other application is playing sound)" << std::endl;
    } else if (runningCount <= 1 && m_temporarilyMutedByOtherAudio) {
        resume();
        m_temporarilyMutedByOtherAudio = false;
        std::cout << "AudioPlayer: Resumed wallpaper audio" << std::endl;
    }
}

void AudioPlayer::pause() {
    if (m_process && m_process->state() == QProcess::Running) {
        // pid must be live and non-zero — kill(0, ...) would signal our own
        // process group if the child died between check and call.
        const qint64 pid = m_process->processId();
        if (pid > 0 && kill(static_cast<pid_t>(pid), SIGSTOP) == 0) {
            m_isPaused = true;
        }
    }
}

void AudioPlayer::resume() {
    if (m_process && m_process->state() == QProcess::Running && m_isPaused) {
        const qint64 pid = m_process->processId();
        if (pid > 0 && kill(static_cast<pid_t>(pid), SIGCONT) == 0) {
            m_isPaused = false;
        }
    }
}

void AudioPlayer::stop() {
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(500)) {
            m_process->kill();
        }
    }
    m_isPaused = false;
    m_temporarilyMutedByOtherAudio = false;
}

void AudioPlayer::setVolume(int volumePercent) {
    m_volume = std::clamp(volumePercent, 0, 100);
}

void AudioPlayer::setMuted(bool muted) {
    m_isMuted = muted;
    if (muted) {
        pause();
    } else {
        resume();
    }
}

void AudioPlayer::setMuteOnOtherAudio(bool enabled) {
    m_muteOnOtherAudio = enabled;
}

void AudioPlayer::setMuteOnFullscreen(bool enabled) {
    m_muteOnFullscreen = enabled;
}

} // namespace WallpaperEngine::Audio
