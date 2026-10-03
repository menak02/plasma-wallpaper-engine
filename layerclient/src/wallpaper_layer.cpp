#include "wallpaper_layer.h"
#include "x11_desktop_window.h"

#include <QQuickItem>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlContext>
#include <QDBusReply>
#include <QDBusConnection>
#include <QDBusUnixFileDescriptor>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QWindow>

// LayerShellQt ships into a system include directory on most distros, so
// __has_include alone cannot decide whether the library is linkable. CMake
// defines PWE_HAVE_LAYERSHELL only when it actually resolved the package, so
// require both: the header is reachable *and* the library is there.
#if defined(PWE_HAVE_LAYERSHELL) && __has_include(<LayerShellQt/Window>)
#define PWE_WITH_LAYERSHELL 1
#include <LayerShellQt/Window>
#endif

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

WallpaperLayer::Backend WallpaperLayer::detectBackend()
{
    // Environment first, Qt platform second. A Wayland session started from
    // inside an X session (labwc launched by hand, a nested compositor) has
    // both WAYLAND_DISPLAY and the inherited DISPLAY set, and only layer-shell
    // actually works there, so Wayland must be tested first. The Qt platform
    // name is only a fallback for a session that exports neither.
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        return Backend::LayerShell;
    }
    if (!qEnvironmentVariableIsEmpty("DISPLAY")) {
        return Backend::X11Desktop;
    }

    const QString platform = QGuiApplication::platformName();
    if (platform.startsWith(QStringLiteral("wayland"), Qt::CaseInsensitive)) {
        return Backend::LayerShell;
    }
    if (platform == QLatin1String("xcb")) {
        return Backend::X11Desktop;
    }
    return Backend::Unknown;
}

const char* WallpaperLayer::backendName(Backend backend)
{
    switch (backend) {
    case Backend::LayerShell:
        return "wlr-layer-shell";
    case Backend::X11Desktop:
        return "X11 EWMH desktop window";
    case Backend::Unknown:
        break;
    }
    return "unknown";
}

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

    m_backend = detectBackend();
    if (m_backend == Backend::Unknown) {
        // Qt is running, so it is on some platform plugin; with neither
        // environment variable set, X11 is the only thing left to try.
        qWarning() << "LayerClient:" << m_outputName
                   << "neither WAYLAND_DISPLAY nor DISPLAY is set (Qt platform:"
                   << QGuiApplication::platformName() << "); assuming X11";
        m_backend = Backend::X11Desktop;
    }
    qInfo() << "LayerClient:" << m_outputName << "using the"
            << backendName(m_backend) << "path (Qt platform:"
            << QGuiApplication::platformName() << ")";

    m_screen = screen ? screen : QGuiApplication::primaryScreen();

    m_view = new QQuickView();
    m_view->setResizeMode(QQuickView::SizeRootObjectToView);

    m_view->engine()->addImageProvider(QStringLiteral("frame"), m_provider);
    m_view->setSource(QUrl::fromLocalFile(QStringLiteral(LAYER_QML_SOURCE_DIR) + QStringLiteral("/wallpaper_layer.qml")));
    m_view->engine()->rootContext()->setContextProperty(QStringLiteral("layerOutput"), m_outputName);

    if (m_backend == Backend::X11Desktop) {
        configureX11();
    } else {
        configureLayerShell();
        // Layer-shell surfaces are configured and sized by the compositor
        // from the anchors and exclusive zone, so the window only has to be
        // made visible.
        m_view->showFullScreen();
    }

    if (m_screen) {
        m_screenGeometryConn = connect(m_screen, &QScreen::geometryChanged,
                                       this, &WallpaperLayer::onScreenGeometryChanged);
    }

    connectDbusSignals();

    m_valid = refreshBufferInfo();
    if (m_valid) {
        qDebug() << "LayerClient: layer for" << m_outputName << "ready at"
                 << m_width << "x" << m_height;
    }
}

WallpaperLayer::~WallpaperLayer()
{
    // QQuickView must be destroyed first: X11DesktopWindow talks to the
    // window that lives inside it.
    delete m_view;
    m_view = nullptr;
    m_x11.reset();
}

void WallpaperLayer::configureLayerShell()
{
#if PWE_WITH_LAYERSHELL
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
        if (m_screen) {
            shell->setScreen(m_screen);
        }
    } else {
        // Reachable when the session is Wayland but the compositor does not
        // implement wlr-layer-shell. Showing the window anyway would put a
        // fullscreen toplevel on top of the user's desktop, so do not.
        qWarning() << "LayerClient:" << m_outputName
                   << "LayerShellQt::Window::get returned null — this compositor"
                   << "does not support wlr-layer-shell; leaving the window unmapped"
                   << "rather than covering the desktop with a normal toplevel";
    }
#else
    qWarning() << "LayerClient:" << m_outputName
               << "running on Wayland but this binary was built without"
               << "LayerShellQt, so no background surface can be requested;"
               << "leaving the window unmapped";
#endif
}

void WallpaperLayer::configureX11()
{
    if (!m_screen) {
        qWarning() << "LayerClient:" << m_outputName
                   << "no QScreen to pin the X11 desktop window to";
    }

    // Qt-side hints, set before the platform window is created so Qt makes
    // the window honour them from the start:
    //   FramelessWindowHint     - no title bar or resize frame around the
    //                             wallpaper,
    //   WindowStaysOnBottomHint - Qt maps this to _NET_WM_STATE_BELOW itself
    //                             and, more importantly, stops Qt from
    //                             raising the window when it takes focus,
    //   Tool                    - Qt classifies it as a transient utility
    //                             window, so it never lands in the taskbar.
    // EWMH properties set below remain authoritative; they are written
    // after create(), which is when Qt has already stamped its own
    // _NET_WM_WINDOW_TYPE_UTILITY onto the window.
    m_view->setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnBottomHint | Qt::Tool);
    if (m_screen) {
        m_view->setScreen(m_screen);
        m_view->setGeometry(m_screen->geometry());
    }

    m_x11 = std::make_unique<X11DesktopWindow>();
    if (!m_x11->isUsable()) {
        qWarning() << "LayerClient:" << m_outputName
                   << "no X11 connection available; cannot request a desktop-level"
                   << "window. Check that DISPLAY is set and the xcb platform"
                   << "plugin is installed.";
        m_x11.reset();
        return;
    }

    // Pre-map pass: _NET_WM_WINDOW_TYPE, _NET_WM_STATE and _NET_WM_DESKTOP
    // must exist before the window manager handles the MapRequest, otherwise
    // it places the window as an ordinary toplevel and never reconsiders.
    m_x11->configureBeforeMap(m_view);

    m_view->show();

    // Post-map pass, twice over, because the two requests are not
    // interchangeable:
    //  - The 0 ms post lands in the next event-loop iteration, by which point
    //    XMapWindow has been written to the connection. Because the state
    //    message goes out on the same connection, the server forwards it to
    //    the window manager after the MapRequest, so the state can never be
    //    applied to a window that does not exist yet.
    //  - The first Expose is the earliest point at which the window manager
    //    has genuinely mapped and stacked the window. xfwm4 recomputes
    //    placement around that moment, which is where a _NET_WM_STATE_BELOW
    //    request handled too early tends to get lost, so re-assert it there.
    QTimer::singleShot(0, this, &WallpaperLayer::applyX11State);
    m_view->installEventFilter(this);
}

void WallpaperLayer::applyX11State()
{
    if (!m_x11 || !m_view) {
        return;
    }
    m_x11->restackAfterMap(m_view);

    if (!m_x11StateApplied) {
        // Only the first expose is interesting; after that the window is
        // where it belongs and re-restacking on every expose would be noise.
        m_x11StateApplied = true;
        m_view->removeEventFilter(this);
    }
}

void WallpaperLayer::onScreenGeometryChanged()
{
    if (!m_view) {
        return;
    }
    if (m_screen) {
        m_view->setScreen(m_screen);
    }

    if (m_backend == Backend::LayerShell) {
        // Anchored layer surfaces are resized by the compositor; all we can
        // usefully do is follow the screen if it moved to another output.
        return;
    }

    // A resolution change (or a panel-free geometry change) has to be
    // followed by an explicit resize: nothing else moves the window, and a
    // resized window can be pushed back up the stacking order.
    if (m_screen) {
        m_view->setGeometry(m_screen->geometry());
    }
    if (m_x11) {
        m_x11->restackAfterMap(m_view);
    }
}

bool WallpaperLayer::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_view && event->type() == QEvent::Expose && m_x11) {
        applyX11State();
    }
    return QObject::eventFilter(watched, event);
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
