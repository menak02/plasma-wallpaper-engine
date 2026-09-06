#pragma once

#include <QObject>
#include <QQuickView>
#include <QImage>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusConnection>
#include <QDBusUnixFileDescriptor>
#include <QProcess>
#include <QTimer>
#include <QDir>
#include <QFileInfo>

class ViewerWindow : public QObject {
    Q_OBJECT

public:
    explicit ViewerWindow(QObject* parent = nullptr);
    ~ViewerWindow() override = default;

    void loadPath(const QString& path);

public Q_SLOTS:
    void checkConnection();
    void openPkgFile();
    void onFrameReady();
    void onWallpaperLoaded(const QString& title);

private:
    void setStatus(const QString& text, const QString& color);
    void setInfo(const QString& text);
    void ensureDaemonRunning();
    void connectDbusSignals();
    void pollRootItem();

    QQuickView* m_view = nullptr;
    QObject* m_rootItem = nullptr;
    QDBusInterface m_iface;
    QString m_activeOutput;
    QString m_pendingPath;

    QImage m_currentFrame;
};
