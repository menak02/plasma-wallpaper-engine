#pragma once

#include <memory>
#include <QImage>
#include "scene_parser.h"
#include "particle_engine.h"
#include "../vulkan/vulkan_context.h"
#include "../audio/audio_visualizer.h"
#include "../render/render_graph.h"
#include "../render/mesh_renderer.h"
#include "../render/vulkan_compute.h"
#include "../assets/video_decoder.h"

namespace WallpaperEngine::Scene {

class SceneCompositor {
public:
    explicit SceneCompositor(Render::VulkanContext* vulkanCtx);
    ~SceneCompositor();

    bool loadScene(Assets::PkgReader& pkgReader);
    bool loadScene(Assets::PkgReader& pkgReader, const std::unordered_map<std::string, QVariant>& overrideProps);
    bool reloadWithProperties(const std::unordered_map<std::string, QVariant>& props);
    bool loadWeb(const std::string& html);
    // Standalone video wallpaper (project.json "type":"video" with a loose
    // media file — no scene.pkg archive involved).
    bool loadVideo(const std::string& videoFilePath);
    void setWebProperty(const QString& key, const QVariant& value);
    void updateAndRender(float dt, float time);
    void setMouseParallax(float normX, float normY);
    void setTargetResolution(uint32_t width, uint32_t height);

    // Audio-reactive support: live capture control + per-frame band access.
    Audio::AudioVisualizer& audioVisualizer() { return m_audioVisualizer; }
    bool startAudioCapture();
    void stopAudioCapture();

    const SceneDescription& getScene() const { return m_scene; }
    bool hasScene() const { return m_hasScene; }
    bool isWeb() const;

    // Post-composite post-processing info (scene-level effects like film grain).
    bool hasFilmGrain() const { return m_hasFilmGrain; }

private:
    Render::VulkanContext* m_vulkanCtx = nullptr;
    Render::VulkanCompute m_compute;
    bool m_hasCompute = false;
    Render::RenderGraph m_renderGraph;
    SceneDescription m_scene;
    ParticleEngine m_particleEngine;
    Audio::AudioVisualizer m_audioVisualizer;
    bool m_hasScene = false;
    bool m_isWeb = false;
    bool m_isVideo = false;
    std::unique_ptr<class WebWallpaper> m_web;
    std::shared_ptr<Assets::VideoDecoder> m_video;
    float m_videoAcc = 0.0f;
    Assets::PkgReader* m_lastPkg = nullptr;
    std::unordered_map<std::string, QVariant> m_lastOverrideProps;

    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    QImage m_canvas;

    float m_mouseX = 0.5f;
    float m_mouseY = 0.5f;

    bool initComputePipelines();

    // Post-composite post-processing state. Film grain is a scene-level
    // screen-space effect in Wallpaper Engine, applied once after all
    // layers/particles are composited.
    void scanPostEffects();
    bool m_hasFilmGrain = false;
    float m_grainPower = 0.0f;
    float m_grainScale = 4.0f;
};

} // namespace WallpaperEngine::Scene
