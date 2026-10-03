#pragma once

#include <QString>
#include <QtGui/qwindow.h>

struct xcb_connection_t;

// Turns a QWindow into an EWMH "desktop level" window: the window manager
// stacks it at the bottom of the z-order, keeps it out of the taskbar and
// pager, and shows it on every workspace. This is the X11 stand-in for the
// wlr-layer-shell background surface that LayerShellQt requests on Wayland —
// on X11 there is no such protocol, so the window has to be pushed down the
// stacking order by convention instead.
//
// The work is deliberately split into a pre-map and a post-map call. Both are
// required and neither alone is sufficient; see the comments in
// configureBeforeMap() / restackAfterMap() and in wallpaper_layer.cpp.
class X11DesktopWindow {
public:
    X11DesktopWindow();
    ~X11DesktopWindow();

    X11DesktopWindow(const X11DesktopWindow&) = delete;
    X11DesktopWindow& operator=(const X11DesktopWindow&) = delete;

    // False when no usable X connection could be obtained: no DISPLAY, a
    // Wayland-only session, or a Qt built without xcb support. Callers must
    // check this before touching anything.
    bool isUsable() const { return m_conn != nullptr; }

    // Writes, as plain X properties:
    //   _NET_WM_WINDOW_TYPE = _NET_WM_WINDOW_TYPE_DESKTOP
    //   _NET_WM_STATE        = { _NET_WM_STATE_BELOW, _NET_WM_STATE_STICKY,
    //                           _NET_WM_STATE_SKIP_TASKBAR, _NET_WM_STATE_SKIP_PAGER }
    //   _NET_WM_DESKTOP      = 0xFFFFFFFF  (all desktops)
    //
    // Creates the native window if it does not exist yet, so this must run
    // after QWindow::create()/winId() is meaningful but before show(): the
    // window manager reads these properties while handling the MapRequest,
    // and a window type written after the map is frequently ignored.
    bool configureBeforeMap(QWindow* window);

    // Re-asserts the same state through the EWMH client messages, then
    // re-states the window's geometry and pushes it to the absolute bottom
    // of the stacking order with one ConfigureWindow. Must run after the
    // window is mapped: _NET_WM_STATE_BELOW is a relative stacking request,
    // and window managers recompute placement during the initial map.
    void restackAfterMap(QWindow* window);

private:
    struct Atoms {
        quint32 windowType = 0;         // _NET_WM_WINDOW_TYPE
        quint32 windowTypeDesktop = 0;  // _NET_WM_WINDOW_TYPE_DESKTOP
        quint32 state = 0;              // _NET_WM_STATE
        quint32 stateBelow = 0;         // _NET_WM_STATE_BELOW
        quint32 stateSkipTaskbar = 0;   // _NET_WM_STATE_SKIP_TASKBAR
        quint32 stateSkipPager = 0;     // _NET_WM_STATE_SKIP_PAGER
        quint32 stateSticky = 0;        // _NET_WM_STATE_STICKY
        quint32 wmDesktop = 0;          // _NET_WM_DESKTOP
        quint32 typeWindow = 0;         // "WINDOW"
        quint32 typeCardinal = 0;       // "CARDINAL"
        quint32 typeAtom = 0;           // "ATOM" — required for _NET_WM_WINDOW_TYPE
                                         // and _NET_WM_STATE, which hold atom values
    };

    quint32 internAtom(const char* name);
    bool atomsReady() const;
    bool setProperty(quint32 window, quint32 property, quint32 type,
                     uint8_t format, uint32_t units, const void* data);
    void sendClientMessage(quint32 window, quint32 message, const uint32_t* words, int wordCount);

    // Qt's own connection when the native interface provides one, otherwise a
    // private connection this object owns and must disconnect.
    xcb_connection_t* m_conn = nullptr;
    bool m_ownsConnection = false;
    quint32 m_root = 0;
    Atoms m_atoms;
};
