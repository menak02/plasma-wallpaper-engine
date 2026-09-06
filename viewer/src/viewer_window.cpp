#include "viewer_window.h"

#include <QGuiApplication>
#include <QQuickView>
#include <QQuickItem>
#include <QImage>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QStandardPaths>
#include <QFile>
#include <QDBusConnection>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>

ViewerWindow::ViewerWindow(QObject* parent)
    : QObject(parent)
    , m_iface(QStringLiteral("org.plasmawallpaperengine.Daemon"),
              QStringLiteral("/WallpaperEngine"),
              QStringLiteral("org.plasmawallpaperengine.Daemon"),
              QDBusConnection::sessionBus())
{
    FILE* vwlog = fopen("/tmp/vw_build.log", "w");
    if (vwlog) { fprintf(vwlog, "Viewer: constructor starting\n"); fflush(vwlog); }

    m_view = new QQuickView();
    m_view->setResizeMode(QQuickView::SizeRootObjectToView);
    m_view->setTitle(QStringLiteral("Plasma Wallpaper Engine - Native Viewer"));
    m_view->resize(960, 540);

    QString qmlPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                      QStringLiteral("/plasma_wallpaper_viewer.qml");
    QFile qmlFile(qmlPath);
    const QString qml = R"(
import QtQuick
import QtQuick.Controls

Rectangle {
    id: root
    color: "#111"
    width: 960; height: 540

    Column {
        anchors.fill: parent
        anchors.margins: 12; spacing: 8

        Text {
            id: statusText
            text: "Status: Connecting..."
            font.pixelSize: 14; color: "#3498db"
            anchors.horizontalCenter: parent.horizontalCenter
        }

        Text {
            id: infoText
            text: "Buffer: Querying..."
            font.pixelSize: 12; color: "#888"
            anchors.horizontalCenter: parent.horizontalCenter
        }

        Item {
            width: 640; height: 360
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.margins: 4

            Rectangle {
                anchors.fill: parent
                color: "#000"
                Image {
                    id: viewport
                    anchors.fill: parent
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    source: ""
                }
            }
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 12
            Button {
                text: "Reconnect"
                onClicked: viewerWindow.checkConnection()
            }
            Button {
                text: "Open .pkg"
                onClicked: viewerWindow.openPkgFile()
            }
        }
    }
}
)";
    if (qmlFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qmlFile.write(qml.toUtf8());
        qmlFile.close();
    }

    m_view->setSource(QUrl::fromLocalFile(qmlPath));

    if (vwlog) { fprintf(vwlog, "Viewer: setSource done\n"); fflush(vwlog); }

    m_view->show();

    if (vwlog) { fprintf(vwlog, "Viewer: window shown\n"); fflush(vwlog); }

    // Wait for the QML to load and find the root item
    QObject* root = m_view->rootObject();
    if (root) {
        m_rootItem = root;
        if (vwlog) { fprintf(vwlog, "Viewer: root object found\n"); fflush(vwlog); }
    } else {
        // Root object might not be available immediately; poll for it
        if (vwlog) { fprintf(vwlog, "Viewer: root object not yet available, polling\n"); fflush(vwlog); }
        QTimer::singleShot(100, this, &ViewerWindow::pollRootItem);
    }

    connectDbusSignals();
    if (vwlog) { fprintf(vwlog, "Viewer: connectDbusSignals returned\n"); fflush(vwlog); }
    ensureDaemonRunning();
    if (vwlog) { fprintf(vwlog, "Viewer: ensureDaemonRunning returned\n"); fflush(vwlog); }
    checkConnection();
    if (vwlog) { fprintf(vwlog, "Viewer: checkConnection returned\n"); fflush(vwlog); }
    if (vwlog) { fprintf(vwlog, "Viewer: constructor fully done\n"); fflush(vwlog); }
}

void ViewerWindow::pollRootItem()
{
    QObject* root = m_view->rootObject();
    if (root && !m_rootItem) {
        m_rootItem = root;
        FILE* vwlog = fopen("/tmp/vw_build.log", "a");
        if (vwlog) { fprintf(vwlog, "Viewer: root item found via poll\n"); fflush(vwlog); }
    }
}

void ViewerWindow::connectDbusSignals()
{
    FILE* vwlog = fopen("/tmp/vw_build.log", "a");
    if (vwlog) { fprintf(vwlog, "Viewer: connectDbusSignals start\n"); fflush(vwlog); }

    bool ok1 = QDBusConnection::sessionBus().connect(
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("frameReady"),
        this,
        SLOT(onFrameReady()));
    if (vwlog) { fprintf(vwlog, "Viewer: frameReady connect ok=%d\n", ok1 ? 1 : 0); fflush(vwlog); }

    bool ok2 = QDBusConnection::sessionBus().connect(
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("wallpaperLoaded"),
        this,
        SLOT(onWallpaperLoaded(QString)));
    if (vwlog) { fprintf(vwlog, "Viewer: wallpaperLoaded connect ok=%d\n", ok2 ? 1 : 0); fflush(vwlog); }
}

void ViewerWindow::ensureDaemonRunning()
{
    if (m_iface.isValid()) return;
    QTimer::singleShot(1500, this, &ViewerWindow::checkConnection);
}

void ViewerWindow::checkConnection()
{
    FILE* vwlog = fopen("/tmp/vw_build.log", "a");
    if (vwlog) { fprintf(vwlog, "Viewer: checkConnection start\n"); fflush(vwlog); }

    if (!m_iface.isValid()) {
        setStatus("Status: Daemon not reachable, waiting...", "#e74c3c");
        QTimer::singleShot(1500, this, &ViewerWindow::checkConnection);
        return;
    }

    QDBusReply<QStringList> outReply = m_iface.call(QStringLiteral("getOutputs"));
    if (vwlog) { fprintf(vwlog, "Viewer: getOutputs reply valid=%d\n", outReply.isValid() ? 1 : 0); fflush(vwlog); }
    QString outputName;
    if (outReply.isValid() && !outReply.value().isEmpty()) {
        outputName = outReply.value().first();
        m_activeOutput = outputName;
    }

    if (outputName.isEmpty()) {
        setStatus("Status: No outputs from daemon", "#e67e22");
        setInfo("No DmaBuf outputs available");
        return;
    }

    QDBusReply<QVariantMap> infoReply =
        m_iface.call(QStringLiteral("getBufferInfoForOutput"), outputName);
    if (!infoReply.isValid()) {
        setStatus("Status: getBufferInfoForOutput failed", "#e74c3c");
        setInfo(QString("Error: %1").arg(infoReply.error().message()));
        return;
    }

    auto info = infoReply.value();
    uint32_t width = info.value(QStringLiteral("width")).toUInt();
    uint32_t height = info.value(QStringLiteral("height")).toUInt();
    uint32_t stride = info.value(QStringLiteral("stride")).toUInt();
    size_t size = info.value(QStringLiteral("size")).toULongLong();

    QDBusReply<QDBusUnixFileDescriptor> fdReply =
        m_iface.call(QStringLiteral("getBufferFdForOutput"), outputName);
    int fd = fdReply.isValid() ? fdReply.value().fileDescriptor() : -1;

    setStatus(QString("Status: Connected — DmaBuf %1x%2, FD %3")
                  .arg(width).arg(height).arg(fd),
              "#2ecc71");
    setInfo(QString("Output: %1 | %2x%3 | Stride: %4 | Size: %5 bytes")
                .arg(outputName).arg(width).arg(height).arg(stride).arg(size));

    if (fd >= 0 && width > 0 && height > 0) {
        void* ptr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (ptr != MAP_FAILED) {
            m_currentFrame = QImage(static_cast<const uchar*>(ptr), width, height,
                                    stride, QImage::Format_ARGB32_Premultiplied);
            m_currentFrame = m_currentFrame.copy();
            munmap(ptr, size);
            close(fd);

            QObject* viewport = m_rootItem->findChild<QObject*>("viewport");
            if (viewport) {
                viewport->setProperty("source", QVariant());
            }
        } else {
            setInfo(QString("mmap failed for FD %1").arg(fd));
        }
    }
}

void ViewerWindow::onFrameReady()
{
    checkConnection();
}

void ViewerWindow::onWallpaperLoaded(const QString& title)
{
    QString display = title.isEmpty() ? QStringLiteral("Scene Package") : title;
    setStatus(QString("Status: Loaded '%1'!").arg(display), "#9b59b6");
    checkConnection();
}

void ViewerWindow::loadPath(const QString& path)
{
    if (path.isEmpty()) return;

    {
        QDBusInterface iface(QStringLiteral("org.plasmawallpaperengine.Daemon"),
                            QStringLiteral("/WallpaperEngine"),
                            QStringLiteral("org.plasmawallpaperengine.Daemon"),
                            QDBusConnection::sessionBus());
        if (iface.isValid()) {
            const QString dir = QFileInfo(path).absolutePath();
            if (!dir.isEmpty()) {
                iface.call(QStringLiteral("registerTrustedDirectory"), dir);
            }
        }
    }

    if (!m_iface.isValid()) {
        m_pendingPath = path;
        ensureDaemonRunning();
        return;
    }

    qDebug() << "Viewer: Loading" << path;
    QDBusReply<bool> reply = m_iface.call(QStringLiteral("loadWallpaper"), path);
    if (reply.isValid() && reply.value()) {
        setStatus(QString("Status: Loading %1...").arg(QFileInfo(path).fileName()),
                  "#3498db");
    } else {
        setStatus("Status: Daemon rejected load", "#e74c3c");
        qWarning() << "Viewer: loadWallpaper failed for" << path;
    }
}

void ViewerWindow::setStatus(const QString& text, const QString& color)
{
    if (!m_rootItem) return;
    QObject* statusText = m_rootItem->findChild<QObject*>("statusText");
    if (statusText) {
        statusText->setProperty("text", text);
        statusText->setProperty("color", color);
    }
}

void ViewerWindow::setInfo(const QString& text)
{
    if (!m_rootItem) return;
    QObject* infoText = m_rootItem->findChild<QObject*>("infoText");
    if (infoText) {
        infoText->setProperty("text", text);
    }
}

void ViewerWindow::openPkgFile()
{
    QDir startDir(QStringLiteral("/home/mena/.steam/debian-installation/steamapps/workshop/content/431960"));
    if (!startDir.exists()) {
        startDir = QDir::homePath();
    }
    qDebug() << "Viewer: Open Pkg clicked (file dialog requires QtWidgets)";
}
