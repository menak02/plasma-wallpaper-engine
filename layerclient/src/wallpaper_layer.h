#pragma once

#include <QObject>
#include <QQuickView>
#include <QImage>
#include <QDBusInterface>
#include <QQuickImageProvider>
#include <QTimer>
#include <QMutex>
#include <QScreen>
#include <QString>

#include <memory>

class X11DesktopWindow;

// One wallpaper surface per compositor output.
//
// How the surface is requested depends on the display server the session runs
// on, because there is no cross-platform protocol for "put this behind
// everything":
//
//   - Wayland: a wlr-layer-shell background surface, requested through
//     LayerShellQt. Composed for Hyprland/Sway/labwc and friends.
//   - X11:     an EWMH desktop-level window, i.e. _NET_WM_WINDOW_TYPE_DESKTOP
//     plus the _NET_WM_STATE set (BELOW, SKIP_TASKBAR, SKIP_PAGER, STICKY)
//     and _NET_WM_DESKTOP = 0xFFFFFFFF. xfwm4, openbox, mutter and friends
//     all honour that combination.
//
// Either way the class owns a QQuickView, installs a QQuickImageProvider
// named "frame", and pumps the daemon's exported DmaBuf for the matching
// output into it.
class WallpaperLayer : public QObject {
    Q_OBJECT

public:
    // Which display path this build/session takes.
    enum class Backend {
        Unknown,    // neither WAYLAND_DISPLAY nor DISPLAY says anything useful
        LayerShell, // Wayland + wlr-layer-shell, via LayerShellQt
        X11Desktop, // X11 + EWMH desktop window
    };

    // Wayland wins when both environment variables are set: a Wayland session
    // started from inside an X session (labwc launched by hand, a nested
    // compositor) exports the inherited DISPLAY too, and layer-shell is the
    // path that actually works there.
    static Backend detectBackend();
    static const char* backendName(Backend backend);

    // outputName must match a daemon output from getOutputs(); screen pins
    // the surface to that physical monitor.
    WallpaperLayer(const QString& outputName, QScreen* screen, QObject* parent = nullptr);
    ~WallpaperLayer() override;

    bool isValid() const { return m_valid; }
    Backend backend() const { return m_backend; }

public Q_SLOTS:
    void onFrameReady();
    void onBufferResized();

protected:
    // Used to catch the first QEvent::Expose, which is the earliest point at
    // which the window manager has actually mapped and stacked the window.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    class FrameProvider;

    void connectDbusSignals();
    bool refreshBufferInfo();
    void publishFrame();

    void configureLayerShell();
    void configureX11();
    void applyX11State();
    void onScreenGeometryChanged();

    QString m_outputName;
    QQuickView* m_view = nullptr;
    QDBusInterface m_iface;

    FrameProvider* m_provider = nullptr;
    qint64 m_frameSerial = 0;

    // Buffer geometry from getBufferInfoForOutput; refetched when the daemon
    // reports bufferResized for this output.
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_stride = 0;
    size_t m_size = 0;

    qint64 m_lastPollMs = 0;
    bool m_valid = false;

    Backend m_backend = Backend::Unknown;
    QScreen* m_screen = nullptr;
    QMetaObject::Connection m_screenGeometryConn;

    // X11 only. Null on the layer-shell path, and also null on the X11 path
    // when no X connection could be obtained.
    std::unique_ptr<X11DesktopWindow> m_x11;
    bool m_x11StateApplied = false;
};
