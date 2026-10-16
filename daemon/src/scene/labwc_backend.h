#pragma once

#include "compositor_backend.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace WallpaperEngine::Scene {

/** labwc (wlroots) backend for the pause gate.

    Answers "is this output covered?" on labwc / any wlroots compositor by
    speaking wlr-ipc-unstable-v2 directly on the wire:

      zwlr_output_manager_v1           output names, positions, mode sizes
      zwlr_foreign_toplevel_manager_v1  the toplevel list (swaymsg get_tree)
      zwlr_foreign_toplevel_handle_v1   title/app_id/state/output_enter
      zxdg_output_manager_v1            output logical position/size
      wl_output                         per-output geometry + the object
                                       identity used by output_enter

    Coverage follows the same model as the Hyprland backend: an output is
    covered when a fullscreen-family toplevel is on it, or when the union of
    normal-toplevel rects reaches the coverage threshold. Rects are unioned,
    never summed, so overlapping windows cannot double-count into a false
    "covered".

    Known limitation, see labwc_backend.cpp for the full discussion:
    wlr-foreign-toplevel-management carries no per-window geometry, so
    non-maximized windows contribute no rect and the union only ever holds
    output-sized rects derived from fullscreen/maximized state. Everything
    else (socket, registry, all four globals, event state) is live.

    Not thread-safe: call update() from the same thread as the rest of the
    pause gate, the way the Hyprland backend is used.
*/
class LabwcBackend : public CompositorBackend {
public:
    LabwcBackend();
    ~LabwcBackend() override;

    LabwcBackend(const LabwcBackend&) = delete;
    LabwcBackend& operator=(const LabwcBackend&) = delete;

    /** Connects to $WAYLAND_DISPLAY (or `displayName`) and binds the
        required globals. Returns false when no Wayland display is reachable
        or zwlr_foreign_toplevel_manager_v1 is missing, which is the normal
        outcome on an X11 session or under a non-wlroots compositor. Never
        throws. */
    bool initialize(const char* displayName = nullptr);

    std::vector<std::string> outputNames() const override;
    bool isOutputCovered(const std::string& outputName) const override;
    void update() override;
    double coverageThreshold() const override { return m_threshold; }

    /** Exposed so the coverage math can be exercised without a live labwc
        session, mirroring the X11 EWMH backend's test surface. */
    void setCoverageThreshold(double threshold) { m_threshold = threshold; }

    /** Geometry of a window or of the wallpaper output. */
    struct Rect {
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = 0;
        int32_t height = 0;
    };

    /** Union area of `rects`, clipped to `bounds`.

        This is the piece that must not be a plain sum: two overlapping
        windows would otherwise count their overlap twice and report a false
        "covered". Sweep-line over x slabs, independent of the X11 backend's
        implementation (which this file deliberately does not include). */
    static long long unionArea(const std::vector<Rect>& rects, const Rect& bounds);

    /** The threshold decision made after unioning. A zero-area bounds can
        never be covered. Inline so the decision is testable without a
        Wayland connection. */
    static bool unionReachesThreshold(long long unionPx, const Rect& bounds,
                                      double threshold) {
        const long long total = static_cast<long long>(bounds.width) * bounds.height;
        if (total <= 0) {
            return false;
        }
        return static_cast<double>(unionPx) / static_cast<double>(total) >= threshold;
    }

    /** All Wayland state lives in the .cpp behind this incomplete type, so
        this header never has to include <wayland-client.h>. Public only so
        the free listener trampolines in the .cpp can name it. */
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
    double m_threshold = 0.90;
};

/** Returns a backend for the Wayland display in $WAYLAND_DISPLAY, or nullptr
    when no display is reachable or the required globals are absent. */
std::unique_ptr<CompositorBackend> makeLabwcBackend();

} // namespace WallpaperEngine::Scene
