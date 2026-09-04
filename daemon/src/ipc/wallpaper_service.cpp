#include "wallpaper_service.h"
#include "../assets/tex_parser.h"
#include "../assets/dxt_decoder.h"
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <algorithm>

namespace WallpaperEngine::IPC {

WallpaperService::WallpaperService(Render::VulkanContext* vulkanCtx,
                                   const QStringList& trustedDirs,
                                   QObject* parent)
    : QObject(parent), m_vulkanCtx(vulkanCtx), m_compositor(vulkanCtx) {
    connect(&m_libraryScanner, &Assets::LibraryScanner::scanCompleted, this, &WallpaperService::libraryUpdated);
    m_libraryScanner.scanAll();

    // Seed the load-path allowlist: canonical library roots known to the
    // scanner (Steam workshop roots + custom dirs) plus anything the daemon
    // process was explicitly told to trust at startup.
    m_trustedDirs = m_libraryScanner.getTrustedDirectories();
    for (const QString& dir : trustedDirs) {
        const QString canon = canonicalizePath(dir);
        if (!canon.isEmpty() && !m_trustedDirs.contains(canon)) {
            m_trustedDirs.append(canon);
        }
    }
}

QString WallpaperService::canonicalizePath(const QString& path) const {
    // QDir::canonicalPath resolves symlinks, ".." and "." segments; it
    // returns an empty string for non-existent targets, so fall back to an
    // absolute cleaning pass (no symlink resolution) for the not-yet-existing
    // case so validation stays deterministic.
    QString canon = QDir(path).canonicalPath();
    if (canon.isEmpty()) {
        canon = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    }
    return canon;
}

bool WallpaperService::isPathAllowed(const QString& canonicalPath) const {
    // Cheap static traversal rejection first: no parent-directory segments.
    const QStringList segments = canonicalPath.split(u'/', Qt::SkipEmptyParts);
    if (segments.contains(QStringLiteral(".."))) {
        return false;
    }
    // Containment: the path must live under one of the trusted library roots.
    return std::any_of(m_trustedDirs.cbegin(), m_trustedDirs.cend(),
                       [&canonicalPath](const QString& root) {
                           return canonicalPath.startsWith(root + u'/');
                       });
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

bool WallpaperService::setResolutionForOutput(const QString& outputName, uint32_t width, uint32_t height) {
    if (!m_vulkanCtx) return false;
    Render::DmaBufBuffer newBuf;
    if (m_vulkanCtx->setResolutionForOutput(outputName.toStdString(), width, height, newBuf)) {
        qInfo() << "Output" << outputName << "resolution set to" << width << "x" << height;
        Q_EMIT bufferResized(width, height);
        return true;
    }
    return false;
}

QDBusUnixFileDescriptor WallpaperService::getBufferFdForOutput(const QString& outputName) {
    QDBusUnixFileDescriptor desc;
    if (m_vulkanCtx) {
        const auto* buf = m_vulkanCtx->getBufferForOutput(outputName.toStdString());
        if (buf && buf->fd >= 0) {
            desc.setFileDescriptor(buf->fd);
        }
    }
    return desc;
}

QVariantMap WallpaperService::getBufferInfoForOutput(const QString& outputName) {
    QVariantMap map;
    if (m_vulkanCtx) {
        const auto* buf = m_vulkanCtx->getBufferForOutput(outputName.toStdString());
        if (buf) {
            map[QStringLiteral("width")] = buf->width;
            map[QStringLiteral("height")] = buf->height;
            map[QStringLiteral("stride")] = buf->stride;
            map[QStringLiteral("format")] = buf->format;
            map[QStringLiteral("size")] = static_cast<qulonglong>(buf->size);
        }
    }
    return map;
}

QVariantList WallpaperService::getOutputs() {
    QVariantList list;
    if (m_vulkanCtx) {
        for (const auto& name : m_vulkanCtx->getOutputNames()) {
            list.append(QString::fromStdString(name));
        }
    }
    return list;
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

    // Security gate: only accept wallpapers that live inside a trusted
    // library root (Steam workshop roots, registered custom directories).
    // Canonicalizing first neutralizes symlink and '..' traversal tricks.
    const QString canonical = canonicalizePath(path);
    if (!isPathAllowed(canonical)) {
        qWarning() << "WallpaperService: rejected untrusted load path:" << path
                   << "(canonical:" << canonical << ")"
                   << "— register its directory via registerTrustedDirectory first.";
        return false;
    }

    QFileInfo info(canonical);

    if (!info.exists()) {
        qWarning() << "Path does not exist:" << canonical;
        return false;
    }

    m_activeWallpaperId = canonical;
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
        QString pkgFile = info.isDir() ? (canonical + QStringLiteral("/scene.pkg")) : canonical;
        
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

    // Registering a library directory is an explicit local act, so the new
    // root is trusted for loadWallpaper too. Refresh against the scanner's
    // view so custom dirs added by other means are picked up as well.
    for (const QString& dir : m_libraryScanner.getTrustedDirectories()) {
        const QString canon = canonicalizePath(dir);
        if (!canon.isEmpty() && !m_trustedDirs.contains(canon)) {
            m_trustedDirs.append(canon);
        }
    }
}

bool WallpaperService::registerTrustedDirectory(const QString& dirPath) {
    if (dirPath.isEmpty()) {
        return false;
    }
    // Only existing directories may be registered — not files.
    QFileInfo info(dirPath);
    if (!info.isDir()) {
        qWarning() << "WallpaperService: registerTrustedDirectory: not a directory:" << dirPath;
        return false;
    }
    const QString canon = canonicalizePath(dirPath);
    if (canon.isEmpty()) {
        return false;
    }
    // Refuse to trust the filesystem root — that would disable the gate.
    if (canon == QStringLiteral("/")) {
        qWarning() << "WallpaperService: refusing to trust filesystem root";
        return false;
    }
    if (!m_trustedDirs.contains(canon)) {
        m_trustedDirs.append(canon);
        qInfo() << "WallpaperService: trusted directory registered:" << canon;
    }
    return true;
}

QStringList WallpaperService::getTrustedDirectories() {
    return m_trustedDirs;
}

QVariantMap WallpaperService::getWallpaperProperties(const QString& id) {
    auto item = m_libraryScanner.getWallpaperById(id);
    return item.properties;
}

void WallpaperService::setProperty(const QString& key, const QDBusVariant& value) {
    m_activeProperties[key] = value.variant();
    qInfo() << "Property changed:" << key << "=" << value.variant();
    std::unordered_map<std::string, QVariant> stdMap;
    for (auto it = m_activeProperties.begin(); it != m_activeProperties.end(); ++it) {
        stdMap[it.key().toStdString()] = it.value();
    }
    if (m_compositor.hasScene()) {
        if (m_compositor.isWeb()) {
            m_compositor.setWebProperty(key, value.variant());
        } else {
            if (m_compositor.reloadWithProperties(stdMap)) {
                qInfo() << "Property live reload succeeded for" << key;
            }
        }
    }
    Q_EMIT propertyChanged(key, value);
}

QDBusVariant WallpaperService::getProperty(const QString& key) {
    return QDBusVariant(m_activeProperties.value(key));
}

} // namespace WallpaperEngine::IPC
