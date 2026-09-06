#pragma once

#include <string>
#include <vector>
#include <QObject>
#include <QTimer>

#include "common/managed_process.h"

namespace WallpaperEngine::Audio {

class AudioPlayer : public QObject {
    Q_OBJECT

public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer();

    bool play(const std::vector<uint8_t>& audioBytes, const std::string& extension);
    void pause();
    void resume();
    void stop();

    void setVolume(int volumePercent);
    void setMuted(bool muted);
    void setMuteOnOtherAudio(bool enabled);
    void setMuteOnFullscreen(bool enabled);

    // Pause gate integration: called when the engine is paused so audio
    // can follow the engine pause state independently of the per-user
    // mute settings.
    void setEnginePaused(bool paused);
    bool isEnginePaused() const { return m_enginePaused; }

    int getVolume() const { return m_volume; }
    bool isMuted() const { return m_isMuted; }

    // Pause gate integration: ambient pause state the pause gate can set
    // so audio muting tracks engine pause independently from user mute toggles.
    // Pause gate integration: ambient pause state the pause gate can set
    // so audio muting tracks engine pause independently from user mute toggles.
    void setMuteAudioOnPause(bool enabled);
    bool isMuteAudioOnPause() const { return mutedAudioOnPause; }

private:
    bool mutedAudioOnPause = true;


private:
    bool mutedAudioOnPause = true;

private Q_SLOTS:
    void checkOtherAudioActivity();

private:
    Common::ManagedProcess m_process;
    Common::ManagedProcess m_audioCheckProcess;
    QTimer m_audioActivityTimer;

    std::string m_tempFilePath;
    int m_volume = 80;
    bool m_isMuted = false;
    bool m_isPaused = false;
    bool m_enginePaused = false;
    bool m_muteOnOtherAudio = true;
    bool m_muteOnFullscreen = true;    bool m_temporarilyMutedByOtherAudio = false;

private:
    bool mutedAudioOnPause = true;

    void cleanupTempFile();
    void applyVolume();
    bool startPlaybackProcess();
    void stopPlaybackProcess();

};

private:

    void cleanupTempFile();
    void applyVolume();
    bool startPlaybackProcess();
    void stopPlaybackProcess();
};

} // namespace WallpaperEngine::Audio
