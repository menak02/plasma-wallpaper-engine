#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>
#include <QObject>
#include <vector>
#include <memory>
#include <filesystem>

namespace WallpaperEngine::Assets {

struct WallpaperItemMetadata {
    QString id;
    QString title;
    QString type; // "scene", "video", "web"
    QString previewPath;
    QString sourcePath; // path to scene.pkg, scene.json, etc.
    QString description;
    QStringList tags;
    QVariantMap properties;
    bool isCustomFolder = false;
};

class LibraryScanner : public QObject {
    Q_OBJECT

public:
    explicit LibraryScanner(QObject* parent = nullptr);
    ~LibraryScanner() override = default;

    void addCustomDirectory(const QString& path);
    void scanAll();

    QVariantList getLibraryAsVariantList() const;
    const std::vector<WallpaperItemMetadata>& getWallpapers() const { return m_wallpapers; }

    WallpaperItemMetadata getWallpaperById(const QString& id) const;

Q_SIGNALS:
    void scanCompleted(int count);

private:
    QStringList m_customDirectories;
    std::vector<WallpaperItemMetadata> m_wallpapers;

    void scanSteamLibraries();
    void scanDirectory(const std::filesystem::path& dirPath, bool isCustom = false);
    void parseWallpaperFolder(const std::filesystem::path& folderPath, bool isCustom = false);
    void parsePkgFile(const std::filesystem::path& pkgPath, bool isCustom = false);

    QStringList findSteamLibraryPaths();
};

} // namespace WallpaperEngine::Assets
