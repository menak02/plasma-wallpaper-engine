#pragma once

#include <memory>
#include <QImage>
#include "scene_parser.h"
#include "particle_engine.h"
#include "../vulkan/vulkan_context.h"
#include "../render/render_graph.h"
#include "../render/mesh_renderer.h"
#include "../render/vulkan_compute.h"

namespace WallpaperEngine::Scene {

class SceneCompositor {
public:
    explicit SceneCompositor(Render::VulkanContext* vulkanCtx);
    ~SceneCompositor();

    bool loadScene(Assets::PkgReader& pkgReader);
    bool loadScene(Assets::PkgReader& pkgReader, const std::unordered_map<std::string, QVariant>& overrideProps);
    bool reloadWithProperties(const std::unordered_map<std::string, QVariant>& props);
    bool loadWeb(const std::string& html);
    void setWebProperty(const QString& key, const QVariant& value);
    void updateAndRender(float dt, float time);
    void setMouseParallax(float normX, float normY);
    void setTargetResolution(uint32_t width, uint32_t height);

    const SceneDescription& getScene() const { return m_scene; }
    bool hasScene() const { return m_hasScene; }
    bool isWeb() const;

private:
    Render::VulkanContext* m_vulkanCtx = nullptr;
    Render::VulkanCompute m_compute;
    bool m_hasCompute = false;
    Render::RenderGraph m_renderGraph;
    SceneDescription m_scene;
    ParticleEngine m_particleEngine;
    bool m_hasScene = false;
    bool m_isWeb = false;
    std::unique_ptr<class WebWallpaper> m_web;
    Assets::PkgReader* m_lastPkg = nullptr;
    std::unordered_map<std::string, QVariant> m_lastOverrideProps;

    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    QImage m_canvas;

    float m_mouseX = 0.5f;
    float m_mouseY = 0.5f;

    bool initComputePipelines();
};

} // namespace WallpaperEngine::Scene
