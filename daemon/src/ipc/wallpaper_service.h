#pragma once

#include <QObject>
#include <QDBusAbstractAdaptor>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QVariantMap>
#include <QVariantList>

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>

#include "../vulkan/vulkan_context.h"
#include "../assets/pkg_reader.h"
#include "../assets/library_scanner.h"
#include "../scene/scene_compositor.h"
#include "../scene/compositor_backend.h"
#include "../audio/audio_player.h"

namespace WallpaperEngine::IPC {

class WallpaperService : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.antigravity.WallpaperEngine")

public:
    explicit WallpaperService(Render::VulkanContext* vulkanCtx,
                              const QStringList& trustedDirs = {},
                              QObject* parent = nullptr);

    void updateAndRender(float dt, float time);

public Q_SLOTS:
    QDBusUnixFileDescriptor getBufferFd();
    QVariantMap getBufferInfo();
    bool loadWallpaper(const QString& path);
    void requestFrame();

    // Dynamic Resolution and GPU discovery
    bool setResolution(uint32_t width, uint32_t height);
    QVariantList getAvailableGpus();

    // Multi-output: per-output resolution + buffer export (wlr-layer-shell style)
    bool setResolutionForOutput(const QString& outputName, uint32_t width, uint32_t height);
    QDBusUnixFileDescriptor getBufferFdForOutput(const QString& outputName);
    QVariantMap getBufferInfoForOutput(const QString& outputName);
    QVariantList getOutputs();

    // Interactive mouse parallax
    void setMousePosition(float normX, float normY);

    // Pause gate control
    void setPauseEnabled(bool enabled);
    bool isPauseEnabled() const { return m_pauseEnabled.load(); }
    void setPauseAllOutputs(bool enabled);
    bool isPauseAllOutputs() const { return m_pauseAllOutputs.load(); }
    void setPauseCoverageThreshold(double threshold);
    double pauseCoverageThreshold() const { return m_pauseCoverageThreshold.load(); }
    void setPauseMuteAudio(bool muted);
    bool isPauseMuteAudio() const { return m_pauseMuteAudio.load(); }

    // Pause state queries for UI/diagnostics
    bool isOutputCovered(const QString& outputName) const;
    QVariantList getPauseState() const;
};

    // Audio & playback control
    bool startAudioCapture();
    void stopAudioCapture();
    QVariantList getAudioBands();
    void setAudioVolume(int volume);
    void setAudioMuted(bool muted);
    void setMuteOnOtherAudio(bool enabled);
    void setMuteOnFullscreen(bool enabled);
    QVariantMap getAudioSettings();

    void pause();
    void resume();
    void stop();

    // Library management
    QVariantList getLibrary();
    void scanLibrary();
    void addCustomLibraryPath(const QString& path);

    // Load-path trust management: loadWallpaper only accepts paths inside
    // trusted library roots (Steam workshop + custom dirs). A directory can
    // be trusted explicitly over D-Bus (or via --trusted-directory at startup).
    bool registerTrustedDirectory(const QString& dirPath);
    QStringList getTrustedDirectories();

    // Live property manipulation
    QVariantMap getWallpaperProperties(const QString& id);
    void setProperty(const QString& key, const QDBusVariant& value);
    QDBusVariant getProperty(const QString& key);

Q_SIGNALS:
    void frameReady();
    void wallpaperLoaded(const QString& title);
    void libraryUpdated(int count);
    void bufferResized(uint32_t width, uint32_t height);
    void propertyChanged(const QString& key, const QDBusVariant& value);

private:
    Render::VulkanContext* m_vulkanCtx = nullptr;
    Assets::PkgReader m_pkgReader;
    Assets::LibraryScanner m_libraryScanner;
    Scene::SceneCompositor m_compositor;
    Audio::AudioPlayer m_audioPlayer;
    std::unique_ptr<Scene::CompositorBackend> m_backend;

    QVariantMap m_activeProperties;
    QString m_activeWallpaperId;

    // Allowlist of canonical library roots for D-Bus loadWallpaper
    QStringList m_trustedDirs;

    // Pause gate state
    std::atomic<bool> m_pauseEnabled{false};
    std::atomic<bool> m_pauseAllOutputs{false};
    std::atomic<double> m_pauseCoverageThreshold{0.90};
    std::atomic<bool> m_pauseMuteAudio{true};

    QString canonicalizePath(const QString& path) const;
    bool isPathAllowed(const QString& canonicalPath) const;

    // Pause gate implementation
    bool shouldRenderThisFrame() const;
    bool isOutputCovered(const std::string& outputName) const;
    void updatePauseGate();
};

} // namespace WallpaperEngine::IPC

