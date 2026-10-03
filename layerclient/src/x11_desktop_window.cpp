#include "x11_desktop_window.h"

#include <QDebug>
#include <QGuiApplication>
#include <QRect>

#include <QtGui/qguiapplication_platform.h>

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>

#include <cstdlib>
#include <cstring>

namespace {

// EWMH desktop number meaning "show on every workspace".
constexpr uint32_t kAllDesktops = 0xFFFFFFFFu;

// _NET_WM_STATE client message layout: data.l[0] is the source indication
// (1 = application, 2 = pager), data.l[1] the action (0 = remove, 1 = add,
// 2 = toggle), followed by up to four state atoms.
constexpr uint32_t kStateSourceApplication = 1;
constexpr uint32_t kStateActionAdd = 1;

// _NET_WM_DESKTOP client message: data.l[0] = desktop, data.l[1] = source
// indication (1 = user), data.l[2] = serial (unused).
constexpr uint32_t kDesktopSourceUser = 1;

// A 32-bit client message carries five 32-bit words.
constexpr int kClientMessageWords = 5;

} // namespace

X11DesktopWindow::X11DesktopWindow()
{
#if QT_CONFIG(xcb)
    // Prefer Qt's own XCB connection. Two reasons: the window we decorate is
    // a resource of that connection, and sharing it means our requests and
    // Qt's XMapWindow travel the same wire, so the window manager is
    // guaranteed to see the MapRequest before the state message that follows
    // it. That ordering is what makes restackAfterMap() reliable.
    if (auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
        m_conn = x11->connection();
    }
#endif
    if (!m_conn) {
        // Qt is not on xcb, or it does not publish the native interface (Qt
        // older than 6.2, or a platform plugin that does not implement it).
        // X resource ids are global to the server, so a second connection to
        // the same DISPLAY can still address Qt's window — we just lose the
        // request-ordering guarantee with MapWindow.
        m_conn = xcb_connect(nullptr, nullptr);
        m_ownsConnection = m_conn != nullptr;
    }

    if (!m_conn || xcb_connection_has_error(m_conn)) {
        if (m_ownsConnection && m_conn) {
            xcb_disconnect(m_conn);
        }
        m_conn = nullptr;
        return;
    }

    m_root = xcb_setup_roots_iterator(xcb_get_setup(m_conn)).data->root;

    m_atoms.windowType = internAtom("_NET_WM_WINDOW_TYPE");
    m_atoms.windowTypeDesktop = internAtom("_NET_WM_WINDOW_TYPE_DESKTOP");
    m_atoms.state = internAtom("_NET_WM_STATE");
    m_atoms.stateBelow = internAtom("_NET_WM_STATE_BELOW");
    m_atoms.stateSkipTaskbar = internAtom("_NET_WM_STATE_SKIP_TASKBAR");
    m_atoms.stateSkipPager = internAtom("_NET_WM_STATE_SKIP_PAGER");
    m_atoms.stateSticky = internAtom("_NET_WM_STATE_STICKY");
    m_atoms.wmDesktop = internAtom("_NET_WM_DESKTOP");
    m_atoms.typeWindow = internAtom("WINDOW");
    m_atoms.typeCardinal = internAtom("CARDINAL");
    m_atoms.typeAtom = internAtom("ATOM");

    if (!atomsReady()) {
        qWarning() << "X11DesktopWindow: could not intern the EWMH atoms;"
                      " this window will not be treated as a desktop window";
    }
}

X11DesktopWindow::~X11DesktopWindow()
{
    // A borrowed Qt connection is left alone; Qt owns it and outlives us.
    if (m_ownsConnection && m_conn) {
        xcb_disconnect(m_conn);
    }
    m_conn = nullptr;
}

quint32 X11DesktopWindow::internAtom(const char* name)
{
    const xcb_intern_atom_cookie_t cookie =
        xcb_intern_atom(m_conn, /*only_if_exists=*/0,
                        static_cast<uint16_t>(std::strlen(name)), name);
    xcb_generic_error_t* error = nullptr;
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(m_conn, cookie, &error);
    if (!reply) {
        qWarning() << "X11DesktopWindow: InternAtom of" << name << "failed"
                   << (error ? QString::number(error->error_code)
                             : QStringLiteral("no reply"));
        std::free(error);
        return 0;
    }
    const quint32 atom = reply->atom;
    std::free(reply);
    return atom;
}

bool X11DesktopWindow::atomsReady() const
{
    return m_atoms.windowType != 0 && m_atoms.windowTypeDesktop != 0 &&
           m_atoms.state != 0 && m_atoms.stateBelow != 0 &&
           m_atoms.stateSkipTaskbar != 0 && m_atoms.stateSkipPager != 0 &&
           m_atoms.stateSticky != 0 && m_atoms.wmDesktop != 0 &&
           m_atoms.typeWindow != 0 && m_atoms.typeCardinal != 0 && m_atoms.typeAtom != 0;
}

bool X11DesktopWindow::setProperty(quint32 window, quint32 property, quint32 type,
                                   uint8_t format, uint32_t units, const void* data)
{
    // Checked requests keep failures attached to the request that caused
    // them. We deliberately do not install an xcb error handler: the handler
    // is a process-wide setting and Qt installs its own.
    const xcb_void_cookie_t cookie =
        xcb_change_property_checked(m_conn, XCB_PROP_MODE_REPLACE, window,
                                    property, type, format, units, data);
    if (xcb_generic_error_t* error = xcb_request_check(m_conn, cookie)) {
        qWarning() << "X11DesktopWindow: ChangeProperty of atom" << property
                   << "on window" << window
                   << "failed with X error code" << error->error_code;
        std::free(error);
        return false;
    }
    return true;
}

void X11DesktopWindow::sendClientMessage(quint32 window, quint32 message,
                                         const uint32_t* words, int wordCount)
{
    xcb_client_message_event_t event;
    std::memset(&event, 0, sizeof(event));
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = window;
    event.type = message;

    const int count = wordCount < kClientMessageWords ? wordCount : kClientMessageWords;
    for (int i = 0; i < count; ++i) {
        event.data.data32[i] = words[i];
    }

    // Sent to the root window; only the window manager listens for it.
    const xcb_void_cookie_t cookie = xcb_send_event_checked(
        m_conn, /*propagate=*/0, m_root,
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
        reinterpret_cast<const char*>(&event));
    if (xcb_generic_error_t* error = xcb_request_check(m_conn, cookie)) {
        qWarning() << "X11DesktopWindow: client message" << message
                   << "to the root window failed with X error code" << error->error_code;
        std::free(error);
    }
}

bool X11DesktopWindow::configureBeforeMap(QWindow* window)
{
    if (!m_conn || !window || !atomsReady()) {
        return false;
    }

    // Everything below addresses an X resource, so make sure the platform
    // window exists. Qt writes its own _NET_WM_WINDOW_TYPE while creating
    // it (Qt::Tool becomes _NET_WM_WINDOW_TYPE_UTILITY), which is exactly why
    // this overwrite has to happen after create() and before the map.
    window->create();
    const WId id = window->winId();
    if (!id) {
        return false;
    }
    const auto win = static_cast<quint32>(id);

    // A DESKTOP window is what makes the window manager treat this as part of
    // the desktop rather than as a normal client window.
    //
    // The property's *type* must be ATOM, not WINDOW: both _NET_WM_WINDOW_TYPE
    // and _NET_WM_STATE carry atom values. Writing them with type WINDOW makes
    // xprop report "(WINDOW): window id # 0x..." and a conforming window
    // manager ignores the property outright, so the window never becomes a
    // desktop-layer window and ends up swallowing desktop clicks.
    const quint32 type = m_atoms.windowTypeDesktop;
    setProperty(win, m_atoms.windowType, m_atoms.typeAtom, 32, 1, &type);

    // Pre-set _NET_WM_STATE as a property as well. Qt only puts BELOW in
    // there (it maps Qt::WindowStaysOnBottomHint to that single atom), and a
    // window manager that samples the property while handling the MapRequest
    // would otherwise never see SKIP_TASKBAR / SKIP_PAGER / STICKY.
    const quint32 states[4] = {
        m_atoms.stateBelow, m_atoms.stateSticky,
        m_atoms.stateSkipTaskbar, m_atoms.stateSkipPager
    };
    setProperty(win, m_atoms.state, m_atoms.typeAtom, 32, 4, states);

    // 0xFFFFFFFF means "on every desktop". restackAfterMap() repeats this as
    // a client message, because window managers are split on whether they
    // honour the property or the message.
    const quint32 all = kAllDesktops;
    setProperty(win, m_atoms.wmDesktop, m_atoms.typeCardinal, 32, 1, &all);

    xcb_flush(m_conn);
    return true;
}

void X11DesktopWindow::restackAfterMap(QWindow* window)
{
    if (!m_conn || !window || !atomsReady()) {
        return;
    }
    const WId id = window->winId();
    if (!id) {
        return;
    }
    const auto win = static_cast<quint32>(id);

    // The authoritative way to set _NET_WM_STATE: "add" the four atoms. A
    // 32-bit client message carries five words and two of them are the source
    // indication and the action, so only three atoms fit per message.
    const uint32_t primaryStateWords[kClientMessageWords] = {
        kStateSourceApplication,
        kStateActionAdd,
        m_atoms.stateBelow,
        m_atoms.stateSticky,
        m_atoms.stateSkipTaskbar,
    };
    sendClientMessage(win, m_atoms.state, primaryStateWords, kClientMessageWords);
    const uint32_t secondaryStateWords[kClientMessageWords] = {
        kStateSourceApplication, kStateActionAdd, m_atoms.stateSkipPager,
    };
    sendClientMessage(win, m_atoms.state, secondaryStateWords, 3);

    const uint32_t desktopWords[3] = { kAllDesktops, kDesktopSourceUser, 0 };
    sendClientMessage(win, m_atoms.wmDesktop, desktopWords, 3);

    // Explicit restack plus an explicit geometry, in one request: this is the
    // X equivalent of EWMH's _NET_RESTACK_WINDOW, and it is the part that
    // actually matters. _NET_WM_STATE_BELOW alone is relative ("below
    // whatever was above me when the request was handled"), and a window
    // manager may place or re-place the window right afterwards. Re-stating
    // the geometry covers the case where the manager moved us during the
    // initial map; moving to the absolute bottom of the stacking order is
    // what keeps the wallpaper behind every other window.
    const QRect geometry = window->geometry();
    xcb_configure_window_value_list_t place;
    std::memset(&place, 0, sizeof(place));
    place.x = geometry.x();
    place.y = geometry.y();
    place.width = static_cast<uint32_t>(geometry.width());
    place.height = static_cast<uint32_t>(geometry.height());
    place.stack_mode = XCB_STACK_MODE_BELOW;

    const uint16_t placeMask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                              XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                              XCB_CONFIG_WINDOW_STACK_MODE;
    if (xcb_generic_error_t* error = xcb_request_check(
            m_conn, xcb_configure_window_checked(m_conn, win, placeMask, &place))) {
        qWarning() << "X11DesktopWindow: placing window" << win
                   << "failed with X error code" << error->error_code;
        std::free(error);
    }

    xcb_flush(m_conn);
}
