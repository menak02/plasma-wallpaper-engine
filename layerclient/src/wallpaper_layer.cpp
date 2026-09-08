#include "wallpaper_layer.h"

#include <QQuickItem>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlContext>
#include <QDBusReply>
#include <QDBusConnection>
#include <QDBusUnixFileDescriptor>
#include <QDateTime>
#include <QDebug>

#include <LayerShellQt/Window>

#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>

// Per-output frame publisher. QQuickImageProvider hands the QML Image
// element a QImage per request; the serial in the source URL busts the
// cache every frame.
class WallpaperLayer::FrameProvider final : public QQuickImageProvider {
public:
    FrameProvider()
        : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override {
        Q_UNUSED(id);
        Q_UNUSED(requestedSize);
        const QMutexLocker locker(&m_mutex);
        if (size && !m_frame.isNull()) {
            *size = m_frame.size();
        }
        return m_frame;
    }

    void publish(const QImage& frame) {
        const QMutexLocker locker(&m_mutex);
        m_frame = frame;
    }

private:
    QMutex m_mutex;
    QImage m_frame;
};

// QML scene for one output: a black root with an Image filling it. The
// controller swaps the source URL each frame; fillMode Stretch matches the
// daemon behavior of center-crop-scaling the scene canvas to the exact
// output resolution, so the buffer already has the output's aspect ratio.
static const char* kLayerQml = R"(
import QtQuick

Rectangle {
    id: root
    color: "#000"

    Image {
        id: viewport
        objectName: "viewport"
        anchors.fill: parent
        fillMode: Image.Stretch
        asynchronous: false
        cache: false
        source: ""
    }
}
)";

WallpaperLayer::WallpaperLayer(const QString& outputName, QScreen* screen, QObject* parent)
    : QObject(parent)
    , m_outputName(outputName)
    , m_iface(QStringLiteral("org.plasmawallpaperengine.Daemon"),
              QStringLiteral("/WallpaperEngine"),
              QStringLiteral("org.plasmawallpaperengine.Daemon"),
              QDBusConnection::sessionBus())
    , m_provider(new FrameProvider())
{
    if (!m_iface.isValid()) {
        qWarning() << "LayerClient:" << m_outputName
                   << "daemon not reachable on D-Bus; layer will stay black";
    }

    m_view = new QQuickView();
    m_view->setResizeMode(QQuickView::SizeRootObjectToView);

    // Layer-shell configuration must happen before the window is shown /
    // exposed: once the surface is created the compositor has already
    // assigned it a shell role. QQuickView is-a QWindow, so it goes straight
    // into Window::get.
    if (auto* shell = LayerShellQt::Window::get(m_view)) {
        using Layer = LayerShellQt::Window::Layer;
        using Anchor = LayerShellQt::Window::Anchor;
        shell->setLayer(Layer::LayerBackground);
        shell->setExclusiveZone(-1);   // ignore all exclusions (panels, bars)
        shell->setMargins(QMargins(0, 0, 0, 0));
        LayerShellQt::Window::Anchors anchors;
        anchors.setFlag(Anchor::AnchorTop);
        anchors.setFlag(Anchor::AnchorBottom);
        anchors.setFlag(Anchor::AnchorLeft);
        anchors.setFlag(Anchor::AnchorRight);
        shell->setAnchors(anchors);
        shell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        shell->setScope(QStringLiteral("wallpaper"));
        if (screen) {
            shell->setScreen(screen);
        }
    } else {
        qWarning() << "LayerClient:" << m_outputName
                   << "LayerShellQt::Window::get returned null (not a Wayland"
                   << "layer-shell platform?) — window will behave like a normal toplevel";
    }

    m_view->engine()->addImageProvider(QStringLiteral("frame"), m_provider);
    m_view->setSource(QUrl::fromLocalFile(QStringLiteral(LAYER_QML_SOURCE_DIR) + QStringLiteral("/wallpaper_layer.qml")));
    m_view->engine()->rootContext()->setContextProperty(QStringLiteral("layerOutput"), m_outputName);

    m_view->showFullScreen();

    connectDbusSignals();

    m_valid = refreshBufferInfo();
    if (m_valid) {
        qDebug() << "LayerClient: layer for" << m_outputName << "ready at"
                 << m_width << "x" << m_height;
    }
}

WallpaperLayer::~WallpaperLayer()
{
    delete m_view;
}

void WallpaperLayer::connectDbusSignals()
{
    // Per-output repaint cadence: the daemon emits one frameReady() per
    // frame for all outputs; each layer throttles to ~30fps locally.
    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("frameReady"),
        this,
        SLOT(onFrameReady()));

    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("bufferResized(uint,uint)"),
        this,
        SLOT(onBufferResized()));
}

void WallpaperLayer::onBufferResized()
{
    // Buffer geometry may have changed; refetch before repainting.
    refreshBufferInfo();
}

bool WallpaperLayer::refreshBufferInfo()
{
    if (!m_iface.isValid()) {
        return false;
    }

    QDBusReply<QVariantMap> infoReply =
        m_iface.call(QStringLiteral("getBufferInfoForOutput"), m_outputName);
    if (!infoReply.isValid()) {
        qWarning() << "LayerClient:" << m_outputName
                   << "getBufferInfoForOutput failed:" << infoReply.error().message();
        return false;
    }

    const QVariantMap info = infoReply.value();
    m_width = info.value(QStringLiteral("width")).toUInt();
    m_height = info.value(QStringLiteral("height")).toUInt();
    m_stride = info.value(QStringLiteral("stride")).toUInt();
    m_size = info.value(QStringLiteral("size")).toULongLong();
    return m_width > 0 && m_height > 0 && m_size > 0;
}

void WallpaperLayer::onFrameReady()
{
    if (!m_iface.isValid() || m_width == 0 || m_height == 0) {
        return;
    }

    // ~30fps repaint cap; the daemon runs at 60 and this client is a
    // background layer where the extra smoothness is not worth the mmap
    // + memcpy cost per frame.
    const qint64 now = QDateTime::currentDateTime().toMSecsSinceEpoch();
    if (now - m_lastPollMs < 33) {
        return;
    }
    m_lastPollMs = now;

    publishFrame();
}

void WallpaperLayer::publishFrame()
{
    QDBusReply<QDBusUnixFileDescriptor> fdReply =
        m_iface.call(QStringLiteral("getBufferFdForOutput"), m_outputName);
    const int fd = fdReply.isValid() ? fdReply.value().fileDescriptor() : -1;
    if (fd < 0) {
        return;
    }

    void* ptr = mmap(nullptr, m_size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        qWarning() << "LayerClient:" << m_outputName << "mmap failed:" << strerror(errno);
        close(fd);
        return;
    }

    // Daemon exports DRM_FORMAT_ARGB8888 (little-endian BGRA in memory),
    // byte-identical to QImage::Format_ARGB32_Premultiplied.
    const QImage frame(static_cast<const uchar*>(ptr), static_cast<int>(m_width),
                       static_cast<int>(m_height), static_cast<qsizetype>(m_stride),
                       QImage::Format_ARGB32_Premultiplied);
    if (!frame.isNull()) {
        m_provider->publish(frame.copy());
        if (QObject* root = m_view->rootObject()) {
            if (QQuickItem* viewport = root->findChild<QQuickItem*>(QStringLiteral("viewport"))) {
                viewport->setProperty("source",
                    QUrl(QStringLiteral("image://frame/%1").arg(++m_frameSerial)));
            }
        }
    }

    munmap(ptr, m_size);
    close(fd);
}
