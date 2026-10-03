#include "pause_gate.h"

namespace WallpaperEngine::Scene {

bool shouldRender(const CompositorBackend& backend,
                  const PauseGateConfig& config) {
    if (!config.enabled) {
        return true;
    }

    const auto outputs = backend.outputNames();
    if (outputs.empty()) {
        return true;
    }

    bool anyCovered = false;
    bool anyUncovered = false;
    for (const auto& out : outputs) {
        if (backend.isOutputCovered(out)) {
            anyCovered = true;
        } else {
            anyUncovered = true;
        }
    }

    if (config.pauseAllOutputs) {
        return !anyCovered;
    }

    return anyUncovered;
}

} // namespace WallpaperEngine::Scene
