#pragma once

#include "compositor_backend.h"
#include <string>
#include <vector>
#include <cstdint>

struct xcb_connection_t;
struct xcb_screen_t;

namespace WallpaperEngine::Scene {

/** X11 EWMH backend for the pause gate.

    Answers "is this output covered?" on a plain X server (XFCE/xfwm4,
    LXDE, MATE, ...) by reading the EWMH properties the window manager
    already maintains:

      _NET_CLIENT_LIST           managed top-level windows
      _NET_CLIENT_LIST_STACKING  bottom-to-top stacking order
      _NET_CURRENT_DESKTOP       the visible workspace
      _NET_WM_DESKTOP            per-window workspace (0xFFFFFFFF = all)
      _NET_WM_STATE_FULLSCREEN   fullscreen-family detection
      _NET_WM_WINDOW_TYPE        skip DOCK/DESKTOP/UTILITY chrome
      _NET_DESKTOP_GEOMETRY      total root area

    Coverage follows the same model as the Hyprland backend: an output is
    covered when a fullscreen window is on it, or when the union of
    normal-window rects reaches the coverage threshold. Window rects are
    unioned rather than summed so two overlapping windows cannot
    double-count their overlap into a false "covered".
*/
class X11EwmhBackend : public CompositorBackend {
public:
    X11EwmhBackend() = default;
    ~X11EwmhBackend() override;

    X11EwmhBackend(const X11EwmhBackend&) = delete;
    X11EwmhBackend& operator=(const X11EwmhBackend&) = delete;

    /** Connects to $DISPLAY (or the given display name) and snapshots the
        EWMH state. Returns false when no X server is reachable, which is
        the normal outcome in a pure Wayland session. */
    bool initialize(const char* displayName = nullptr);

    std::vector<std::string> outputNames() const override;
    bool isOutputCovered(const std::string& outputName) const override;
    void update() override;
    double coverageThreshold() const override { return m_threshold; }

    /** Exposed for the decision test: threshold can be overridden so tests
        do not need a live X server to exercise the coverage math. */
    void setCoverageThreshold(double threshold) { m_threshold = threshold; }

    /** Human-readable name of the detected window manager, for logging. */
    std::string windowManagerName() const { return m_wmName; }

    /** Geometry of a window or of the wallpaper output. */
    struct Rect {
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = 0;
        int32_t height = 0;
    };

    /** Union area of the given rects, clipped to `bounds`.

        Public so the coverage math can be unit-tested without a live X
        server. This is the piece that must not be a plain sum: two
        overlapping windows would otherwise double-count the overlap and
        report a false "covered". */
    static long long unionArea(const std::vector<Rect>& rects, const Rect& bounds);

    /** The threshold decision the backend makes after unioning: true when
        `unionPx` reaches `threshold` of the bounds area. A zero-area bounds
        can never be covered. Inline so the decision stays testable while the
        XCB plumbing stays private. */
    static bool unionReachesThreshold(long long unionPx, const Rect& bounds,
                                      double threshold) {
        const long long total = static_cast<long long>(bounds.width) * bounds.height;
        if (total <= 0) {
            return false;
        }
        return static_cast<double>(unionPx) / static_cast<double>(total) >= threshold;
    }

private:
    struct WindowInfo {
        uint32_t id = 0;
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = 0;
        int32_t height = 0;
        int32_t borderWidth = 0;
        bool fullscreen = false;
        bool skipTaskbar = false;
        bool skipPager = false;
        bool sticky = false;
        int64_t desktop = -1;
        bool desktopOnly = false;   // _NET_WM_WINDOW_TYPE_DESKTOP
        bool dock = false;          // _NET_WM_WINDOW_TYPE_DOCK
        bool utility = false;       // _NET_WM_WINDOW_TYPE_UTILITY
        bool splash = false;        // _NET_WM_WINDOW_TYPE_SPLASH
        bool windowTypeDialog = false; // _NET_WM_WINDOW_TYPE_DIALOG
        bool mapped = false;
    };

    bool connectXcb(const char* displayName);
    void cacheAtoms();
    bool queryRootProperties();
    std::vector<WindowInfo> queryWindows() const;

    /** Current visible workspace, or 0xFFFFFFFF when unknown. */
    uint32_t currentDesktop() const { return m_currentDesktop; }

    /** True when a window is visible on the current workspace. Sticky
        windows (0xFFFFFFFF) are visible everywhere. */
    bool windowOnCurrentDesktop(const WindowInfo& w) const;

    /** Per-window stacking index; higher sits closer to the user. */
    int stackingIndex(uint32_t windowId) const;

    /** True when any of `windows` is fullscreen and covers `bounds`. */
    bool hasFullscreenCovering(const std::vector<WindowInfo>& windows,
                               const Rect& bounds) const;

    xcb_connection_t* m_conn = nullptr;
    xcb_screen_t* m_screen = nullptr;

    // Cached EWMH atoms.
    uint32_t m_atomNetClientList = 0;
    uint32_t m_atomNetClientListStacking = 0;
    uint32_t m_atomNetCurrentDesktop = 0;
    uint32_t m_atomNetWmDesktop = 0;
    uint32_t m_atomNetWmState = 0;
    uint32_t m_atomNetWmStateFullscreen = 0;
    uint32_t m_atomNetWmStateSkipTaskbar = 0;
    uint32_t m_atomNetWmStateSkipPager = 0;
    uint32_t m_atomNetWmStateSticky = 0;
    uint32_t m_atomNetWmStateAbove = 0;
    uint32_t m_atomNetWmWindowType = 0;
    uint32_t m_atomNetWmWindowTypeDesktop = 0;
    uint32_t m_atomNetWmWindowTypeDock = 0;
    uint32_t m_atomNetWmWindowTypeUtility = 0;
    uint32_t m_atomNetWmWindowTypeSplash = 0;
    uint32_t m_atomNetWmWindowTypeDialog = 0;
    uint32_t m_atomNetSupportingWmCheck = 0;
    uint32_t m_atomNetWmName = 0;
    uint32_t m_atomUtf8String = 0;
    uint32_t m_atomNetDesktopGeometry = 0;
    uint32_t m_atomNetDesktopNames = 0;

    uint32_t m_currentDesktop = 0;
    bool m_haveCurrentDesktop = false;
    bool m_initialized = false;
    double m_threshold = 0.90;
    std::string m_wmName;

    /** Single-screen setups report one output. XFCE here drives one
        monitor, so the root geometry is the wallpaper area. */
    Rect m_rootRect;
    std::string m_outputName;

    std::vector<WindowInfo> m_windows;
    std::vector<uint32_t> m_stackingOrder;
};

/** Returns a backend for the X display in $DISPLAY, or nullptr when no
    X server is reachable. */
std::unique_ptr<CompositorBackend> makeX11EwmhBackend();

} // namespace WallpaperEngine::Scene
