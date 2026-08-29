#pragma once

#include <memory>
#include <QImage>
#include "scene_parser.h"
#include "particle_engine.h"
#include "../vulkan/vulkan_context.h"
#include "../render/render_graph.h"
#include "../render/mesh_renderer.h"

namespace WallpaperEngine::Scene {

class SceneCompositor {
public:
    explicit SceneCompositor(Render::VulkanContext* vulkanCtx);
    ~SceneCompositor() = default;

    bool loadScene(Assets::PkgReader& pkgReader);
    void updateAndRender(float dt, float time);
    void setMouseParallax(float normX, float normY);
    void setTargetResolution(uint32_t width, uint32_t height);

    const SceneDescription& getScene() const { return m_scene; }
    bool hasScene() const { return m_hasScene; }

private:
    Render::VulkanContext* m_vulkanCtx = nullptr;
    Render::RenderGraph m_renderGraph;
    SceneDescription m_scene;
    ParticleEngine m_particleEngine;
    bool m_hasScene = false;

    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    QImage m_canvas;

    float m_mouseX = 0.5f;
    float m_mouseY = 0.5f;
};

} // namespace WallpaperEngine::Scene
