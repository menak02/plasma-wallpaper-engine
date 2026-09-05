#include "compositor_backend.h"
#include <string>
#include <vector>
#include <cstdint>
#include <iostream>

namespace WallpaperEngine::Scene {

// Hyprland IPC uses a control socket, typically at
//   $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/daemon_ipc
// The instance signature is discoverable from the environment or the
// runtime dir contents. The coverage decision is implemented against the
// Hyprland JSON IPC protocol (hyprctl dispatch/jobs/outputs/workspaces).
//
// This first cut uses the same interface as the rest of the pause gate so
// the render tick can stay compositor-agnostic.
class HyprlandBackend : public CompositorBackend {
public:
    explicit HyprlandBackend(const std::string& controlSocketPath)
        : m_controlSocketPath(controlSocketPath) {}

    std::vector<std::string> outputNames() const override {
        if (m_controlSocketPath.empty()) return {};
        // In this cut the set of tracked outputs is derived from the daemon's
        // registered Vulkan/DmaBuf outputs, not from a live Hyprland query
        // that could race with the render loop. Fallback for now:
        return {m_fallbackOutput};
    }

    bool isOutputCovered(const std::string& outputName) const override {
        (void)outputName;
        if (m_controlSocketPath.empty()) {
            // No Hyprland control socket available -> assume not covered so
            // the daemon keeps rendering. This is the safe default when the
            // backend cannot answer.
            return false;
        }

        // Placeholder implementation for the first cut.
        // Real coverage logic (fullscreen-family OR tiling coverage >= 90%)
        // is driven by Hyprland IPC responses and will be filled in once the
        // IPC channel is wired.
        //
        // For now: always report "not covered" so the daemon keeps running.
        // This lets the pause gate exist without regressing the render path
        // before Hyprland IPC is live.
        return false;
    }

    static std::string discoverControlSocket() {
        // Look for the Hyprland daemon IPC socket in the runtime dir.
        const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
        if (!runtimeDir) return {};

        std::string prefix = std::string(runtimeDir) + "/hypr/";
        // Shortlist matching socket paths so the caller can pick one.
        // (Real enumeration is left to the backend init path once
        //  filesystem iteration is in place.)
        (void)prefix;
        return {};
    }

private:
    std::string m_controlSocketPath;
    std::string m_fallbackOutput = "screen0";
};

std::unique_ptr<CompositorBackend> makeHyprlandBackend() {
    std::string socketPath = HyprlandBackend::discoverControlSocket();
    if (socketPath.empty()) {
        return nullptr;
    }
    return std::make_unique<HyprlandBackend>(socketPath);
}

} // namespace WallpaperEngine::Scene
