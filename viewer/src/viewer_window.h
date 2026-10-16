#pragma once

#include <QObject>
#include <QQuickView>
#include <QImage>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusConnection>
#include <QDBusUnixFileDescriptor>
#include <QQuickImageProvider>
#include <QTimer>
#include <QDir>
#include <QFileInfo>

// Streams the daemon's exported DmaBuf frames into the QML scene. The
// mmap'ed pixels are copied into a QImage and published through an
// QQuickImageProvider; the QML Image element requests "frame://<n>" each
// time a new frame lands so the scene graph re-uploads the texture.
class ViewerWindow : public QObject {
    Q_OBJECT

public:
    explicit ViewerWindow(QObject* parent = nullptr);
    ~ViewerWindow() override = default;

    void loadPath(const QString& path);

    // QML hover handler -> daemon setMousePosition (drives wallpaper parallax).
    Q_INVOKABLE void sendMousePosition(double normX, double normY);

public Q_SLOTS:
    void checkConnection();
    void openPkgFile();
    void onFrameReady();
    void onWallpaperLoaded(const QString& title);

private:
    class FrameProvider;

    void onSceneReady();
    void setStatus(const QString& text, const QString& color);
    void setInfo(const QString& text);
    void connectDbusSignals();
    void pullFrame();

    QQuickView* m_view = nullptr;
    QObject* m_rootItem = nullptr;
    QDBusInterface m_iface;
    QString m_activeOutput;
    QString m_pendingPath;

    // Provider + monotonically increasing request id. The id is appended to
    // the Image source URL so every frame is a cache-busting new request.
    FrameProvider* m_provider = nullptr;
    qint64 m_frameSerial = 0;

    // On-demand frame pump: single-shot timer rearmed after each pull, with
    // backoff when the daemon isn't rendering. frameReady nudges it to 0ms.
    QTimer* m_pumpTimer = nullptr;
};
