#include "x11_ewmh_backend.h"

#include <xcb/xcb.h>
#include <xcb/randr.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int32_t kAllDesktops = 0xFFFFFFFF;

uint32_t internAtom(xcb_connection_t* conn, const char* name) {
    auto cookie = xcb_intern_atom(conn, 0, static_cast<uint16_t>(strlen(name)), name);
    auto reply = xcb_intern_atom_reply(conn, cookie, nullptr);
    return reply ? reply->atom : XCB_ATOM_NONE;
}

uint32_t internAtomChecked(xcb_connection_t* conn, xcb_atom_t existing, const char* name) {
    if (existing != XCB_ATOM_NONE) {
        return existing;
    }
    return internAtom(conn, name);
}

std::string getEnv(const char* name) {
    const char* value = getenv(name);
    return value ? std::string(value) : std::string();
}

/** Reads a CARDINAL property. */
uint32_t readCardinal(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom) {
    auto cookie = xcb_get_property(conn, 0, window, atom, XCB_ATOM_CARDINAL, 0, 1);
    auto reply = xcb_get_property_reply(conn, cookie, nullptr);
    if (!reply || reply->type != XCB_ATOM_CARDINAL || reply->format != 32 || reply->value_len < 1) {
        free(reply);
        return XCB_ATOM_NONE;
    }
    const uint32_t value = *static_cast<uint32_t*>(xcb_get_property_value(reply));
    free(reply);
    return value;
}

/** Reads a WINDOW property into a vector of window ids. */
std::vector<uint32_t> readWindows(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom) {
    std::vector<uint32_t> windows;
    auto cookie = xcb_get_property(conn, 0, window, atom, XCB_ATOM_WINDOW, 0, 4096);
    auto reply = xcb_get_property_reply(conn, cookie, nullptr);
    if (!reply || reply->type != XCB_ATOM_WINDOW || reply->format != 32) {
        free(reply);
        return windows;
    }
    const auto* values = static_cast<uint32_t*>(xcb_get_property_value(reply));
    const int count = xcb_get_property_value_length(reply) / 4;
    windows.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        windows.push_back(values[i]);
    }
    free(reply);
    return windows;
}

/** Reads an ATOM property into a vector of atom ids. */
std::vector<uint32_t> readAtoms(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atom) {
    std::vector<uint32_t> atoms;
    auto cookie = xcb_get_property(conn, 0, window, atom, XCB_ATOM_ATOM, 0, 256);
    auto reply = xcb_get_property_reply(conn, cookie, nullptr);
    if (!reply || reply->type != XCB_ATOM_ATOM || reply->format != 32) {
        free(reply);
        return atoms;
    }
    const auto* values = static_cast<uint32_t*>(xcb_get_property_value(reply));
    const int count = xcb_get_property_value_length(reply) / 4;
    atoms.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        atoms.push_back(values[i]);
    }
    free(reply);
    return atoms;
}

bool atomInList(const std::vector<uint32_t>& atoms, uint32_t atom) {
    return std::find(atoms.begin(), atoms.end(), atom) != atoms.end();
}

/** Fetches a window's geometry including border, via the tree so we get
    real coordinates rather than the WM's pre-translate values. */
bool fetchGeometry(xcb_connection_t* conn, uint32_t windowId,
                   int32_t& x, int32_t& y, int32_t& width, int32_t& height, int32_t& border) {
    auto cookie = xcb_get_geometry(conn, windowId);
    auto reply = xcb_get_geometry_reply(conn, cookie, nullptr);
    if (!reply) {
        return false;
    }
    width = static_cast<int32_t>(reply->width);
    height = static_cast<int32_t>(reply->height);
    x = static_cast<int32_t>(reply->x);
    y = static_cast<int32_t>(reply->y);
    border = static_cast<int32_t>(reply->border_width);
    free(reply);

    // Translate to root-relative coordinates. Skip if the window is
    // unmapped — an unmapped window contributes no coverage.
    if (width <= 0 || height <= 0) {
        return false;
    }
    return true;
}

} // anonymous namespace

namespace WallpaperEngine::Scene {

X11EwmhBackend::~X11EwmhBackend() {
    if (m_conn) {
        xcb_disconnect(m_conn);
        m_conn = nullptr;
    }
}

bool X11EwmhBackend::connectXcb(const char* displayName) {
    int screenNum = 0;
    if (displayName && *displayName) {
        m_conn = xcb_connect(displayName, &screenNum);
    } else {
        const std::string display = getEnv("DISPLAY");
        if (display.empty()) {
            return false;
        }
        m_conn = xcb_connect(display.c_str(), &screenNum);
    }

    if (!m_conn || xcb_connection_has_error(m_conn)) {
        if (m_conn) {
            xcb_disconnect(m_conn);
            m_conn = nullptr;
        }
        return false;
    }

    const xcb_setup_t* setup = xcb_get_setup(m_conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screenNum && it.rem; ++i) {
        xcb_screen_next(&it);
    }
    m_screen = it.data;
    if (!m_screen) {
        xcb_disconnect(m_conn);
        m_conn = nullptr;
        return false;
    }
    return true;
}

void X11EwmhBackend::cacheAtoms() {
    m_atomNetClientList = internAtom(m_conn, "_NET_CLIENT_LIST");
    m_atomNetClientListStacking = internAtom(m_conn, "_NET_CLIENT_LIST_STACKING");
    m_atomNetCurrentDesktop = internAtom(m_conn, "_NET_CURRENT_DESKTOP");
    m_atomNetWmDesktop = internAtom(m_conn, "_NET_WM_DESKTOP");
    m_atomNetWmState = internAtom(m_conn, "_NET_WM_STATE");
    m_atomNetWmStateFullscreen = internAtom(m_conn, "_NET_WM_STATE_FULLSCREEN");
    m_atomNetWmStateSkipTaskbar = internAtom(m_conn, "_NET_WM_STATE_SKIP_TASKBAR");
    m_atomNetWmStateSkipPager = internAtom(m_conn, "_NET_WM_STATE_SKIP_PAGER");
    m_atomNetWmStateSticky = internAtom(m_conn, "_NET_WM_STATE_STICKY");
    m_atomNetWmStateAbove = internAtom(m_conn, "_NET_WM_STATE_ABOVE");
    m_atomNetWmWindowType = internAtom(m_conn, "_NET_WM_WINDOW_TYPE");
    m_atomNetWmWindowTypeDesktop = internAtom(m_conn, "_NET_WM_WINDOW_TYPE_DESKTOP");
    m_atomNetWmWindowTypeDock = internAtom(m_conn, "_NET_WM_WINDOW_TYPE_DOCK");
    m_atomNetWmWindowTypeUtility = internAtom(m_conn, "_NET_WM_WINDOW_TYPE_UTILITY");
    m_atomNetWmWindowTypeSplash = internAtom(m_conn, "_NET_WM_WINDOW_TYPE_SPLASH");
    m_atomNetWmWindowTypeDialog = internAtom(m_conn, "_NET_WM_WINDOW_TYPE_DIALOG");
    m_atomNetSupportingWmCheck = internAtom(m_conn, "_NET_SUPPORTING_WM_CHECK");
    m_atomNetWmName = internAtom(m_conn, "_NET_WM_NAME");
    m_atomUtf8String = internAtom(m_conn, "UTF8_STRING");
    m_atomNetDesktopGeometry = internAtom(m_conn, "_NET_DESKTOP_GEOMETRY");
    m_atomNetDesktopNames = internAtom(m_conn, "_NET_DESKTOP_NAMES");
}

bool X11EwmhBackend::queryRootProperties() {
    const xcb_window_t root = m_screen->root;

    // Current workspace.
    m_currentDesktop = readCardinal(m_conn, root, m_atomNetCurrentDesktop);
    m_haveCurrentDesktop = (m_currentDesktop != XCB_ATOM_NONE);

    // Desktop geometry gives the wallpaper area. Fall back to the screen
    // dimensions when the WM does not advertise it.
    auto cookie = xcb_get_property(m_conn, 0, root, m_atomNetDesktopGeometry,
                                   XCB_ATOM_CARDINAL, 0, 4);
    auto reply = xcb_get_property_reply(m_conn, cookie, nullptr);
    if (reply && reply->type == XCB_ATOM_CARDINAL && reply->value_len >= 4) {
        const auto* v = static_cast<uint32_t*>(xcb_get_property_value(reply));
        m_rootRect.x = 0;
        m_rootRect.y = 0;
        m_rootRect.width = static_cast<int32_t>(v[2]);
        m_rootRect.height = static_cast<int32_t>(v[3]);
    } else {
        m_rootRect.x = 0;
        m_rootRect.y = 0;
        m_rootRect.width = static_cast<int32_t>(m_screen->width_in_pixels);
        m_rootRect.height = static_cast<int32_t>(m_screen->height_in_pixels);
    }
    free(reply);

    // A single X screen is one wallpaper output. The root geometry is
    // authoritative; screen dimensions are the fallback.
    m_outputName = "X11-0";

    // Window manager identity, for logging only.
    const uint32_t wmCheck = readCardinal(m_conn, root, m_atomNetSupportingWmCheck);
    if (wmCheck != XCB_ATOM_NONE && wmCheck != 0) {
        const std::string name = "EWMH";
        m_wmName = name;
    } else {
        m_wmName = "unknown";
    }

    return m_rootRect.width > 0 && m_rootRect.height > 0;
}

std::vector<X11EwmhBackend::WindowInfo> X11EwmhBackend::queryWindows() const {
    std::vector<WindowInfo> windows;
    const xcb_window_t root = m_screen->root;

    // Prefer the stacking list so later entries sit above earlier ones.
    std::vector<uint32_t> ids = readWindows(m_conn, root, m_atomNetClientListStacking);
    if (ids.empty()) {
        ids = readWindows(m_conn, root, m_atomNetClientList);
    }

    for (uint32_t id : ids) {
        WindowInfo info;
        info.id = id;

        // Skip unmapped / viewable check: a window that is not viewable
        // cannot be covering anything.
        auto attrsCookie = xcb_get_window_attributes(m_conn, id);
        auto attrs = xcb_get_window_attributes_reply(m_conn, attrsCookie, nullptr);
        if (attrs) {
            info.mapped = (attrs->map_state == XCB_MAP_STATE_VIEWABLE);
            free(attrs);
        }
        if (!info.mapped) {
            continue;
        }

        if (!fetchGeometry(m_conn, id, info.x, info.y, info.width, info.height, info.borderWidth)) {
            continue;
        }

        // Window state.
        const std::vector<uint32_t> state = readAtoms(m_conn, id, m_atomNetWmState);
        info.fullscreen = atomInList(state, m_atomNetWmStateFullscreen);
        info.skipTaskbar = atomInList(state, m_atomNetWmStateSkipTaskbar);
        info.skipPager = atomInList(state, m_atomNetWmStateSkipPager);
        info.sticky = atomInList(state, m_atomNetWmStateSticky);

        // Workspace assignment. XFCE reports 0xFFFFFFFF for "all desktops".
        const uint32_t desktop = readCardinal(m_conn, id, m_atomNetWmDesktop);
        info.desktop = (desktop == XCB_ATOM_NONE) ? kAllDesktops
                                                  : static_cast<int64_t>(desktop);

        // Window type: skip panels, desktop icons, and other chrome that
        // sits permanently on screen and would otherwise read as "covered".
        const std::vector<uint32_t> types = readAtoms(m_conn, id, m_atomNetWmWindowType);
        info.desktopOnly = atomInList(types, m_atomNetWmWindowTypeDesktop);
        info.dock = atomInList(types, m_atomNetWmWindowTypeDock);
        info.utility = atomInList(types, m_atomNetWmWindowTypeUtility);
        info.splash = atomInList(types, m_atomNetWmWindowTypeSplash);
        info.windowTypeDialog = atomInList(types, m_atomNetWmWindowTypeDialog);

        windows.push_back(info);
    }

    return windows;
}

bool X11EwmhBackend::initialize(const char* displayName) {
    if (!connectXcb(displayName)) {
        return false;
    }
    cacheAtoms();
    if (!queryRootProperties()) {
        xcb_disconnect(m_conn);
        m_conn = nullptr;
        return false;
    }
    m_windows = queryWindows();
    m_stackingOrder.clear();
    for (const auto& w : m_windows) {
        m_stackingOrder.push_back(w.id);
    }
    m_initialized = true;
    return true;
}

std::vector<std::string> X11EwmhBackend::outputNames() const {
    if (!m_initialized) {
        return {};
    }
    return {m_outputName};
}

void X11EwmhBackend::update() {
    if (!m_initialized) {
        m_windows.clear();
        m_stackingOrder.clear();
        return;
    }
    queryRootProperties();
    m_windows = queryWindows();
    m_stackingOrder.clear();
    for (const auto& w : m_windows) {
        m_stackingOrder.push_back(w.id);
    }
}

bool X11EwmhBackend::windowOnCurrentDesktop(const WindowInfo& w) const {
    if (!m_haveCurrentDesktop) {
        // Without a workspace property, treat everything as visible and
        // let the coverage math decide. Safer to over-render than pause.
        return true;
    }
    const int64_t current = static_cast<int64_t>(m_currentDesktop);
    if (w.desktop == kAllDesktops || w.sticky) {
        return true;
    }
    return w.desktop == current;
}

int X11EwmhBackend::stackingIndex(uint32_t windowId) const {
    for (size_t i = 0; i < m_stackingOrder.size(); ++i) {
        if (m_stackingOrder[i] == windowId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

long long X11EwmhBackend::unionArea(const std::vector<Rect>& rects, const Rect& bounds) {
    // Clip every rect to the output bounds first.
    std::vector<Rect> clipped;
    clipped.reserve(rects.size());
    for (const auto& r : rects) {
        const int32_t x0 = std::max(r.x, bounds.x);
        const int32_t y0 = std::max(r.y, bounds.y);
        const int32_t x1 = std::min(r.x + r.width, bounds.x + bounds.width);
        const int32_t y1 = std::min(r.y + r.height, bounds.y + bounds.height);
        if (x1 > x0 && y1 > y0) {
            clipped.push_back({x0, y0, x1 - x0, y1 - y0});
        }
    }
    if (clipped.empty()) {
        return 0;
    }

    // Sweep-line union area: collect distinct x edges, then for each x slab
    // determine the y ranges covered by any rect and sum their lengths.
    std::vector<int32_t> xs;
    xs.reserve(clipped.size() * 2);
    for (const auto& r : clipped) {
        xs.push_back(r.x);
        xs.push_back(r.x + r.width);
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());

    long long total = 0;
    for (size_t i = 0; i + 1 < xs.size(); ++i) {
        const int32_t xa = xs[i];
        const int32_t xb = xs[i + 1];
        if (xb <= xa) {
            continue;
        }
        // Midpoint of the slab decides which rects are active.
        const int32_t sample = xa;
        std::vector<std::pair<int32_t, int32_t>> spans;
        for (const auto& r : clipped) {
            if (r.x <= sample && r.x + r.width >= xb) {
                spans.emplace_back(r.y, r.y + r.height);
            }
        }
        if (spans.empty()) {
            continue;
        }
        std::sort(spans.begin(), spans.end());
        int32_t curStart = spans[0].first;
        int32_t curEnd = spans[0].second;
        long long coveredY = 0;
        for (size_t j = 1; j < spans.size(); ++j) {
            if (spans[j].first > curEnd) {
                coveredY += curEnd - curStart;
                curStart = spans[j].first;
                curEnd = spans[j].second;
            } else {
                curEnd = std::max(curEnd, spans[j].second);
            }
        }
        coveredY += curEnd - curStart;
        total += static_cast<long long>(coveredY) * (xb - xa);
    }
    return total;
}

bool X11EwmhBackend::hasFullscreenCovering(const std::vector<WindowInfo>& windows,
                                           const Rect& bounds) const {
    for (const auto& w : windows) {
        if (!w.fullscreen) {
            continue;
        }
        if (!windowOnCurrentDesktop(w)) {
            continue;
        }
        // Ignore desktop/dock chrome even if it claims fullscreen.
        if (w.desktopOnly || w.dock) {
            continue;
        }
        // Fullscreen means the window owns the whole output. Require it to
        // actually overlap the wallpaper area substantially, so a
        // fullscreen window on a different output does not pause this one.
        const int32_t ox0 = std::max(w.x, bounds.x);
        const int32_t oy0 = std::max(w.y, bounds.y);
        const int32_t ox1 = std::min(w.x + w.width, bounds.x + bounds.width);
        const int32_t oy1 = std::min(w.y + w.height, bounds.y + bounds.height);
        const long long overlap = static_cast<long long>(ox1 - ox0) * (oy1 - oy0);
        const long long full = static_cast<long long>(bounds.width) * bounds.height;
        if (full > 0 && overlap >= full * m_threshold) {
            return true;
        }
    }
    return false;
}

bool X11EwmhBackend::isOutputCovered(const std::string& outputName) const {
    if (!m_initialized) {
        return false;
    }
    if (outputName != m_outputName) {
        return false;
    }
    if (m_rootRect.width <= 0 || m_rootRect.height <= 0) {
        return false;
    }

    // Fullscreen-family window on this output -> covered.
    if (hasFullscreenCovering(m_windows, m_rootRect)) {
        return true;
    }

    // Otherwise, union of "real" window rects. Chrome (panels, desktop,
    // utilities, splash, skip-taskbar) must not count, otherwise a docked
    // panel would look like a covering window.
    std::vector<Rect> rects;
    for (const auto& w : m_windows) {
        if (w.fullscreen) {
            continue; // handled above
        }
        if (w.desktopOnly || w.dock || w.utility || w.splash) {
            continue;
        }
        if (w.skipTaskbar) {
            continue;
        }
        if (!windowOnCurrentDesktop(w)) {
            continue;
        }
        rects.push_back({w.x, w.y, w.width, w.height});
    }

    const long long rootArea = static_cast<long long>(m_rootRect.width) * m_rootRect.height;
    if (rootArea <= 0) {
        return false;
    }
    const long long covered = unionArea(rects, m_rootRect);
    return static_cast<double>(covered) / static_cast<double>(rootArea) >= m_threshold;
}

std::unique_ptr<CompositorBackend> makeX11EwmhBackend() {
    auto backend = std::make_unique<X11EwmhBackend>();
    if (!backend->initialize()) {
        return nullptr;
    }
    return backend;
}

} // namespace WallpaperEngine::Scene
