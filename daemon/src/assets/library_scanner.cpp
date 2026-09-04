#include "library_scanner.h"
#include "pkg_reader.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QDebug>
#include <fstream>
#include <iostream>

namespace WallpaperEngine::Assets {

LibraryScanner::LibraryScanner(QObject* parent) : QObject(parent) {}

void LibraryScanner::addCustomDirectory(const QString& path) {
    if (!m_customDirectories.contains(path) && QDir(path).exists()) {
        m_customDirectories.append(path);
    }
}

QStringList LibraryScanner::findSteamLibraryPaths() const {
    QStringList candidates = {
        QDir::homePath() + QStringLiteral("/.steam/debian-installation"),
        QDir::homePath() + QStringLiteral("/.local/share/Steam"),
        QDir::homePath() + QStringLiteral("/.steam/steam"),
        QDir::homePath() + QStringLiteral("/.steam/root"),
        QDir::homePath() + QStringLiteral("/.var/app/com.valvesoftware.Steam/.steam/steam"),
        QDir::homePath() + QStringLiteral("/.var/app/com.valvesoftware.Steam/.local/share/Steam")
    };

    QStringList steamWorkshopPaths;

    for (const auto& base : candidates) {
        if (!QDir(base).exists()) continue;

        QString defaultWorkshop = base + QStringLiteral("/steamapps/workshop/content/431960");
        if (QDir(defaultWorkshop).exists() && !steamWorkshopPaths.contains(defaultWorkshop)) {
            steamWorkshopPaths.append(defaultWorkshop);
        }

        // Parse libraryfolders.vdf for extra drives
        QString vdfPath = base + QStringLiteral("/steamapps/libraryfolders.vdf");
        QFile vdfFile(vdfPath);
        if (vdfFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString content = QString::fromUtf8(vdfFile.readAll());
            QRegularExpression pathRegex(QStringLiteral("\"path\"\\s+\"([^\"]+)\""));
            auto matches = pathRegex.globalMatch(content);
            while (matches.hasNext()) {
                auto match = matches.next();
                QString libPath = match.captured(1);
                QString extraWorkshop = libPath + QStringLiteral("/steamapps/workshop/content/431960");
                if (QDir(extraWorkshop).exists() && !steamWorkshopPaths.contains(extraWorkshop)) {
                    steamWorkshopPaths.append(extraWorkshop);
                }
            }
        }
    }

    return steamWorkshopPaths;
}

void LibraryScanner::scanSteamLibraries() {
    QStringList paths = findSteamLibraryPaths();
    for (const auto& path : paths) {
        scanDirectory(path.toStdString(), false);
    }
}

void LibraryScanner::scanAll() {
    m_wallpapers.clear();

    // 1. Scan all Steam Workshop locations across all drives
    scanSteamLibraries();

    // 2. Scan user-defined custom directories
    for (const auto& customDir : m_customDirectories) {
        scanDirectory(customDir.toStdString(), true);
    }

    qInfo() << "LibraryScanner: Found" << m_wallpapers.size() << "wallpapers in total.";
    Q_EMIT scanCompleted(static_cast<int>(m_wallpapers.size()));
}

void LibraryScanner::scanDirectory(const std::filesystem::path& dirPath, bool isCustom) {
    if (!std::filesystem::exists(dirPath) || !std::filesystem::is_directory(dirPath)) {
        return;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
            if (entry.is_directory()) {
                parseWallpaperFolder(entry.path(), isCustom);
            } else if (entry.is_regular_file() && entry.path().extension() == ".pkg") {
                parsePkgFile(entry.path(), isCustom);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "LibraryScanner error in " << dirPath << ": " << e.what() << std::endl;
    }
}

void LibraryScanner::parseWallpaperFolder(const std::filesystem::path& folderPath, bool isCustom) {
    auto projectJsonPath = folderPath / "project.json";
    if (!std::filesystem::exists(projectJsonPath)) {
        return;
    }

    QFile file(QString::fromStdString(projectJsonPath.string()));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        return;
    }

    QJsonObject obj = doc.object();
    WallpaperItemMetadata item;
    item.id = QString::fromStdString(folderPath.filename().string());
    item.title = obj.value(QStringLiteral("title")).toString();
    if (item.title.isEmpty()) {
        item.title = item.id;
    }

    item.type = obj.value(QStringLiteral("type")).toString().toLower();
    item.description = obj.value(QStringLiteral("description")).toString();
    item.isCustomFolder = isCustom;

    // Tags
    QJsonArray tagsArr = obj.value(QStringLiteral("tags")).toArray();
    for (const auto& tagVal : tagsArr) {
        item.tags.append(tagVal.toString());
    }

    // Preview
    QString previewRel = obj.value(QStringLiteral("preview")).toString();
    if (!previewRel.isEmpty()) {
        item.previewPath = QString::fromStdString((folderPath / previewRel.toStdString()).string());
    } else {
        // Fallbacks
        for (const auto& ext : {".jpg", ".png", ".gif"}) {
            auto candidate = folderPath / ("preview" + std::string(ext));
            if (std::filesystem::exists(candidate)) {
                item.previewPath = QString::fromStdString(candidate.string());
                break;
            }
        }
    }

    // Source path
    auto scenePkg = folderPath / "scene.pkg";
    auto sceneJson = folderPath / "scene.json";
    if (std::filesystem::exists(scenePkg)) {
        item.sourcePath = QString::fromStdString(scenePkg.string());
    } else if (std::filesystem::exists(sceneJson)) {
        item.sourcePath = QString::fromStdString(sceneJson.string());
    } else {
        QString mainFile = obj.value(QStringLiteral("file")).toString();
        if (!mainFile.isEmpty()) {
            item.sourcePath = QString::fromStdString((folderPath / mainFile.toStdString()).string());
        }
    }

    // User properties
    QJsonObject generalObj = obj.value(QStringLiteral("general")).toObject();
    QJsonObject propsObj = generalObj.value(QStringLiteral("properties")).toObject();
    for (auto it = propsObj.begin(); it != propsObj.end(); ++it) {
        item.properties[it.key()] = it.value().toVariant();
    }

    m_wallpapers.push_back(std::move(item));
}

void LibraryScanner::parsePkgFile(const std::filesystem::path& pkgPath, bool isCustom) {
    PkgReader reader;
    if (!reader.open(pkgPath)) {
        return;
    }

    WallpaperItemMetadata item;
    item.id = QString::fromStdString(pkgPath.stem().string());
    item.sourcePath = QString::fromStdString(pkgPath.string());
    item.isCustomFolder = isCustom;
    item.type = QStringLiteral("scene");

    // Try reading project.json or scene.json inside PKG
    std::string projStr = reader.readTextFile("project.json");
    if (!projStr.empty()) {
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(projStr));
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            item.title = obj.value(QStringLiteral("title")).toString();
            item.description = obj.value(QStringLiteral("description")).toString();
            item.type = obj.value(QStringLiteral("type")).toString().toLower();
        }
    }

    if (item.title.isEmpty()) {
        std::string sceneStr = reader.readTextFile("scene.json");
        if (!sceneStr.empty()) {
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(sceneStr));
            if (doc.isObject()) {
                item.title = doc.object().value(QStringLiteral("general")).toObject().value(QStringLiteral("title")).toString();
            }
        }
    }

    if (item.title.isEmpty()) {
        item.title = item.id;
    }

    // Check if preview image exists in the same folder
    auto parentDir = pkgPath.parent_path();
    for (const auto& ext : {".jpg", ".png", ".gif"}) {
        auto candidate = parentDir / ("preview" + std::string(ext));
        if (std::filesystem::exists(candidate)) {
            item.previewPath = QString::fromStdString(candidate.string());
            break;
        }
    }

    m_wallpapers.push_back(std::move(item));
}

QVariantList LibraryScanner::getLibraryAsVariantList() const {
    QVariantList list;
    list.reserve(static_cast<int>(m_wallpapers.size()));

    for (const auto& item : m_wallpapers) {
        QVariantMap map;
        map[QStringLiteral("id")] = item.id;
        map[QStringLiteral("title")] = item.title;
        map[QStringLiteral("type")] = item.type;
        map[QStringLiteral("preview")] = item.previewPath;
        map[QStringLiteral("source")] = item.sourcePath;
        map[QStringLiteral("description")] = item.description;
        map[QStringLiteral("tags")] = item.tags;
        map[QStringLiteral("properties")] = item.properties;
        map[QStringLiteral("isCustom")] = item.isCustomFolder;
        list.append(map);
    }

    return list;
}

WallpaperItemMetadata LibraryScanner::getWallpaperById(const QString& id) const {
    for (const auto& item : m_wallpapers) {
        if (item.id == id || item.sourcePath == id) {
            return item;
        }
    }
    return {};
}

QStringList LibraryScanner::getTrustedDirectories() const {
    // The scanner's own view of library roots: every Steam workshop root it
    // scans plus user-registered custom directories.
    return findSteamLibraryPaths() + m_customDirectories;
}

} // namespace WallpaperEngine::Assets
