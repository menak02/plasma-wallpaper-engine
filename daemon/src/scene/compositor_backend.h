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

} // namespace WallpaperEngine::Scene
