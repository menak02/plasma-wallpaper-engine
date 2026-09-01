#include "wallpaper_service.h"
#include "../assets/tex_parser.h"
#include "../assets/dxt_decoder.h"
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace WallpaperEngine::IPC {

WallpaperService::WallpaperService(Render::VulkanContext* vulkanCtx, QObject* parent)
    : QObject(parent), m_vulkanCtx(vulkanCtx), m_compositor(vulkanCtx) {
    connect(&m_libraryScanner, &Assets::LibraryScanner::scanCompleted, this, &WallpaperService::libraryUpdated);
    m_libraryScanner.scanAll();
}

void WallpaperService::updateAndRender(float dt, float time) {
    if (m_compositor.hasScene()) {
        m_compositor.updateAndRender(dt, time);
    }
}

QDBusUnixFileDescriptor WallpaperService::getBufferFd() {
    QDBusUnixFileDescriptor desc;
    if (m_vulkanCtx) {
        int fd = m_vulkanCtx->getCurrentBuffer().fd;
        if (fd >= 0) {
            desc.setFileDescriptor(fd);
        }
    }
    return desc;
}

QVariantMap WallpaperService::getBufferInfo() {
    QVariantMap map;
    if (m_vulkanCtx) {
        const auto& buf = m_vulkanCtx->getCurrentBuffer();
        map[QStringLiteral("width")] = buf.width;
        map[QStringLiteral("height")] = buf.height;
        map[QStringLiteral("stride")] = buf.stride;
        map[QStringLiteral("format")] = buf.format;
        map[QStringLiteral("size")] = static_cast<qulonglong>(buf.size);
    }
    return map;
}

bool WallpaperService::setResolution(uint32_t width, uint32_t height) {
    if (!m_vulkanCtx) return false;
    Render::DmaBufBuffer newBuf;
    if (m_vulkanCtx->setResolution(width, height, newBuf)) {
        m_compositor.setTargetResolution(width, height);
        Q_EMIT bufferResized(width, height);
        return true;
    }
    return false;
}

void WallpaperService::setMousePosition(float normX, float normY) {
    m_compositor.setMouseParallax(normX, normY);
}

void WallpaperService::setAudioVolume(int volume) {
    m_audioPlayer.setVolume(volume);
}

void WallpaperService::setAudioMuted(bool muted) {
    m_audioPlayer.setMuted(muted);
}

void WallpaperService::setMuteOnOtherAudio(bool enabled) {
    m_audioPlayer.setMuteOnOtherAudio(enabled);
}

void WallpaperService::setMuteOnFullscreen(bool enabled) {
    m_audioPlayer.setMuteOnFullscreen(enabled);
}

QVariantMap WallpaperService::getAudioSettings() {
    QVariantMap map;
    map[QStringLiteral("volume")] = m_audioPlayer.getVolume();
    map[QStringLiteral("isMuted")] = m_audioPlayer.isMuted();
    return map;
}

void WallpaperService::pause() {
    m_audioPlayer.pause();
}

void WallpaperService::resume() {
    m_audioPlayer.resume();
}

void WallpaperService::stop() {
    m_audioPlayer.stop();
}

QVariantList WallpaperService::getAvailableGpus() {
    QVariantList list;
    if (!m_vulkanCtx) return list;
    auto gpus = m_vulkanCtx->getAvailableGpus();
    for (const auto& gpu : gpus) {
        QVariantMap map;
        map[QStringLiteral("id")] = gpu.id;
        map[QStringLiteral("name")] = QString::fromStdString(gpu.name);
        map[QStringLiteral("isDiscrete")] = gpu.isDiscrete;
        map[QStringLiteral("hasDmaBuf")] = gpu.hasDmaBufSupport;
        list.append(map);
    }
    return list;
}

bool WallpaperService::loadWallpaper(const QString& path) {
    qInfo() << "WallpaperService: Loading wallpaper from:" << path;
    QFileInfo info(path);

    if (!info.exists()) {
        qWarning() << "Path does not exist:" << path;
        return false;
    }

    m_activeWallpaperId = path;
    QString title;

    // Web wallpaper detection: project.json file==index.html or path ends with .html
    auto isWebProject = [&](const std::string& projJsonStr) -> bool {
        if (projJsonStr.empty()) return false;
        QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(projJsonStr));
        if (!d.isObject()) return false;
        QString file = d.object().value(QStringLiteral("file")).toString();
        QString type = d.object().value(QStringLiteral("type")).toString().toLower();
        return file.endsWith(QStringLiteral(".html")) || type == QStringLiteral("web") || type == QStringLiteral("webwallpaper");
    };

    if (info.suffix().toLower() == QStringLiteral("pkg") || info.isDir()) {
        QString pkgFile = info.isDir() ? (path + QStringLiteral("/scene.pkg")) : path;
        
        if (QFile::exists(pkgFile) && m_pkgReader.open(pkgFile.toStdString())) {
            // Load Project metadata
            std::string projJson = m_pkgReader.readTextFile("project.json");
            if (!projJson.empty()) {
                QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(projJson));
                if (doc.isObject()) {
                    title = doc.object().value(QStringLiteral("title")).toString();
                    m_activeProperties = doc.object().value(QStringLiteral("general")).toObject().value(QStringLiteral("properties")).toObject().toVariantMap();
                }
            }

            // Web wallpaper path: extract index.html and render via WebWallpaper
            if (isWebProject(projJson)) {
                qInfo() << "WallpaperService: Web wallpaper detected, using WebWallpaper";
                std::string html = m_pkgReader.readTextFile("index.html");
                if (html.empty()) {
                    for (auto& f : m_pkgReader.listFiles()) {
                        if (f.ends_with(".html")) { html = m_pkgReader.readTextFile(f); if (!html.empty()) break; }
                    }
                }
                if (!html.empty()) {
                    if (m_compositor.loadWeb(html)) {
                        title = title.isEmpty() ? QString::fromStdString(m_compositor.getScene().title) : title;
                    }
                }
            } else {
                std::unordered_map<std::string, QVariant> ov;
                for (auto it = m_activeProperties.begin(); it != m_activeProperties.end(); ++it) ov[it.key().toStdString()] = it.value();
                if (m_compositor.loadScene(m_pkgReader, ov)) {
                    if (title.isEmpty()) {
                        title = QString::fromStdString(m_compositor.getScene().title);
                    }

                    // Check for background audio OST
                    std::string soundPath = m_compositor.getScene().soundPath;
                    if (!soundPath.empty()) {
                        auto soundBytes = m_pkgReader.readFile(soundPath);
                        if (!soundBytes.empty()) {
                            std::string ext = "mp3";
                            if (soundPath.ends_with(".ogg")) ext = "ogg";
                            else if (soundPath.ends_with(".wav")) ext = "wav";
                            else if (soundPath.ends_with(".flac")) ext = "flac";
                            m_audioPlayer.play(soundBytes, ext);
                        }
                    }
                }
            }
        }
    }

    if (title.isEmpty()) {
        title = info.baseName();
    }

    qInfo() << "WallpaperService: Successfully loaded wallpaper:" << title;
    Q_EMIT wallpaperLoaded(title);
    return true;
}

void WallpaperService::requestFrame() {
    Q_EMIT frameReady();
}

QVariantList WallpaperService::getLibrary() {
    return m_libraryScanner.getLibraryAsVariantList();
}

void WallpaperService::scanLibrary() {
    m_libraryScanner.scanAll();
}

void WallpaperService::addCustomLibraryPath(const QString& path) {
    m_libraryScanner.addCustomDirectory(path);
    m_libraryScanner.scanAll();
}

QVariantMap WallpaperService::getWallpaperProperties(const QString& id) {
    auto item = m_libraryScanner.getWallpaperById(id);
    return item.properties;
}

void WallpaperService::setProperty(const QString& key, const QDBusVariant& value) {
    m_activeProperties[key] = value.variant();
    qInfo() << "Property changed:" << key << "=" << value.variant();
    // Live reload: re-parse scene with updated properties
    std::unordered_map<std::string, QVariant> stdMap;
    for (auto it = m_activeProperties.begin(); it != m_activeProperties.end(); ++it) {
        stdMap[it.key().toStdString()] = it.value();
    }
    if (m_compositor.hasScene() && !m_compositor.isWeb()) {
        if (m_compositor.reloadWithProperties(stdMap)) {
            qInfo() << "Property live reload succeeded for" << key;
        }
    }
    Q_EMIT propertyChanged(key, value);
}

QDBusVariant WallpaperService::getProperty(const QString& key) {
    return QDBusVariant(m_activeProperties.value(key));
}

} // namespace WallpaperEngine::IPC
