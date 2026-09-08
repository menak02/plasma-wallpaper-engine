#include <QGuiApplication>
#include <QCommandLineParser>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusConnection>
#include <QDebug>
#include <QScreen>
#include <QTimer>
#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>
#include "wallpaper_layer.h"

// Matches a daemon output name (from getOutputs()) to the QScreen that
// should carry the layer surface. Compositors disagree on naming: KWin
// exposes connector names ("eDP-1"), Hyprland exposes connector names too,
// but some builds report DRM paths or model strings, so fall back to
// case-insensitive contains() matching before giving up on geometry alone.
static QScreen* screenForOutput(const QString& outputName) {
    QScreen* exact = nullptr;
    QScreen* fuzzy = nullptr;
    for (QScreen* screen : QGuiApplication::screens()) {
        const QString name = screen->name();
        if (name == outputName) {
            exact = screen;
            break;
        }
        if (!fuzzy && name.contains(outputName, Qt::CaseInsensitive)) {
            fuzzy = screen;
        }
    }
    return exact ? exact : fuzzy;
}

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("plasma-wallpaper-engine-layerclient"));
    app.setApplicationVersion(QStringLiteral("1.0.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Plasma Wallpaper Engine - wlroots layer-shell client.\n"
                       "Renders the daemon's exported DmaBuf frames as a "
                       "layer-shell background surface on Wayland compositors "
                       "that support wlr-layer-shell (Hyprland, Sway, ...)."));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption outputsOption(
        QStringList() << QStringLiteral("output") << QStringLiteral("o"),
        QStringLiteral("Restrict layers to these outputs (repeatable). "
                       "Defaults to every daemon output."),
        QStringLiteral("name"));
    parser.addOption(outputsOption);

    parser.process(app);
    const QStringList wantedOutputs = parser.values(outputsOption);

    QDBusInterface iface(
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QDBusConnection::sessionBus());
    if (!iface.isValid()) {
        std::cerr << "Error: daemon not reachable on D-Bus "
                     "(org.plasmawallpaperengine.Daemon). Start the daemon first."
                  << std::endl;
        return 1;
    }

    QDBusReply<QStringList> outputsReply = iface.call(QStringLiteral("getOutputs"));
    if (!outputsReply.isValid() || outputsReply.value().isEmpty()) {
        std::cerr << "Error: daemon reports no outputs; nothing to render."
                  << std::endl;
        return 1;
    }

    QStringList outputs = outputsReply.value();
    if (!wantedOutputs.isEmpty()) {
        QStringList filtered;
        for (const QString& wanted : wantedOutputs) {
            for (const QString& candidate : outputs) {
                if (candidate == wanted ||
                    candidate.contains(wanted, Qt::CaseInsensitive)) {
                    filtered.append(candidate);
                }
            }
        }
        if (filtered.isEmpty()) {
            std::cerr << "Error: none of the requested outputs"
                      << " (" << wantedOutputs.join(QStringLiteral(", ")).toStdString()
                      << ") match daemon outputs"
                      << " (" << outputs.join(QStringLiteral(", ")).toStdString() << ")."
                      << std::endl;
            return 1;
        }
        outputs = filtered;
    }

    // One layer-shell surface per output, each pinned to its matching
    // QScreen so multi-monitor setups get one wallpaper per monitor.
    std::vector<std::unique_ptr<WallpaperLayer>> layers;
    for (const QString& outputName : outputs) {
        QScreen* screen = screenForOutput(outputName);
        if (!screen) {
            qWarning() << "LayerClient: no QScreen matches output" << outputName
                       << "- layer will land on the default screen";
        }
        auto layer = std::make_unique<WallpaperLayer>(outputName, screen);
        if (layer->isValid()) {
            layers.push_back(std::move(layer));
        } else {
            qWarning() << "LayerClient: output" << outputName
                       << "has no usable buffer yet; skipping layer";
        }
    }

    if (layers.empty()) {
        std::cerr << "Error: no usable layer surfaces created." << std::endl;
        return 1;
    }

    qInfo() << "LayerClient: rendering" << layers.size() << "layer(s):"
            << outputs.join(QStringLiteral(", "));

    return app.exec();
}
