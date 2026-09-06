#include <QGuiApplication>
#include <QCommandLineParser>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDBusConnection>
#include <QDebug>
#include <QRegularExpression>
#include <QFileInfo>
#include <algorithm>
#include <iostream>
#include "viewer_window.h"

static QDBusInterface* serviceInterface() {
    auto* iface = new QDBusInterface(
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QStringLiteral("/WallpaperEngine"),
        QStringLiteral("org.plasmawallpaperengine.Daemon"),
        QDBusConnection::sessionBus());
    if (!iface->isValid()) {
        delete iface;
        return nullptr;
    }
    return iface;
}

static void ensureDirectoryTrusted(QDBusInterface* iface, const QString& path) {
    const QString dir = QFileInfo(path).absolutePath();
    if (!dir.isEmpty()) {
        iface->call(QStringLiteral("registerTrustedDirectory"), dir);
    }
}

static QVariant parsePropertyValue(const QString& raw) {
    QString v = raw.trimmed();
    if (v.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) return true;
    if (v.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) return false;
    bool ok = false;
    qlonglong intVal = v.toLongLong(&ok);
    if (ok) return intVal;
    double dblVal = v.toDouble(&ok);
    if (ok) return dblVal;
    return v;
}

static int runListProperties(const QString& id) {
    QDBusInterface* iface = serviceInterface();
    if (!iface) {
        std::cerr << "Error: daemon not reachable on DBus (org.plasmawallpaperengine.Daemon)." << std::endl;
        return 1;
    }
    QDBusReply<QVariantMap> reply = iface->call(QStringLiteral("getWallpaperProperties"), id);
    if (!reply.isValid()) {
        std::cerr << "Error: getWallpaperProperties failed: "
                  << reply.error().message().toStdString() << std::endl;
        delete iface;
        return 1;
    }

    QVariantMap props = reply.value();
    if (props.isEmpty()) {
        std::cout << "No properties found for wallpaper '" << id.toStdString() << "'." << std::endl;
        delete iface;
        return 0;
    }

    QList<QString> keys = props.keys();
    std::sort(keys.begin(), keys.end());
    std::cout << "Properties for '" << id.toStdString() << "' (" << keys.size() << "):" << std::endl;
    for (const QString& key : keys) {
        std::cout << "  " << key.toStdString() << " = "
                  << props.value(key).toString().toStdString() << std::endl;
    }
    delete iface;
    return 0;
}

static int runSetProperty(const QString& keyValue, const QString& optionalPath) {
    int eq = keyValue.indexOf(u'=');
    if (eq <= 0) {
        std::cerr << "Error: --set-property expects key=value, got '" << keyValue.toStdString() << "'." << std::endl;
        return 1;
    }
    QString key = keyValue.left(eq).trimmed();
    QVariant value = parsePropertyValue(keyValue.mid(eq + 1));

    QDBusInterface* iface = serviceInterface();
    if (!iface) {
        std::cerr << "Error: daemon not reachable on DBus (org.plasmawallpaperengine.Daemon)." << std::endl;
        return 1;
    }

    if (!optionalPath.isEmpty()) {
        ensureDirectoryTrusted(iface, optionalPath);
        QDBusReply<bool> loadReply = iface->call(QStringLiteral("loadWallpaper"), optionalPath);
        if (!loadReply.isValid() || !loadReply.value()) {
            std::cerr << "Error: loadWallpaper failed for '" << optionalPath.toStdString() << "'." << std::endl;
            delete iface;
            return 1;
        }
        std::cout << "Loaded wallpaper '" << optionalPath.toStdString() << "'." << std::endl;
    }

    QDBusReply<void> reply = iface->call(QStringLiteral("setProperty"), key,
                                         QVariant::fromValue(QDBusVariant(value)));
    if (!reply.isValid()) {
        std::cerr << "Error: setProperty failed: " << reply.error().message().toStdString() << std::endl;
        delete iface;
        return 1;
    }

    std::cout << "Property set: " << key.toStdString() << " = "
              << value.toString().toStdString() << " (live reload dispatched)" << std::endl;
    delete iface;
    return 0;
}

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("plasma-wallpaper-engine-viewer"));
    app.setApplicationVersion(QStringLiteral("1.0.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Plasma Wallpaper Engine - Native Viewer"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("file"),
                                QStringLiteral("Optional path to .pkg, project.json, or workshop directory."));

    QCommandLineOption listPropsOption(
        QStringList() << QStringLiteral("list-properties"),
        QStringLiteral("Print all properties of wallpaper <id> and exit."),
        QStringLiteral("id"));
    parser.addOption(listPropsOption);

    QCommandLineOption setPropOption(
        QStringList() << QStringLiteral("set-property"),
        QStringLiteral("Set property <key=value> on the active wallpaper and exit."),
        QStringLiteral("key=value"));
    parser.addOption(setPropOption);

    parser.process(app);

    const QStringList args = parser.positionalArguments();
    QString initialFile;
    if (!args.isEmpty()) {
        initialFile = args.first();
    }

    // CLI-only modes: talk to daemon over D-Bus and exit without GUI.
    if (parser.isSet(listPropsOption)) {
        return runListProperties(parser.value(listPropsOption));
    }
    if (parser.isSet(setPropOption)) {
        return runSetProperty(parser.value(setPropOption), initialFile);
    }

    // GUI mode: show a QML window connected to the daemon.
    ViewerWindow window;
    if (!initialFile.isEmpty()) {
        window.loadPath(initialFile);
    }

    return app.exec();
}
