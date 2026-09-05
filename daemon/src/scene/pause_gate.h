#pragma once

#include <string>
#include <vector>
#include "compositor_backend.h"

namespace WallpaperEngine::Scene {

/** Pause gate decision logic. This is the testable core of the pause feature;
    the live daemon wires it into WallpaperService and the audio pause signal.
 */
struct PauseGateConfig {
    bool enabled = false;
    bool pauseAllOutputs = false;
};

/** Returns true when the frame should be rendered given the backend state and
    pause config. */
bool shouldRender(const CompositorBackend& backend,
                  const PauseGateConfig& config);

} // namespace WallpaperEngine::Scene
