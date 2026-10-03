#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace WallpaperEngine::Scene {

/** Compositor-specific backend that answers "is this wallpaper output
    covered right now?".

    The pause gate asks this per output; different compositors implement
    the query differently (Hyprland IPC, KWin D-Bus, X11 geometry).
 */
class CompositorBackend {
public:
    virtual ~CompositorBackend() = default;

    /** Returns the names of outputs the daemon should track. For a single
        screen setup this may be one entry; for multi-output setups it may
        be more. Empty means "no compositor backend available". */
    virtual std::vector<std::string> outputNames() const = 0;

    /** Whether the given output is covered right now.

    True means this output should pause (no tick, OST muted by default).
    Decision uses fullscreen-family windows OR tiling coverage >= threshold. */
    virtual bool isOutputCovered(const std::string& outputName) const = 0;

    /** Update internal state from IPC events. Called periodically to
        process pending events and refresh coverage state. */
    virtual void update() {}

    /** Coverage threshold used for tiling/pseudotile coverage calculations.
        0.0 - 1.0, default 0.90. */
    virtual double coverageThreshold() const { return 0.90; }
};

std::unique_ptr<CompositorBackend> makeHyprlandBackend();

/** Backends that live in their own translation units, declared here so the
    session detection below can call them. The definitions are added to the
    build by CMake, and the headers are included by compositor_backend.cpp
    behind __has_include — NOT here, because x11_ewmh_backend.h includes this
    header and would need CompositorBackend to be complete at that point. */
std::unique_ptr<CompositorBackend> makeLabwcBackend();   // labwc_backend.cpp
std::unique_ptr<CompositorBackend> makeX11EwmhBackend(); // x11_ewmh_backend.cpp

/** Detects the session the daemon is running in and returns the best backend
    available for it, or nullptr when none is usable.

    Detection order (first match wins, each candidate is probed cheaply
    before it is asked to do any real work):

      1. Hyprland   - $HYPRLAND_INSTANCE_SIGNATURE set, or a live IPC socket
                      under $XDG_RUNTIME_DIR/hypr/.
      2. labwc      - $WAYLAND_DISPLAY set and the display socket answers;
                      the backend itself then verifies the wlr-IPC globals.
      3. X11 EWMH   - $DISPLAY set.
      4. nullptr    - nothing usable.

    A candidate that is present but fails to initialize does not stop the
    search: the next candidate is tried and, at worst, nullptr is returned.
    nullptr is a safe outcome, not an error — the pause gate simply never
    trips and the daemon keeps rendering. */
std::unique_ptr<CompositorBackend> makeCompositorBackend();

} // namespace WallpaperEngine::Scene
