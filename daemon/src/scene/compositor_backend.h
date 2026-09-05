#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace WallpaperEngine::Scene {

/** Compositor-specific backend that answers "is this wallpaper output
    covered right now?".

    The pause gate asks this per output. Different compositors implement
    the same query differently (Hyprland IPC, KWin D-Bus, X11 geometry,
    etc.). This interface is the stable seam.
 */
class CompositorBackend {
public:
    virtual ~CompositorBackend() = default;

    /** Returns the names of outputs the daemon should track. For a single
        screen setup this may be one entry; for multi-output setups it may
        be more. Empty means "no compositor backend available". */
    virtual std::vector<std::string> outputNames() const = 0;

    /** Whether the given output is covered right now.

        A return of true means the wallpaper on this output should pause
        (no tick, OST muted by default). False means keep rendering.
        The decision is based on fullscreen-family windows OR tiling
        coverage >= the configured threshold on the given output. */
    virtual bool isOutputCovered(const std::string& outputName) const = 0;
};

std::unique_ptr<CompositorBackend> makeHyprlandBackend();

} // namespace WallpaperEngine::Scene
