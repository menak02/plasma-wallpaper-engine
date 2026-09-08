#include "viewer_window.h"

#include <QGuiApplication>
#include <QQuickView>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlEngine>
#include <QQmlContext>
#include <QDebug>
#include <QStandardPaths>
#include <QFile>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QMutex>
#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>

// Publishes the most recent mmap'ed frame to QML. QQuickImageProvider hands
// the Image element a QImage per request; the viewer bumps a serial number
// in the source URL on every frameReady so the request always re-runs.
class ViewerWindow::FrameProvider final : public QQuickImageProvider {
public:
    explicit FrameProvider()
        : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override {
        Q_UNUSED(id);
        Q_UNUSED(requestedSize);
        QMutexLocker locker(&m_mutex);
        if (size && !m_frame.isNull()) {
            *size = m_frame.size();
        }
        return m_frame;
    }

    void publish(const QImage& frame) {
        QMutexLocker locker(&m_mutex);
        m_frame = frame;
    }

private:
    QMutex m_mutex;
    QImage m_frame;
};

ViewerWindow::ViewerWindow(QObject* parent)
    : QObject(parent)
    , m_iface(QStringLiteral("org.plasmawallpaperengine.Daemon"),
              QStringLiteral("/WallpaperEngine"),
              QStringLiteral("org.plasmawallpaperengine.Daemon"),
              QDBusConnection::sessionBus())
    , m_provider(new FrameProvider())
{
    m_view = new QQuickView();
    m_view->setResizeMode(QQuickView::SizeRootObjectToView);
    m_view->setTitle(QStringLiteral("Plasma Wallpaper Engine - Native Viewer"));
    m_view->resize(1280, 800);

    // Provider must be registered before the QML source loads so the first
    // Image request can already resolve the "frame" scheme.
    m_view->engine()->addImageProvider(QStringLiteral("frame"), m_provider);

    QString qmlPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                      QStringLiteral("/plasma_wallpaper_viewer.qml");
    QFile qmlFile(qmlPath);
    const QString qml = QStringLiteral(R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    color: "#111"
    width: 1280; height: 800

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Button {
                text: "Reconnect"
                onClicked: viewerWindow.checkConnection()
            }
            Button {
                text: "Load wallpaper"
                onClicked: viewerWindow.openPkgFile()
            }

            Text {
                id: statusText
                objectName: "statusText"
                text: "Status: Connecting..."
                font.pixelSize: 14
                color: "#3498db"
                Layout.alignment: Qt.AlignVCenter
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#000"
            clip: true

            Image {
                id: viewport
                objectName: "viewport"
                anchors.fill: parent
                fillMode: Image.PreserveAspectFit
                asynchronous: false
                cache: false
                source: ""
            }

            Text {
                id: infoText
                objectName: "infoText"
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.margins: 6
                text: "Buffer: Querying..."
                font.pixelSize: 12
                color: "#aaaaaa"
                style: Text.Outline
                styleColor: "#000000"
            }
        }

        // Feeds normalized cursor position to the daemon so scene mouse
        // parallax and hover effects respond like on a real desktop.
        MouseArea {
            id: pointerTracker
            anchors.fill: parent
            hoverEnabled: true
            onPositionChanged: (mouse) => {
                viewerWindow.sendMousePosition(mouse.x / width, mouse.y / height)
            }
            onExited: viewerWindow.sendMousePosition(0.5, 0.5)
        }
    }
}
)");
    if (qmlFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qmlFile.write(qml.toUtf8());
        qmlFile.close();
    }

    m_view->setSource(QUrl::fromLocalFile(qmlPath));

    // QML from a local file usually loads synchronously inside setSource,
    // so the Ready signal can fire before the connect() below is made.
    // Handle both orders: connect first, then also probe the current status.
    connect(m_view, &QQuickView::statusChanged, this, [this](QQuickView::Status status) {
        if (status == QQuickView::Status::Ready && !m_rootItem) {
            onSceneReady();
        }
    });
    if (m_view->status() == QQuickView::Status::Ready) {
        onSceneReady();
    }

    // Expose this controller to QML under the name the buttons call. The
    // context property must be set before the source loads so the QML
    // onClicked handlers resolve viewerWindow at compile time.
    m_view->engine()->rootContext()->setContextProperty(QStringLiteral("viewerWindow"), this);

    m_view->show();
}

// First point where the full QML item tree exists. D-Bus subscriptions and
// the first frame pump both start here.
void ViewerWindow::onSceneReady()
{
    if (m_rootItem) return;
    m_rootItem = m_view->rootObject();
    qDebug() << "Viewer: QML scene ready, root item captured";
    connectDbusSignals();
    checkConnection();
}

// QML ids are not discoverable via QObject::findChild; walk QQuickItem
// children recursively matching objectName instead.
static QQuickItem* findItem(QObject* root, const QString& objectName) {
    if (!root) return nullptr;
    for (QObject* child : root->findChildren<QObject*>()) {
        if (auto* item = qobject_cast<QQuickItem*>(child)) {
            if (item->objectName() == objectName) {
                return item;
            }
        }
    }
    return nullptr;
}

void ViewerWindow::setStatus(const QString& text, const QString& color) {
    if (QQuickItem* item = findItem(m_rootItem, QStringLiteral("statusText"))) {
        item->setProperty("text", text);
        item->setProperty("color", color);
    }
}

void ViewerWindow::setInfo(const QString& text) {
    if (QQuickItem* item = findItem(m_rootItem, QStringLiteral("infoText"))) {
        item->setProperty("text", text);
    }
}

void ViewerWindow::connectDbusSignals()
{
    const auto connectSignal = [this](const QString& signal, const char* slot) {
        return QDBusConnection::sessionBus().connect(
            QStringLiteral("org.plasmawallpaperengine.Daemon"),
            QStringLiteral("/WallpaperEngine"),
            QStringLiteral("org.plasmawallpaperengine.Daemon"),
            signal,
            this,
            slot);
    };

    const bool okFrame = connectSignal(QStringLiteral("frameReady"), SLOT(onFrameReady()));
    const bool okLoaded = connectSignal(QStringLiteral("wallpaperLoaded"), SLOT(onWallpaperLoaded(QString)));
    qDebug() << "Viewer: D-Bus signal subscriptions frameReady=" << okFrame
             << "wallpaperLoaded=" << okLoaded;
}

void ViewerWindow::checkConnection()
{
    if (!m_iface.isValid()) {
        setStatus(QStringLiteral("Status: Daemon not reachable, waiting..."), QStringLiteral("#e74c3c"));
        QTimer::singleShot(1500, this, &ViewerWindow::checkConnection);
        return;
    }

    // A load was requested before the daemon came up; replay it now.
    if (!m_pendingPath.isEmpty()) {
        const QString pending = m_pendingPath;
        m_pendingPath.clear();
        loadPath(pending);
    }

    QDBusReply<QStringList> outReply = m_iface.call(QStringLiteral("getOutputs"));
    QString outputName;
    if (outReply.isValid() && !outReply.value().isEmpty()) {
        outputName = outReply.value().first();
        m_activeOutput = outputName;
    }

    if (outputName.isEmpty()) {
        setStatus(QStringLiteral("Status: No outputs from daemon"), QStringLiteral("#e67e22"));
        setInfo(QStringLiteral("No DmaBuf outputs available"));
        return;
    }

    // First successful handshake: start the frame pump. From here on frames
    // are pulled on demand, never polled in a busy loop.
    if (!m_pumpTimer) {
        m_pumpTimer = new QTimer(this);
        m_pumpTimer->setSingleShot(true);
        connect(m_pumpTimer, &QTimer::timeout, this, &ViewerWindow::pullFrame);
        m_pumpTimer->start(0);
    }

    QDBusReply<QVariantMap> infoReply =
        m_iface.call(QStringLiteral("getBufferInfoForOutput"), outputName);
    if (!infoReply.isValid()) {
        setStatus(QStringLiteral("Status: getBufferInfoForOutput failed"), QStringLiteral("#e74c3c"));
        setInfo(infoReply.error().message());
        return;
    }

    const QVariantMap info = infoReply.value();
    const uint32_t width = info.value(QStringLiteral("width")).toUInt();
    const uint32_t height = info.value(QStringLiteral("height")).toUInt();
    const uint32_t stride = info.value(QStringLiteral("stride")).toUInt();
    const size_t size = info.value(QStringLiteral("size")).toULongLong();

    if (width == 0 || height == 0 || size == 0) {
        setStatus(QStringLiteral("Status: Connected — output empty"), QStringLiteral("#e67e22"));
        setInfo(QStringLiteral("Output: %1").arg(outputName));
        return;
    }

    QDBusReply<QDBusUnixFileDescriptor> fdReply =
        m_iface.call(QStringLiteral("getBufferFdForOutput"), outputName);
    const int fd = fdReply.isValid() ? fdReply.value().fileDescriptor() : -1;
    if (fd < 0) {
        setStatus(QStringLiteral("Status: Connected — no FD exported"), QStringLiteral("#e74c3c"));
        setInfo(QStringLiteral("Output: %1 | %2x%3 | no DMA-BUF FD").arg(outputName).arg(width).arg(height));
        return;
    }

    void* ptr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        setStatus(QStringLiteral("Status: mmap failed"), QStringLiteral("#e74c3c"));
        setInfo(QString("FD %1 (%2 bytes): %3").arg(fd).arg(size).arg(QString::fromUtf8(strerror(errno))));
        close(fd);
        return;
    }

    // The daemon exports DRM_FORMAT_ARGB8888 (little-endian BGRA in memory),
    // which is byte-identical to QImage::Format_ARGB32_Premultiplied.
    const QImage frame(static_cast<const uchar*>(ptr), static_cast<int>(width),
                       static_cast<int>(height), static_cast<qsizetype>(stride),
                       QImage::Format_ARGB32_Premultiplied);

    if (frame.isNull()) {
        setStatus(QStringLiteral("Status: frame decode failed"), QStringLiteral("#e74c3c"));
        munmap(ptr, size);
        close(fd);
        return;
    }

    m_provider->publish(frame.copy());
    munmap(ptr, size);
    close(fd);

    setStatus(QStringLiteral("Status: Connected — live"), QStringLiteral("#2ecc71"));
    setInfo(QStringLiteral("Output: %1 | %2x%3 | Stride: %4 | Size: %5 bytes")
                .arg(outputName).arg(width).arg(height).arg(stride).arg(size));

    // Bump the serial so the Image element treats this as a brand-new URL
    // and re-requests the frame from the provider.
    if (QQuickItem* viewport = findItem(m_rootItem, QStringLiteral("viewport"))) {
        viewport->setProperty("source",
            QUrl(QStringLiteral("image://frame/%1").arg(++m_frameSerial)));
    }
}

void ViewerWindow::pullFrame()
{
    if (!m_iface.isValid()) {
        m_pumpTimer->start(1500);
        return;
    }

    if (m_activeOutput.isEmpty()) {
        m_pumpTimer->start(500);
        return;
    }

    QDBusReply<QVariantMap> infoReply =
        m_iface.call(QStringLiteral("getBufferInfoForOutput"), m_activeOutput);
    if (!infoReply.isValid()) {
        m_pumpTimer->start(500);
        return;
    }

    const QVariantMap info = infoReply.value();
    const uint32_t width = info.value(QStringLiteral("width")).toUInt();
    const uint32_t height = info.value(QStringLiteral("height")).toUInt();
    const uint32_t stride = info.value(QStringLiteral("stride")).toUInt();
    const size_t size = info.value(QStringLiteral("size")).toULongLong();

    if (width == 0 || height == 0 || size == 0) {
        m_pumpTimer->start(500);
        return;
    }

    QDBusReply<QDBusUnixFileDescriptor> fdReply =
        m_iface.call(QStringLiteral("getBufferFdForOutput"), m_activeOutput);
    const int fd = fdReply.isValid() ? fdReply.value().fileDescriptor() : -1;
    if (fd < 0) {
        m_pumpTimer->start(500);
        return;
    }

    void* ptr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        setStatus(QStringLiteral("Status: mmap failed"), QStringLiteral("#e74c3c"));
        setInfo(QString("FD %1 (%2 bytes): %3").arg(fd).arg(size).arg(QString::fromUtf8(strerror(errno))));
        close(fd);
        m_pumpTimer->start(500);
        return;
    }

    // The daemon exports DRM_FORMAT_ARGB8888 (little-endian BGRA in memory),
    // which is byte-identical to QImage::Format_ARGB32_Premultiplied.
    const QImage frame(static_cast<const uchar*>(ptr), static_cast<int>(width),
                       static_cast<int>(height), static_cast<qsizetype>(stride),
                       QImage::Format_ARGB32_Premultiplied);

    if (frame.isNull()) {
        setStatus(QStringLiteral("Status: frame decode failed"), QStringLiteral("#e74c3c"));
        munmap(ptr, size);
        close(fd);
        m_pumpTimer->start(500);
        return;
    }

    m_provider->publish(frame.copy());
    munmap(ptr, size);
    close(fd);

    setStatus(QStringLiteral("Status: Connected — live"), QStringLiteral("#2ecc71"));
    setInfo(QStringLiteral("Output: %1 | %2x%3 | Stride: %4 | Size: %5 bytes")
                .arg(m_activeOutput).arg(width).arg(height).arg(stride).arg(size));

    // Bump the serial so the Image element treats this as a brand-new URL
    // and re-requests the frame from the provider.
    if (QQuickItem* viewport = findItem(m_rootItem, QStringLiteral("viewport"))) {
        viewport->setProperty("source",
            QUrl(QStringLiteral("image://frame/%1").arg(++m_frameSerial)));
    }

    // The daemon renders at 60fps; re-pull at ~20fps for preview. The CPU
    // cost here is a full-frame mmap copy + QML texture upload per pull.
    m_pumpTimer->start(50);
}

void ViewerWindow::onFrameReady()
{
    // Daemon-side render heartbeat. The D-Bus ping only nudges the pump —
    // actual buffer pulls happen on the timer, at ~20fps, with backoff when
    // nothing renders. Without the nudge the pump wakes at the same cadence
    // during long pauses and wastes cycles on identical frames.
    if (!m_pumpTimer) return;
    if (m_pumpTimer->isActive() && m_pumpTimer->remainingTime() <= 200) return;
    m_pumpTimer->start(0);
}

void ViewerWindow::onWallpaperLoaded(const QString& title)
{
    const QString display = title.isEmpty() ? QStringLiteral("Scene Package") : title;
    setStatus(QStringLiteral("Status: Loaded '%1'!").arg(display), QStringLiteral("#9b59b6"));
    checkConnection();
}

void ViewerWindow::loadPath(const QString& path)
{
    if (path.isEmpty()) return;

    if (!m_iface.isValid()) {
        m_pendingPath = path;
        QTimer::singleShot(1500, this, &ViewerWindow::checkConnection);
        return;
    }

    // Preview loads use the ephemeral variant: the daemon grants one-shot
    // trust for this wallpaper's directory and consumes it on this load, so
    // previewing never permanently widens the load allowlist.
    qDebug() << "Viewer: Loading" << path;
    QDBusReply<bool> reply = m_iface.call(QStringLiteral("loadWallpaperEphemeral"), path);
    if (reply.isValid() && reply.value()) {
        setStatus(QStringLiteral("Status: Loading %1...").arg(QFileInfo(path).fileName()),
                  QStringLiteral("#3498db"));
    } else {
        setStatus(QStringLiteral("Status: Daemon rejected load"), QStringLiteral("#e74c3c"));
        qWarning() << "Viewer: loadWallpaper failed for" << path
                   << (reply.isValid() ? QString() : reply.error().message());
    }
}

void ViewerWindow::sendMousePosition(double normX, double normY)
{
    if (!m_iface.isValid()) return;
    m_iface.call(QStringLiteral("setMousePosition"), normX, normY);
}

void ViewerWindow::openPkgFile()
{
    // Start in the Steam Workshop library when it exists so the common case
    // is two clicks away; the dialog can reach anything else on disk.
    const QString workshopDir = QDir::homePath() +
        QStringLiteral("/.steam/debian-installation/steamapps/workshop/content/431960");
    const QString startDir = QDir(workshopDir).exists() ? workshopDir : QDir::homePath();

    // Scene wallpapers ship as scene.pkg inside per-item workshop
    // subdirectories; standalone video wallpapers are selected via their
    // project.json. Both are accepted by the daemon's loadWallpaper.
    const QString pkgPath = QFileDialog::getOpenFileName(
        nullptr,
        QStringLiteral("Open Wallpaper Package"),
        startDir,
        QStringLiteral("Wallpaper packages (*.pkg *.json);;Scene packages (*.pkg);;All files (*)"));
    if (pkgPath.isEmpty()) {
        return;
    }

    qDebug() << "Viewer: Open Pkg clicked, loading" << pkgPath;
    loadPath(pkgPath);
}
