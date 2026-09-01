#include "web_wallpaper.h"
#include <QDebug>
#include <QPainter>

#if __has_include(<QWebEngineView>)
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#define HAS_WEBENGINE 1
#else
#define HAS_WEBENGINE 0
#endif

namespace WallpaperEngine::Scene {

class WebWallpaper::Impl {
public:
#if HAS_WEBENGINE
    QWebEngineView* view = nullptr;
#endif
};

WebWallpaper::WebWallpaper(QObject* parent) : QObject(parent) {
    m_impl = new Impl();
#if HAS_WEBENGINE
    // Do not create view until load() to allow headless fallback
#endif
}

WebWallpaper::~WebWallpaper() {
#if HAS_WEBENGINE
    if (m_impl && m_impl->view) delete m_impl->view;
#endif
    delete m_impl;
}

bool WebWallpaper::hasWebEngine() const {
#if HAS_WEBENGINE
    return true;
#else
    return false;
#endif
}

bool WebWallpaper::load(const std::string& htmlContent, const std::string& baseUrl) {
#if HAS_WEBENGINE
    if (!m_impl->view) {
        m_impl->view = new QWebEngineView();
        m_impl->view->resize(m_w, m_h);
        m_impl->view->page()->settings()->setAttribute(QWebEngineSettings::ShowScrollBars, false);
        m_impl->view->page()->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
        QObject::connect(m_impl->view, &QWebEngineView::loadFinished, this, [this](bool ok){
            m_loaded = ok;
            Q_EMIT loaded(ok);
            qInfo() << "WebWallpaper: loadFinished" << ok;
        });
    } else {
        m_loaded = false;
    }
    QUrl base = baseUrl.empty() ? QUrl(QStringLiteral("qrc:///")) : QUrl(QString::fromStdString(baseUrl));
    m_impl->view->setHtml(QString::fromStdString(htmlContent), base);
    // Don't set m_loaded true immediately; wait for loadFinished signal
    qInfo() << "WebWallpaper: loading HTML" << htmlContent.size() << "bytes hasWebEngine=1";
    return true;
#else
    Q_UNUSED(htmlContent); Q_UNUSED(baseUrl);
    qWarning() << "WebWallpaper: QtWebEngine not available at compile time, stub";
    m_loaded = false;
    return false;
#endif
}

bool WebWallpaper::loadFile(const std::string& filePath) {
#if HAS_WEBENGINE
    if (!m_impl->view) {
        m_impl->view = new QWebEngineView();
        m_impl->view->resize(m_w, m_h);
    }
    m_impl->view->load(QUrl::fromLocalFile(QString::fromStdString(filePath)));
    m_loaded = true;
    return true;
#else
    Q_UNUSED(filePath);
    return false;
#endif
}

void WebWallpaper::setSize(uint32_t w, uint32_t h) {
    m_w = w; m_h = h;
#if HAS_WEBENGINE
    if (m_impl->view) m_impl->view->resize(w, h);
#endif
}

QImage WebWallpaper::grabImage() {
#if HAS_WEBENGINE
    if (!m_impl->view) return {};
    if (!m_loaded) {
        // Still loading — return placeholder until loadFinished
        QImage placeholder(m_w, m_h, QImage::Format_ARGB32);
        placeholder.fill(QColor(20,20,30));
        QPainter p(&placeholder);
        p.setPen(Qt::white);
        p.drawText(placeholder.rect(), Qt::AlignCenter, QStringLiteral("Web Loading..."));
        return placeholder;
    }
    QImage img = m_impl->view->grab().toImage();
    if (!img.isNull()) return img.convertToFormat(QImage::Format_ARGB32);
    QImage placeholder(m_w, m_h, QImage::Format_ARGB32);
    placeholder.fill(QColor(20,20,30));
    QPainter p(&placeholder);
    p.setPen(Qt::white);
    p.drawText(placeholder.rect(), Qt::AlignCenter, QStringLiteral("Web Wallpaper (QtWebEngine)"));
    return placeholder;
#else
    QImage placeholder(m_w, m_h, QImage::Format_ARGB32);
    placeholder.fill(QColor(20,20,30));
    return placeholder;
#endif
}

void WebWallpaper::setProperty(const QString& key, const QVariant& value) {
#if HAS_WEBENGINE
    if (!m_impl->view || !m_impl->view->page()) return;
    QString js = QStringLiteral("if(window.wallpaperPropertyListener){"
                                "  var p={}; p['%1']= %2;"
                                "  wallpaperPropertyListener.applyUserProperties(p);"
                                "} else if(window.scriptProperties){"
                                "  scriptProperties['%1']={value:%2};"
                                "}")
                   .arg(key, value.toString().isEmpty() ? QStringLiteral("null") : QStringLiteral("\"%1\"").arg(value.toString()));
    // Try generic set
    QString valStr;
    if (value.typeId() == QMetaType::Bool) valStr = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    else if (value.typeId() == QMetaType::QString) valStr = QStringLiteral("\"%1\"").arg(value.toString());
    else valStr = value.toString();
    js = QStringLiteral("try{"
                        "if(window.wallpaperPropertyListener) wallpaperPropertyListener.applyUserProperties({'%1':%2});"
                        "else if(window.scriptProperties) scriptProperties['%1'].value=%2;"
                        "}catch(e){}").arg(key, valStr);
    m_impl->view->page()->runJavaScript(js);
    qInfo() << "WebWallpaper: setProperty" << key << "=" << value;
#else
    Q_UNUSED(key); Q_UNUSED(value);
#endif
}

void WebWallpaper::runJavaScript(const QString& script) {
#if HAS_WEBENGINE
    if (m_impl->view && m_impl->view->page()) m_impl->view->page()->runJavaScript(script);
#else
    Q_UNUSED(script);
#endif
}

} // namespace
