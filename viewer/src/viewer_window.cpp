#include "viewer_window.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDBusReply>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusUnixFileDescriptor>
#include <QDebug>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <climits>
#include <unistd.h>
#include <QMouseEvent>
#include <QPainter>

#if __has_include(<linux/dma-buf.h>)
#include <linux/dma-buf.h>
#else
struct dma_buf_sync {
    uint64_t flags;
};
#define DMA_BUF_SYNC_READ (1 << 0)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END (1 << 2)
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)
#endif

// Cache-synchronous access to the shared dma-buf; without START/END the
// reader can observe torn or stale rows on cached/imported buffers.
static void dmaBufSync(int fd, bool start) {
    if (fd < 0) return;
    struct dma_buf_sync sync {};
    sync.flags = DMA_BUF_SYNC_READ | (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END);
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

LiveViewport::LiveViewport(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setMinimumSize(480, 270);
    setStyleSheet(QStringLiteral("background-color: #111; border: 2px solid #27ae60; border-radius: 8px;"));
}

LiveViewport::~LiveViewport() {
    if (m_mappedPtr && m_mappedPtr != MAP_FAILED && m_size > 0) {
        munmap(m_mappedPtr, m_size);
        m_mappedPtr = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

void LiveViewport::mouseMoveEvent(QMouseEvent* event) {
    if (width() > 0 && height() > 0) {
        float normX = static_cast<float>(event->position().x()) / width();
        float normY = static_cast<float>(event->position().y()) / height();
        Q_EMIT mouseMoved(normX, normY);
    }
}

void LiveViewport::updateBuffer(int fd, uint32_t width, uint32_t height, uint32_t stride, size_t size) {
    if (m_mappedPtr && m_mappedPtr != MAP_FAILED && m_size > 0) {
        munmap(m_mappedPtr, m_size);
        m_mappedPtr = nullptr;
    }
    if (m_fd >= 0 && m_fd != fd) {
        ::close(m_fd);
    }

    m_fd = fd;
    m_width = width;
    m_height = height;
    m_stride = stride;
    m_size = size;

    // Validate the advertised geometry before mapping: the image wrap below
    // would otherwise read past the mapping (SIGBUS) on a size/stride mismatch.
    // All math in 64-bit; QImage stores dimensions as int.
    const size_t needed = size_t(stride) * height;
    if (m_fd >= 0 && m_size > 0 && width > 0 && height > 0 &&
        width <= static_cast<uint32_t>(INT_MAX) && height <= static_cast<uint32_t>(INT_MAX) &&
        stride >= size_t(width) * 4 && m_size >= needed) {
        m_mappedPtr = mmap(nullptr, m_size, PROT_READ, MAP_SHARED, m_fd, 0);
        if (m_mappedPtr == MAP_FAILED) {
            qWarning() << "mmap failed on dmabuf fd:" << m_fd;
            m_mappedPtr = nullptr;
        }
    } else {
        qWarning() << "rejecting dmabuf geometry w" << width << "h" << height
                   << "stride" << stride << "size" << m_size;
        m_mappedPtr = nullptr;
    }
    update();
}

void LiveViewport::triggerRedraw() {
    update();
}

void LiveViewport::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (m_mappedPtr && m_width > 0 && m_height > 0) {
        // Straight-alpha ARGB8888 bytes (B,G,R,A) are exported by the daemon;
        // copy out under dma-buf sync so the draw cannot race the writer and
        // the mapping is not read past its END. Format_ARGB32 rows are
        // host-order words, so big-endian hosts swap the B,G,R,A bytes.
        QImage frame(m_width, m_height, QImage::Format_ARGB32);
        dmaBufSync(m_fd, true);
        const auto* srcBase = static_cast<const uint8_t*>(m_mappedPtr);
        for (uint32_t y = 0; y < m_height; ++y) {
            const uint8_t* srcRow = srcBase + size_t(y) * m_stride;
            uint8_t* dstRow = frame.scanLine(static_cast<int>(y));
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
            for (uint32_t x = 0; x < m_width * 4; x += 4) {
                dstRow[x] = srcRow[x + 3];
                dstRow[x + 1] = srcRow[x + 2];
                dstRow[x + 2] = srcRow[x + 1];
                dstRow[x + 3] = srcRow[x];
            }
#else
            std::memcpy(dstRow, srcRow, size_t(m_width) * 4);
#endif
        }
        dmaBufSync(m_fd, false);
        QRect targetRect = rect().adjusted(4, 4, -4, -4);
        QImage scaled = frame.scaled(targetRect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        int x = (width() - scaled.width()) / 2;
        int y = (height() - scaled.height()) / 2;
        p.drawImage(x, y, scaled);
    } else {
        p.setPen(QColor(180, 180, 180));
        p.setFont(QFont(QStringLiteral("sans-serif"), 12));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("Awaiting Vulkan DmaBuf Frame from Daemon..."));
    }
}

ViewerWindow::ViewerWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Plasma Wallpaper Engine - Native Verification Viewer"));
    resize(900, 680);

    auto* centralWidget = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(centralWidget);

    auto* headerLabel = new QLabel(QStringLiteral("<h2>Wallpaper Engine Native Pipeline Diagnostic</h2>"), this);
    headerLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(headerLabel);

    m_statusLabel = new QLabel(QStringLiteral("Status: Connecting to DBus daemon..."), this);
    m_statusLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 13px; color: #3498db;"));
    m_statusLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_statusLabel);

    m_infoLabel = new QLabel(QStringLiteral("Buffer: Querying..."), this);
    m_infoLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_infoLabel);

    m_fpsLabel = new QLabel(QStringLiteral("Frames Received via IPC: 0"), this);
    m_fpsLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 12px; color: #2ecc71;"));
    m_fpsLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_fpsLabel);

    // Live viewport
    m_viewport = new LiveViewport(this);
    mainLayout->addWidget(m_viewport, 1);

    auto* btnLayout = new QHBoxLayout();
    auto* loadBtn = new QPushButton(QStringLiteral("Open .pkg Wallpaper Archive"), this);
    loadBtn->setFixedHeight(38);
    connect(loadBtn, &QPushButton::clicked, this, &ViewerWindow::openPkgFile);
    btnLayout->addWidget(loadBtn);

    auto* refreshBtn = new QPushButton(QStringLiteral("Reconnect DBus Daemon"), this);
    refreshBtn->setFixedHeight(38);
    connect(refreshBtn, &QPushButton::clicked, this, &ViewerWindow::checkConnection);
    btnLayout->addWidget(refreshBtn);

    mainLayout->addLayout(btnLayout);
    setCentralWidget(centralWidget);

    // Connect to DBus signals
    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.antigravity.WallpaperEngine"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.antigravity.WallpaperEngine"),
        QStringLiteral("frameReady"),
        this,
        SLOT(onFrameReady())
    );

    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.antigravity.WallpaperEngine"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.antigravity.WallpaperEngine"),
        QStringLiteral("wallpaperLoaded"),
        this,
        SLOT(onWallpaperLoaded(QString))
    );

    // The daemon reallocates the exportable buffer on resolution changes;
    // re-fetch fd/geometry instead of reading a stale mapping.
    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.antigravity.WallpaperEngine"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.antigravity.WallpaperEngine"),
        QStringLiteral("bufferResized"),
        this,
        SLOT(onBufferResized())
    );

    connect(m_viewport, &LiveViewport::mouseMoved, this, [](float x, float y) {
        QDBusInterface iface(
            QStringLiteral("org.antigravity.WallpaperEngine"),
            QStringLiteral("/WallpaperEngine"),
            QStringLiteral("org.antigravity.WallpaperEngine"),
            QDBusConnection::sessionBus()
        );
        if (iface.isValid()) {
            iface.asyncCall(QStringLiteral("setMousePosition"), x, y);
        }
    });

    ensureDaemonRunning();
    checkConnection();
}

void ViewerWindow::ensureDaemonRunning() {
    QDBusInterface iface(
        QStringLiteral("org.antigravity.WallpaperEngine"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.antigravity.WallpaperEngine"),
        QDBusConnection::sessionBus()
    );

    if (!iface.isValid()) {
        // Dynamic discovery only: alongside the viewer (covers the build
        // tree layout) and on PATH (covers packaged/user installs).
        const QString daemonName = QStringLiteral("plasma-wallpaper-engine-daemon");
        QString daemonBin = QStandardPaths::findExecutable(daemonName);
        if (daemonBin.isEmpty()) {
            const QStringList candidates = {
                QCoreApplication::applicationDirPath() + u'/' + daemonName,
                QCoreApplication::applicationDirPath() + QStringLiteral("/../daemon/") + daemonName,
            };
            for (const QString& candidate : candidates) {
                if (QFile::exists(candidate)) {
                    daemonBin = candidate;
                    break;
                }
            }
        }

        if (!daemonBin.isEmpty()) {
            qInfo() << "Spawning daemon:" << daemonBin;
            QProcess::startDetached(daemonBin, QStringList());
            QTimer::singleShot(500, this, &ViewerWindow::checkConnection);
        } else {
            qWarning() << "No daemon binary found next to the viewer or on PATH";
        }
    }
}

void ViewerWindow::checkConnection() {
    QDBusInterface iface(
        QStringLiteral("org.antigravity.WallpaperEngine"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.antigravity.WallpaperEngine"),
        QDBusConnection::sessionBus()
    );

    if (!iface.isValid()) {
        m_statusLabel->setText(QStringLiteral("Status: Daemon starting up / reconnecting..."));
        m_statusLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 13px; color: #e74c3c;"));
        QTimer::singleShot(800, this, [this]() {
            if (!m_pendingPath.isEmpty()) {
                loadPath(m_pendingPath);
            }
        });
        return;
    }

    QDBusReply<QVariantMap> reply = iface.call(QStringLiteral("getBufferInfo"));
    if (reply.isValid()) {
        auto map = reply.value();
        uint32_t width = map.value(QStringLiteral("width")).toUInt();
        uint32_t height = map.value(QStringLiteral("height")).toUInt();
        uint32_t stride = map.value(QStringLiteral("stride")).toUInt();
        size_t size = map.value(QStringLiteral("size")).toULongLong();

        QDBusReply<QDBusUnixFileDescriptor> fdReply = iface.call(QStringLiteral("getBufferFd"));
        // The reply object closes its fd when it goes out of scope — hand the
        // viewport a dup it owns for the lifetime of its mapping.
        int fd = -1;
        if (fdReply.isValid() && fdReply.value().isValid()) {
            fd = dup(fdReply.value().fileDescriptor());
        }

        m_statusLabel->setText(QStringLiteral("Status: Connected to Vulkan Engine (Zero-Copy DmaBuf Active)"));
        m_statusLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 13px; color: #2ecc71;"));

        m_infoLabel->setText(QStringLiteral("Active DmaBuf: %1x%2 | Stride: %3 bytes | Shared Linux FD: %4")
                             .arg(width).arg(height).arg(stride).arg(fd));

        if (m_viewport && fd >= 0) {
            m_viewport->updateBuffer(fd, width, height, stride, size);
        } else if (fd >= 0) {
            ::close(fd);
        }
    }

    if (!m_pendingPath.isEmpty()) {
        QString path = m_pendingPath;
        m_pendingPath.clear();
        loadPath(path);
    }
}

void ViewerWindow::loadPath(const QString& path) {
    if (path.isEmpty()) return;

    // The daemon only loads wallpapers from trusted library roots; registering
    // the picked file's directory keeps arbitrary user selections working.
    {
        QDBusInterface iface(
            QStringLiteral("org.antigravity.WallpaperEngine"),
            QStringLiteral("/WallpaperEngine"),
            QStringLiteral("org.antigravity.WallpaperEngine"),
            QDBusConnection::sessionBus());
        if (iface.isValid()) {
            const QString dir = QFileInfo(path).absolutePath();
            if (!dir.isEmpty()) {
                iface.call(QStringLiteral("registerTrustedDirectory"), dir);
            }
        }
    }

    QDBusInterface iface(
        QStringLiteral("org.antigravity.WallpaperEngine"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.antigravity.WallpaperEngine"),
        QDBusConnection::sessionBus()
    );

    if (!iface.isValid()) {
        m_pendingPath = path;
        ensureDaemonRunning();
        return;
    }

    qInfo() << "Viewer: Sending loadWallpaper call for:" << path;
    QDBusReply<bool> reply = iface.call(QStringLiteral("loadWallpaper"), path);
    if (reply.isValid() && reply.value()) {
        m_statusLabel->setText(QStringLiteral("Status: Loading %1...").arg(QFileInfo(path).fileName()));
        m_statusLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 13px; color: #3498db;"));
    } else {
        m_statusLabel->setText(QStringLiteral("Status: Daemon rejected wallpaper load."));
        m_statusLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 13px; color: #e74c3c;"));
    }
}

void ViewerWindow::onFrameReady() {
    m_frameCount++;
    m_fpsLabel->setText(QStringLiteral("Frames Received via IPC: %1 (60 FPS Vulkan Render Loop Active)").arg(m_frameCount));
    if (m_viewport) {
        m_viewport->triggerRedraw();
    }
}

void ViewerWindow::onWallpaperLoaded(const QString& title) {
    QString display = title.isEmpty() ? QStringLiteral("Scene Package") : title;
    m_statusLabel->setText(QStringLiteral("Status: Loaded '%1' successfully!").arg(display));
    m_statusLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 13px; color: #9b59b6;"));
    checkConnection();
}

void ViewerWindow::onBufferResized() {
    checkConnection();
}

void ViewerWindow::openPkgFile() {
    QSettings settings;
    QString startDir = settings.value(QStringLiteral("lastWallpaperDir")).toString();
    if (startDir.isEmpty() || !QDir(startDir).exists()) {
        startDir = QDir::homePath();
    }

    QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select Wallpaper Engine Archive"),
        startDir,
        QStringLiteral("Wallpaper Engine Files (*.pkg scene.json project.json);;All Files (*)")
    );

    if (!path.isEmpty()) {
        QSettings settings;
        settings.setValue(QStringLiteral("lastWallpaperDir"), QFileInfo(path).absolutePath());
        loadPath(path);
    }
}
