#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <QImage>
#include <QColor>
#include "vulkan_compute.h"

namespace WallpaperEngine::Render {

struct RenderTarget {
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    QImage image;
};

class RenderGraph {
public:
    RenderGraph() = default;
    ~RenderGraph() = default;

    bool init(VulkanContext* vulkanCtx);
    void cleanup();

    void setResolution(uint32_t width, uint32_t height);
    void clear();

    QImage& getOrCreateRenderTarget(const std::string& name);
    bool hasRenderTarget(const std::string& name) const;
    void copyFramebufferToRenderTarget(const std::string& name, const QImage& sourceCanvas);

    // Render-Pass Shader Emulation Filters (now using Vulkan compute)
    QImage applyBlurPass(const QImage& input, float radius, bool vertical);
    QImage applyWaterWavesPass(const QImage& input, const QImage& mask, float speed, float scale, float strength, float direction, float time);
    QImage applyPulsePass(const QImage& input, const QImage& mask, float speed, float amount, float power, float time);
    QImage applyCompositionPass(const QImage& currentCanvas, const QImage& backgroundBuffer, const std::string& blendMode);

private:
    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    std::unordered_map<std::string, RenderTarget> m_renderTargets;
    VulkanCompute m_vulkanCompute;
};

} // namespace WallpaperEngine::Render
