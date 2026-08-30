#include <QCoreApplication>
#include <QTimer>
#include <QDebug>
#include <QElapsedTimer>
#include <QDBusConnection>
#include <QDBusError>
#include <csignal>
#include <iostream>

#include "vulkan/vulkan_context.h"
#include "ipc/wallpaper_service.h"
#include "plugin/wallpaper_plugin.h"

volatile sig_atomic_t g_quitRequested = 0;

static void signalHandler(int signal) {
    g_quitRequested = 1;
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("plasma-wallpaper-engine-daemon"));
    app.setOrganizationDomain(QStringLiteral("org.antigravity"));

    qInfo() << "Starting Plasma Wallpaper Engine Daemon (Phase 3 Engine)...";

    // Initialize Vulkan device context
    WallpaperEngine::Render::VulkanContext vulkanCtx;
    if (!vulkanCtx.init()) {
        std::cerr << "Failed to initialize Vulkan Context." << std::endl;
        return 1;
    }

    // Initialize plugin system
    WallpaperEngine::Plugin::PluginRegistry::instance().loadBuiltinPlugins();
    qInfo() << "Plugin system initialized with"
            << WallpaperEngine::Plugin::PluginRegistry::instance().pluginCount()
            << "plugins registered";

    // Allocate default framebuffer for export (1920x1080)
    WallpaperEngine::Render::DmaBufBuffer buffer;
    if (!vulkanCtx.setResolution(1920, 1080, buffer)) {
        std::cerr << "Failed to allocate initial DmaBuf exportable buffer." << std::endl;
        return 1;
    }

    // Register DBus service on session bus
    WallpaperEngine::IPC::WallpaperService service(&vulkanCtx);
    QDBusConnection connection = QDBusConnection::sessionBus();

    if (!connection.registerService(QStringLiteral("org.antigravity.WallpaperEngine"))) {
        qWarning() << "Service already registered or failed:" << connection.lastError().message();
    }

    if (!connection.registerObject(QStringLiteral("/WallpaperEngine"), &service,
                                  QDBusConnection::ExportAllSlots |
                                  QDBusConnection::ExportAllSignals |
                                  QDBusConnection::ExportAllProperties)) {
        qCritical() << "Failed to register DBus object:" << connection.lastError().message();
        return 1;
    }

    qInfo() << "DBus service registered: org.antigravity.WallpaperEngine at /WallpaperEngine";

    // 60 FPS Simulation & Render Loop
    QTimer frameTimer;
    frameTimer.setInterval(16); // ~60 FPS

    QElapsedTimer elapsed;
    elapsed.start();
    qint64 lastTime = 0;

    QObject::connect(&frameTimer, &QTimer::timeout, [&]() {
        // Check if we have been requested to quit via signal
        if (g_quitRequested) {
            QCoreApplication::quit();
            return;
        }

        qint64 now = elapsed.elapsed();
        float dt = (now - lastTime) / 1000.0f;
        if (dt <= 0.0f || dt > 0.1f) dt = 0.0166f;
        lastTime = now;

        float timeSec = now / 1000.0f;

        // 1. Update multi-layer scene graph and particles
        service.updateAndRender(dt, timeSec);

        // 2. Execute Vulkan render pass into DmaBuf
        vulkanCtx.renderFrame(timeSec);

        // 3. Notify connected Plasma wallpaper / viewer
        service.requestFrame();
    });

    frameTimer.start();

    // Signal handlers using signal() and setting a flag (async-signal-safe)
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    return app.exec();
}