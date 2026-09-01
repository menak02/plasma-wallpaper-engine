#pragma once
#include <string>
#include <QImage>
#include <QUrl>
#include <QObject>

namespace WallpaperEngine::Scene {

class WebWallpaper : public QObject {
    Q_OBJECT
public:
    explicit WebWallpaper(QObject* parent = nullptr);
    ~WebWallpaper();

    bool load(const std::string& htmlContent, const std::string& baseUrl = {});
    bool loadFile(const std::string& filePath);
    void setSize(uint32_t w, uint32_t h);
    QImage grabImage();

    bool isLoaded() const { return m_loaded; }
    bool hasWebEngine() const;

Q_SIGNALS:
    void loaded(bool ok);

private:
    class Impl;
    Impl* m_impl = nullptr;
    bool m_loaded = false;
    uint32_t m_w = 1920, m_h = 1080;
};

} // namespace
