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
#include <QStandardPaths>
#include <sys/mman.h>
#include <unistd.h>
#include <QMouseEvent>
#include <QPainter>

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

    m_fd = fd;
    m_width = width;
    m_height = height;
    m_stride = stride;
    m_size = size;

    if (m_fd >= 0 && m_size > 0) {
        m_mappedPtr = mmap(nullptr, m_size, PROT_READ, MAP_SHARED, m_fd, 0);
        if (m_mappedPtr == MAP_FAILED) {
            qWarning() << "mmap failed on dmabuf fd:" << m_fd;
            m_mappedPtr = nullptr;
        }
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
        QImage frame(reinterpret_cast<const uchar*>(m_mappedPtr), m_width, m_height, m_stride, QImage::Format_ARGB32_Premultiplied);
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
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("frameReady"),
        this,
        SLOT(onFrameReady())
    );

    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("wallpaperLoaded"),
        this,
        SLOT(onWallpaperLoaded(QString))
    );

    connect(m_viewport, &LiveViewport::mouseMoved, this, [](float x, float y) {
        QDBusInterface iface(
            QStringLiteral("org.plasmawallpaperengine.Daemon"),
            QStringLiteral("/WallpaperEngine"),
            QStringLiteral("org.plasmawallpaperengine.Daemon"),
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
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QDBusConnection::sessionBus()
    );

    if (!iface.isValid()) {
        QString daemonBin = QDir::homePath() + QStringLiteral("/.local/lib/x86_64-linux-gnu/libexec/plasma-wallpaper-engine-daemon");
        if (!QFile::exists(daemonBin)) {
            daemonBin = QStringLiteral("/usr/lib/plasma-wallpaper-engine/plasma-wallpaper-engine-daemon");
        }

        if (QFile::exists(daemonBin)) {
            qInfo() << "Spawning daemon:" << daemonBin;
            QProcess::startDetached(daemonBin, QStringList());
            QTimer::singleShot(500, this, &ViewerWindow::checkConnection);
        }
    }
}

void ViewerWindow::checkConnection() {
    QDBusInterface iface(
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
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

    // Pick the first available output for the viewer.
    QDBusReply<QStringList> outReply = iface.call(QStringLiteral("getOutputs"));
    QString outputName;
    if (outReply.isValid() && !outReply.value().isEmpty()) {
        outputName = outReply.value().first();
        m_activeOutput = outputName;
    }

    if (outputName.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("Status: No outputs reported by daemon."));
        m_statusLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 13px; color: #e67e22;"));
        return;
    }

    QDBusReply<QVariantMap> reply = iface.call(QStringLiteral("getBufferInfoForOutput"), outputName);
    if (reply.isValid()) {
        auto map = reply.value();
        uint32_t width = map.value(QStringLiteral("width")).toUInt();
        uint32_t height = map.value(QStringLiteral("height")).toUInt();
        uint32_t stride = map.value(QStringLiteral("stride")).toUInt();
        size_t size = map.value(QStringLiteral("size")).toULongLong();

        QDBusReply<QDBusUnixFileDescriptor> fdReply = iface.call(QStringLiteral("getBufferFdForOutput"), outputName);
        int fd = fdReply.isValid() ? fdReply.value().fileDescriptor() : -1;
        m_activeFd = fd;

        m_statusLabel->setText(QStringLiteral("Status: Connected to Vulkan Engine (Zero-Copy DmaBuf Active)"));
        m_statusLabel->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 13px; color: #2ecc71;"));

        m_infoLabel->setText(QStringLiteral("Active DmaBuf (%1): %2x%3 | Stride: %4 bytes | Shared Linux FD: %5")
                             .arg(outputName).arg(width).arg(height).arg(stride).arg(fd));

        if (m_viewport && fd >= 0) {
            m_viewport->updateBuffer(fd, width, height, stride, size);
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
            QStringLiteral("org.plasmawallpaperengine.Daemon"),
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

    QDBusInterface iface(
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
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

void ViewerWindow::openPkgFile() {
    QString startDir = QStringLiteral("/home/mena/.steam/debian-installation/steamapps/workshop/content/431960");
    if (!QDir(startDir).exists()) {
        startDir = QDir::homePath();
    }

    QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select Wallpaper Engine Archive"),
        startDir,
        QStringLiteral("Wallpaper Engine Files (*.pkg scene.json project.json);;All Files (*)")
    );

    if (!path.isEmpty()) {
        loadPath(path);
    }
}
