#pragma once

#include <QObject>
#include <QQuickView>
#include <QImage>
#include <QDBusInterface>
#include <QQuickImageProvider>
#include <QTimer>
#include <QMutex>
#include <QString>

// One wallpaper surface per compositor output. Each WallpaperLayer owns a
// QQuickView configured as a wlr-layer-shell background surface (via
// LayerShellQt) pinned to its screen, and pumps the daemon's exported
// DmaBuf for the matching output into a QQuickImageProvider.
class WallpaperLayer : public QObject {
    Q_OBJECT

public:
    // outputName must match a daemon output from getOutputs(); screen pins
    // the layer surface to that physical monitor.
    WallpaperLayer(const QString& outputName, QScreen* screen, QObject* parent = nullptr);
    ~WallpaperLayer() override;

    bool isValid() const { return m_valid; }

public Q_SLOTS:
    void onFrameReady();
    void onBufferResized();

private:
    class FrameProvider;

    void connectDbusSignals();
    bool refreshBufferInfo();
    void publishFrame();

    QString m_outputName;
    QQuickView* m_view = nullptr;
    QDBusInterface m_iface;

    FrameProvider* m_provider = nullptr;
    qint64 m_frameSerial = 0;

    // Buffer geometry from getBufferInfoForOutput; refetched when the daemon
    // reports bufferResized for this output.
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_stride = 0;
    size_t m_size = 0;

    qint64 m_lastPollMs = 0;
    bool m_valid = false;
};
