#pragma once

#include "compositor_backend.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace WallpaperEngine::Scene {

/** Fake backend for headless tests. Coverage decisions are injected so the
    pause gate can be exercised without a live compositor. */
class FakeCompositorBackend : public CompositorBackend {
public:
    FakeCompositorBackend() = default;

    std::vector<std::string> outputNames() const override {
        std::vector<std::string> out;
        for (const auto& [name, _] : m_coverage) {
            out.push_back(name);
        }
        return out;
    }

    bool isOutputCovered(const std::string& outputName) const override {
        auto it = m_coverage.find(outputName);
        if (it == m_coverage.end()) {
            return false;
        }
        return it->second;
    }

    void setCovered(const std::string& outputName, bool covered) {
        m_coverage[outputName] = covered;
    }

private:
    std::unordered_map<std::string, bool> m_coverage;
};

} // namespace WallpaperEngine::Scene
