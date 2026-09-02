#pragma once

#include <QObject>
#include <QDBusAbstractAdaptor>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QVariantMap>
#include <QVariantList>
#include "../vulkan/vulkan_context.h"
#include "../assets/pkg_reader.h"
#include "../assets/library_scanner.h"
#include "../scene/scene_compositor.h"
#include "../audio/audio_player.h"

namespace WallpaperEngine::IPC {

class WallpaperService : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.antigravity.WallpaperEngine")

public:
    explicit WallpaperService(Render::VulkanContext* vulkanCtx, QObject* parent = nullptr);

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

    // Audio & playback control
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
    QVariantMap m_activeProperties;
    QString m_activeWallpaperId;
};

} // namespace WallpaperEngine::IPC
