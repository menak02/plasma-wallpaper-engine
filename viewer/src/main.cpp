#include <QApplication>
#include <QCommandLineParser>
#include "viewer_window.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("plasma-wallpaper-engine-viewer"));
    app.setApplicationVersion(QStringLiteral("1.0.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Plasma Wallpaper Engine - Interactive Standalone Viewer"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("Optional path to .pkg, project.json, or workshop directory."));
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    QString initialFile;
    if (!args.isEmpty()) {
        initialFile = args.first();
    }

    ViewerWindow window;
    window.show();

    if (!initialFile.isEmpty()) {
        window.loadPath(initialFile);
    }

    return app.exec();
}
