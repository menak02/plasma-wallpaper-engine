#include "scene_compositor.h"
#include "web_wallpaper.h"
#include "../render/shaders_spv.h"
#include <QPainter>
#include <QRadialGradient>
#include <QTemporaryFile>
#include <QDir>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace WallpaperEngine::Scene {

// Accumulated transform from parent chain
struct AccumulatedTransform {
    QVector2D origin{0.0f, 0.0f}; // Final screen position
    QVector2D scale{1.0f, 1.0f};  // Cumulative scale
    float angle = 0.0f;            // Cumulative rotation (degrees)
};

// Walk parent chain, accumulate transforms (root first)
static AccumulatedTransform resolveParentTransform(const SceneLayer& layer, float sceneW, float sceneH) {
    // Build chain: root → ... → layer
    const SceneLayer* chain[32];
    int count = 0;
    const SceneLayer* cur = &layer;
    chain[count++] = cur;
    while (cur->parent != nullptr && count < 32) {
        cur = cur->parent;
        chain[count++] = cur;
    }

    // Accumulate from root downward
    AccumulatedTransform acc;
    acc.scale = QVector2D(1.0f, 1.0f);

    for (int i = count - 1; i >= 0; --i) {
        const SceneLayer& node = *chain[i];
        float ox = node.origin.x();
        float oy = node.origin.y();
        float sx = node.scale.x();
        float sy = node.scale.y();
        float angle = node.angles.z(); // z-rotation in degrees

        // Rotate child origin by accumulated angle, scale by accumulated scale
        float rad = acc.angle * 3.14159265f / 180.0f;
        float cosA = std::cos(rad);
        float sinA = std::sin(rad);
        float rotatedX = (ox * acc.scale.x()) * cosA - (oy * acc.scale.y()) * sinA;
        float rotatedY = (ox * acc.scale.x()) * sinA + (oy * acc.scale.y()) * cosA;

        acc.origin.setX(acc.origin.x() + rotatedX);
        acc.origin.setY(acc.origin.y() + rotatedY);
        acc.scale.setX(acc.scale.x() * sx);
        acc.scale.setY(acc.scale.y() * sy);
        acc.angle += angle;
    }

    return acc;
}

SceneCompositor::SceneCompositor(Render::VulkanContext* vulkanCtx)
    : m_vulkanCtx(vulkanCtx) {
    m_canvas = QImage(m_width, m_height, QImage::Format_RGBA8888);
    m_canvas.fill(Qt::black);
    m_renderGraph.setResolution(m_width, m_height);
    if (m_vulkanCtx && m_vulkanCtx->getDevice() != VK_NULL_HANDLE) {
        m_hasCompute = m_compute.init(m_vulkanCtx);
        if (m_hasCompute) m_hasCompute = initComputePipelines();
        std::cout << "SceneCompositor: VulkanCompute " << (m_hasCompute ? "ACTIVE" : "fallback to QPainter") << std::endl;
    }
}
SceneCompositor::~SceneCompositor() = default;

bool SceneCompositor::initComputePipelines() {
    using namespace Render::Shaders;
    // Blur: binding 0 sampler2D, 1 storage image
    std::vector<VkDescriptorSetLayoutBinding> blurBindings(2);
    blurBindings[0].binding=0; blurBindings[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; blurBindings[0].descriptorCount=1; blurBindings[0].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    blurBindings[1].binding=1; blurBindings[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; blurBindings[1].descriptorCount=1; blurBindings[1].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    m_compute.createPipeline("blur", blur_spv, blurBindings);
    // Water waves: 0 sampler, 1 sampler, 2 storage
    std::vector<VkDescriptorSetLayoutBinding> waveBindings(3);
    waveBindings[0].binding=0; waveBindings[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; waveBindings[0].descriptorCount=1; waveBindings[0].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    waveBindings[1].binding=1; waveBindings[1].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; waveBindings[1].descriptorCount=1; waveBindings[1].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    waveBindings[2].binding=2; waveBindings[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; waveBindings[2].descriptorCount=1; waveBindings[2].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    m_compute.createPipeline("water_waves", water_waves_spv, waveBindings);
    m_compute.createPipeline("waterwaves", water_waves_spv, waveBindings);
    // Pulse: same as waves
    m_compute.createPipeline("pulse", pulse_spv, waveBindings);
    // Composition: 0 sampler,1 sampler,2 storage
    m_compute.createPipeline("composition", composition_spv, waveBindings);
    // Film grain: 0 sampler, 1 storage (frame in, grained frame out)
    std::vector<VkDescriptorSetLayoutBinding> grainBindings(2);
    grainBindings[0].binding=0; grainBindings[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; grainBindings[0].descriptorCount=1; grainBindings[0].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    grainBindings[1].binding=1; grainBindings[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; grainBindings[1].descriptorCount=1; grainBindings[1].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    m_compute.createPipeline("film_grain", film_grain_spv, grainBindings);
    return m_compute.getPipeline("blur") != nullptr;
}

void SceneCompositor::scanPostEffects() {
    // Film grain is a scene-level screen-space effect in Wallpaper Engine:
    // find the most recently declared visible instance among all layers and
    // use its power/scale. Deterministic (no randomness at scan time).
    m_hasFilmGrain = false;
    m_grainPower = 0.0f;
    m_grainScale = 4.0f;
    for (const auto& layer : m_scene.layers) {
        if (!layer.visible) continue;
        for (const auto& eff : layer.effects) {
            if (!eff.visible || eff.type != EffectType::FilmGrain) continue;
            m_hasFilmGrain = true;
            m_grainPower = eff.strength;
            m_grainScale = eff.scale > 0.0f ? eff.scale : 4.0f;
        }
    }
    if (m_hasFilmGrain) {
        std::cout << "SceneCompositor: FilmGrain post-process power=" << m_grainPower
                  << " scale=" << m_grainScale << std::endl;
    }
}

void SceneCompositor::setTargetResolution(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    m_width = width;
    m_height = height;
    m_canvas = QImage(m_width, m_height, QImage::Format_RGBA8888);
    m_canvas.fill(Qt::black);
    m_renderGraph.setResolution(m_width, m_height);
}

void SceneCompositor::setMouseParallax(float normX, float normY) {
    m_mouseX = std::clamp(normX, 0.0f, 1.0f);
    m_mouseY = std::clamp(normY, 0.0f, 1.0f);
}

bool SceneCompositor::startAudioCapture() {
    // Capture targets the default sink monitor (system output) only — never
    // a microphone — and also hears the wallpaper's own OST playback.
    return m_audioVisualizer.startLiveCapture();
}

void SceneCompositor::stopAudioCapture() {
    m_audioVisualizer.stopLiveCapture();
}

bool SceneCompositor::loadScene(Assets::PkgReader& pkgReader) {
    return loadScene(pkgReader, {});
}
bool SceneCompositor::loadScene(Assets::PkgReader& pkgReader, const std::unordered_map<std::string, QVariant>& overrideProps) {
    m_hasScene = false;
    m_isWeb = false;
    m_lastPkg = &pkgReader;
    m_lastOverrideProps = overrideProps;
    SceneDescription desc;
    if (!SceneParser::parseScene(pkgReader, desc, overrideProps)) {
        return false;
    }
    m_scene = std::move(desc);
    m_particleEngine.setEmitters(m_scene.emitters);
    m_renderGraph.clear();
    scanPostEffects();
    m_hasScene = true;
    // GPU/CPU decision is per-scene at load time, never mid-frame.
    m_gpuCompositing = tryInitGpuCompositing();
    std::cout << "SceneCompositor: Loaded scene '" << m_scene.title
              << "' with " << m_scene.layers.size() << " layers"
              << " [renderer: " << (m_gpuCompositing ? "gpu" : "cpu") << "]" << std::endl;
    return true;
}

bool SceneCompositor::sceneSupportsGpuCompositing() const {
    // Mesh-deformation effects (waterwaves/waterripple/wind/foliagesway)
    // render on the GPU via the deform_quad.vert grid pipeline, matching the
    // CPU MeshDeformer::deformVertices math. Puppet bones still force the
    // CPU path; per-layer opacity masks are baked into layer images by the
    // parser, so they are GPU-safe.
    for (const auto& layer : m_scene.layers) {
        if (!layer.bones.empty()) return false;
    }
    return true;
}

bool SceneCompositor::tryInitGpuCompositing() {
    m_gpuGrain = false;
    if (!m_vulkanCtx || m_vulkanCtx->getDevice() == VK_NULL_HANDLE) return false;
    if (!sceneSupportsGpuCompositing()) return false;

    if (!m_gpuCompositor.isInitialized()) {
        if (!m_gpuCompositor.init(m_vulkanCtx, m_width, m_height)) {
            std::cerr << "SceneCompositor: GPU compositor init failed (" << m_gpuCompositor.lastError()
                      << "), staying on QPainter" << std::endl;
            return false;
        }
    }
    // Film grain moves to the GPU pass for scenes that have it.
    m_gpuGrain = m_hasFilmGrain;
    return true;
}

void SceneCompositor::buildGpuFrame(std::vector<Render::GpuLayer>& gpuLayers,
                                    std::vector<Render::GpuParticle>& gpuParticles,
                                    float time) {
    gpuLayers.clear();
    gpuParticles.clear();

    float parallaxOffsetX = (m_mouseX - 0.5f) * 35.0f;
    float parallaxOffsetY = (m_mouseY - 0.5f) * 35.0f;
    float sceneW = m_scene.sceneWidth > 0.0f ? m_scene.sceneWidth : 3840.0f;
    float sceneH = m_scene.sceneHeight > 0.0f ? m_scene.sceneHeight : 2160.0f;
    float scaleX = static_cast<float>(m_width) / sceneW;
    float scaleY = static_cast<float>(m_height) / sceneH;

    auto mapBlend = [](BlendMode b) -> uint32_t {
        switch (b) {
            case BlendMode::Additive: return 1;
            case BlendMode::Opaque: return 2;
            default: return 0; // translucent + all unhandled modes (multiply,
                               // screen, etc.) render as translucent — same
                               // degradation the CPU path documents.
        }
    };

    for (const auto& layer : m_scene.layers) {
        if (!layer.visible || layer.image.isNull()) continue;
        float effectiveOpacity = layer.opacity;
        if (layer.isInteractive) {
            float mouseDist = std::hypot(m_mouseX - 0.5f, m_mouseY - 0.5f);
            if (layer.name.find("zombie") != std::string::npos || layer.name.find("mutated") != std::string::npos) {
                effectiveOpacity *= std::clamp(1.0f - (mouseDist * 2.0f), 0.0f, 1.0f);
            }
        }
        if (effectiveOpacity <= 0.0f) continue;

        // Per-layer animation effects (breath/pulse/shake) — same math as the
        // CPU painter path so both renderers animate identically.
        float animOffsetX = 0.0f, animOffsetY = 0.0f, animScale = 1.0f;
        // Mesh-deform params: the CPU painter applies the LAST visible deform
        // effect (each one overwrites the flags), so mirror that here.
        bool hasMeshDeform = false;
        float deformSpeed = 1.0f, deformStrength = 0.0f, deformDirection = 0.0f;
        for (const auto& eff : layer.effects) {
            if (!eff.visible) continue;
            switch (eff.type) {
                case EffectType::Breath: {
                    float breathPhase = time * eff.speed * 2.0f;
                    animScale *= (1.0f + 0.02f * std::sin(breathPhase));
                    animOffsetY += std::sin(breathPhase) * 4.0f;
                    break;
                }
                case EffectType::Pulse: {
                    const float band = m_audioVisualizer.isLive() ? m_audioVisualizer.getBand(0) : 0.0f;
                    const float pulsePhase = time * eff.speed * 3.0f;
                    animScale *= (1.0f + eff.strength * std::sin(pulsePhase) + eff.strength * 2.0f * band);
                    break;
                }
                case EffectType::Wind:
                case EffectType::WaterWaves:
                case EffectType::WaterRipple:
                case EffectType::FoliageSway:
                    hasMeshDeform = true;
                    deformSpeed = eff.speed;
                    deformStrength = eff.strength;
                    deformDirection = eff.direction;
                    break;
                case EffectType::Shake: {
                    float shakePhase = time * eff.speed * 10.0f;
                    animOffsetX += std::sin(shakePhase * 1.3f) * eff.strength * 5.0f;
                    animOffsetY += std::cos(shakePhase * 1.7f) * eff.strength * 5.0f;
                    break;
                }
                default:
                    break;
            }
        }

        AccumulatedTransform acc = resolveParentTransform(layer, sceneW, sceneH);
        float finalX = (acc.origin.x() / sceneW) * m_width + parallaxOffsetX * layer.parallaxDepth.x() + animOffsetX;
        float finalY = ((sceneH - acc.origin.y()) / sceneH) * m_height + parallaxOffsetY * layer.parallaxDepth.y() + animOffsetY;

        float spriteW, spriteH;
        if (layer.size.x() > 0.0f && layer.size.y() > 0.0f) {
            spriteW = (layer.size.x() / sceneW) * m_width * acc.scale.x() * animScale;
            spriteH = (layer.size.y() / sceneH) * m_height * acc.scale.y() * animScale;
        } else {
            spriteW = layer.image.width() * scaleX * acc.scale.x() * animScale;
            spriteH = layer.image.height() * scaleY * acc.scale.y() * animScale;
        }

        Render::GpuLayer gl;
        gl.textureIndex = m_gpuCompositor.getOrCreateTexture(layer.image, layer.id);
        if (gl.textureIndex == UINT32_MAX) continue;
        gl.centerX = finalX;
        gl.centerY = finalY;
        gl.width = spriteW;
        gl.height = spriteH;
        gl.rotationRad = acc.angle * 3.14159265f / 180.0f;
        gl.opacity = std::clamp(effectiveOpacity, 0.0f, 1.0f);
        gl.blendMode = mapBlend(layer.blending);
        gl.deformed = hasMeshDeform;
        gl.deformSpeed = deformSpeed;
        gl.deformStrength = deformStrength;
        gl.deformDirection = deformDirection;
        gpuLayers.push_back(gl);
    }

    // Particles: the engine owns simulation; the GPU path only translates
    // its particle list into billboard instances (glow sprite, never a
    // per-particle texture — same as the CPU default sprite path).
    const auto& particles = m_particleEngine.particles();
    const auto& emitters = m_particleEngine.emitters();
    gpuParticles.reserve(particles.size());
    for (const auto& p : particles) {
        if (p.emitterIndex < 0 || p.emitterIndex >= static_cast<int>(emitters.size())) continue;
        Render::GpuParticle gp;
        gp.centerX = p.x;
        gp.centerY = p.y;
        gp.size = p.size;
        gp.rotationRad = p.rotation * 3.14159265f / 180.0f;
        gp.r = p.color.redF(); gp.g = p.color.greenF(); gp.b = p.color.blueF(); gp.a = p.alpha;
        gp.blendMode = emitters[p.emitterIndex].blending == BlendMode::Additive ? 1 : 0;
        gpuParticles.push_back(gp);
    }
}
bool SceneCompositor::reloadWithProperties(const std::unordered_map<std::string, QVariant>& props) {
    if (!m_lastPkg) return false;
    // Merge with last overrides
    auto merged = m_lastOverrideProps;
    for (auto& [k,v] : props) merged[k] = v;
    return loadScene(*m_lastPkg, merged);
}

bool SceneCompositor::loadWeb(const std::string& html) {
    m_hasScene = true;
    m_isWeb = true;
    m_scene = SceneDescription{};
    m_scene.title = "Web Wallpaper";
    m_scene.sceneWidth = m_width;
    m_scene.sceneHeight = m_height;
    // Create WebWallpaper offscreen
    if (!m_web) {
        m_web = std::make_unique<WebWallpaper>();
        m_web->setSize(m_width, m_height);
    }
    if (m_web->load(html)) {
        QImage webImg = m_web->grabImage();
        if (!webImg.isNull()) {
            SceneLayer layer;
            layer.name = "Web";
            layer.image = webImg;
            layer.visible = true;
            layer.opacity = 1.0f;
            layer.origin = QVector3D(m_scene.sceneWidth/2, m_scene.sceneHeight/2, 0);
            layer.size = QVector2D(m_scene.sceneWidth, m_scene.sceneHeight);
            m_scene.layers.push_back(std::move(layer));
            m_scene.totalVisualObjectsDeclared = 1;
        }
    }
    std::cout << "SceneCompositor: Loaded Web wallpaper " << html.size() << " bytes" << std::endl;
    return true;
}

bool SceneCompositor::isWeb() const { return m_isWeb; }

bool SceneCompositor::loadVideo(const std::string& videoFilePath) {
    auto decoder = std::make_shared<Assets::VideoDecoder>();
    if (!decoder->openFromFile(videoFilePath, 0, 0)) {
        std::cerr << "SceneCompositor: failed to open video wallpaper " << videoFilePath << std::endl;
        return false;
    }

    m_hasScene = true;
    m_isWeb = false;
    m_isVideo = true;
    m_video = std::move(decoder);
    m_videoAcc = 0.0f;

    m_scene = SceneDescription{};
    m_scene.title = "Video Wallpaper";
    m_scene.sceneWidth = static_cast<float>(m_width);
    m_scene.sceneHeight = static_cast<float>(m_height);

    // First frame becomes the initial layer image; later frames replace it
    // in the ~30fps video tick below.
    QImage first = m_video->decodeNextFrame();
    if (first.isNull()) {
        first = QImage(static_cast<int>(m_width), static_cast<int>(m_height), QImage::Format_ARGB32);
        first.fill(Qt::black);
    }
    SceneLayer layer;
    layer.name = "Video";
    layer.type = "video";
    layer.image = std::move(first);
    layer.visible = true;
    layer.opacity = 1.0f;
    layer.origin = QVector3D(m_scene.sceneWidth / 2.0f, m_scene.sceneHeight / 2.0f, 0);
    layer.size = QVector2D(m_scene.sceneWidth, m_scene.sceneHeight);
    m_scene.layers.push_back(std::move(layer));
    m_scene.totalVisualObjectsDeclared = 1;

    std::cout << "SceneCompositor: Loaded video wallpaper " << videoFilePath << std::endl;
    return true;
}
void SceneCompositor::setWebProperty(const QString& key, const QVariant& value) {
    if (m_web) m_web->setProperty(key, value);
}

void SceneCompositor::updateAndRender(float dt, float time) {
    if (!m_hasScene || !m_vulkanCtx) return;

    // Audio-reactive tick: sample the visualizer once per frame so band
    // energies reflect the current audio window.
    m_audioVisualizer.update();

    // Web: refresh from QWebEngineView each frame (throttled)
    if (m_isWeb && m_web) {
        QImage webImg = m_web->grabImage();
        if (!webImg.isNull() && !m_scene.layers.empty()) {
            m_scene.layers[0].image = std::move(webImg);
        }
    }

    // Standalone video wallpaper: advance by wall-clock video time and decode
    // as many frames as the elapsed time covers. A fixed decode cadence plays
    // high-fps sources in slow motion. Loop by seeking back to the start on
    // EOF so the wallpaper plays forever.
    if (m_isVideo && m_video && !m_scene.layers.empty()) {
        const double fps = std::max(1.0, m_video->framesPerSecond());
        m_videoAcc += dt;
        int steps = static_cast<int>(m_videoAcc * fps);
        if (steps > 8) {
            steps = 8; // cap catch-up work after a stall and drop the backlog
            m_videoAcc = 0.0f;
        } else if (steps > 0) {
            m_videoAcc -= static_cast<float>(steps / fps);
        }
        for (int i = 0; i < steps; ++i) {
            QImage next = m_video->decodeNextFrame();
            if (!next.isNull()) {
                m_scene.layers[0].image = std::move(next);
            } else {
                m_video->seekToStart();
                QImage retry = m_video->decodeNextFrame();
                if (!retry.isNull()) {
                    m_scene.layers[0].image = std::move(retry);
                }
            }
        }
    }

    // Tick video decoders (~30fps, single temp decoder to avoid multi-decoder heap corruption)
    static float videoAcc = 0.0f;
    static std::unordered_map<int, std::shared_ptr<Assets::VideoDecoder>> videoDecoders;
    static std::unordered_map<int, int> videoFramePos;
    videoAcc += dt;
    if (videoAcc > 1.0f/30.0f) {
        videoAcc = 0.0f;
        for (auto& layer : m_scene.layers) {
            if (layer.videoData.empty()) continue;
            auto it = videoDecoders.find(layer.id);
            if (it == videoDecoders.end()) {
                QTemporaryFile tmp(QDir::tempPath() + "/wp_XXXXXX.mp4");
                tmp.setAutoRemove(false);
                if (tmp.open()) {
                    tmp.write(reinterpret_cast<const char*>(layer.videoData.data()), layer.videoData.size());
                    tmp.close();
                    auto dec = std::make_shared<Assets::VideoDecoder>();
                    if (dec->openFromFile(tmp.fileName().toStdString(), 0, 0)) {
                        dec->decodeNextFrame(); // skip first frame already shown
                        videoDecoders[layer.id] = dec;
                        videoFramePos[layer.id] = 1;
                    }
                    QFile::remove(tmp.fileName());
                }
            } else {
                auto dec = it->second;
                if (!dec || !dec->isOpen()) continue;
                QImage next = dec->decodeNextFrame();
                if (!next.isNull()) {
                    layer.image = std::move(next);
                } else {
                    dec->seekToStart();
                    QImage retry = dec->decodeNextFrame();
                    if (!retry.isNull()) layer.image = std::move(retry);
                }
            }
        }
    }

    // GPU path: simulate particles, build instances, render on GPU, blit
    // straight into the dmabuf. Skips the CPU canvas entirely (no QPainter
    // composite, no 8MB staging upload per frame).
    if (m_gpuCompositing && m_gpuCompositor.isInitialized()) {
        m_particleEngine.update(dt, m_width, m_height);

        std::vector<Render::GpuLayer> gpuLayers;
        std::vector<Render::GpuParticle> gpuParticles;
        buildGpuFrame(gpuLayers, gpuParticles, time);

        const float clearColor[4] = {
            m_scene.clearColor.redF(), m_scene.clearColor.greenF(),
            m_scene.clearColor.blueF(), 1.0f
        };
        Render::GpuGrainParams grain;
        grain.enabled = m_gpuGrain;
        grain.power = m_grainPower;
        grain.scale = m_grainScale;
        grain.frame = std::floor(time);

        if (m_gpuCompositor.renderFrame(gpuLayers, gpuParticles, clearColor, grain, time)
            && m_gpuCompositor.blitIntoShared()) {
            return; // dmabuf written; CPU canvas skipped this frame
        }
        std::cerr << "SceneCompositor: GPU frame failed, falling back to CPU composite" << std::endl;
        m_gpuCompositing = false; // don't retry mid-scene; re-decided at next load
    }

    // 1. Clear background canvas
    m_canvas.fill(m_scene.clearColor);

    QPainter painter(&m_canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, true);

    float parallaxOffsetX = (m_mouseX - 0.5f) * 35.0f;
    float parallaxOffsetY = (m_mouseY - 0.5f) * 35.0f;

    float sceneW = m_scene.sceneWidth > 0.0f ? m_scene.sceneWidth : 3840.0f;
    float sceneH = m_scene.sceneHeight > 0.0f ? m_scene.sceneHeight : 2160.0f;
    float scaleX = static_cast<float>(m_width) / sceneW;
    float scaleY = static_cast<float>(m_height) / sceneH;

    // 2. Render all scene layers (topological order: parents before children)
    for (const auto& layer : m_scene.layers) {
        if (!layer.visible) continue;
        if (layer.image.isNull()) continue;

        float effectiveOpacity = layer.opacity;
        if (layer.isInteractive) {
            float mouseDist = std::hypot(m_mouseX - 0.5f, m_mouseY - 0.5f);
            if (layer.name.find("zombie") != std::string::npos || layer.name.find("mutated") != std::string::npos) {
                effectiveOpacity *= std::clamp(1.0f - (mouseDist * 2.0f), 0.0f, 1.0f);
            }
        }

        if (effectiveOpacity <= 0.0f) continue;

        painter.save();

        // Blend Mode Application
        switch (layer.blending) {
            case BlendMode::Additive:
                painter.setCompositionMode(QPainter::CompositionMode_Plus);
                break;
            case BlendMode::Multiply:
                painter.setCompositionMode(QPainter::CompositionMode_Multiply);
                break;
            case BlendMode::Screen:
                painter.setCompositionMode(QPainter::CompositionMode_Screen);
                break;
            case BlendMode::ColorDodge:
                painter.setCompositionMode(QPainter::CompositionMode_ColorDodge);
                break;
            case BlendMode::Overlay:
                painter.setCompositionMode(QPainter::CompositionMode_Overlay);
                break;
            case BlendMode::SoftLight:
                painter.setCompositionMode(QPainter::CompositionMode_SoftLight);
                break;
            case BlendMode::Opaque:
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                break;
            default:
                painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
                break;
        }

        painter.setOpacity(effectiveOpacity);

        // Resolve parent transform chain (accumulated origin/scale/angle)
        AccumulatedTransform acc = resolveParentTransform(layer, sceneW, sceneH);

        // scene-space (center-based, Y-UP) to screen-space (top-left, Y-DOWN).
        float finalX = (acc.origin.x() / sceneW) * m_width + parallaxOffsetX * layer.parallaxDepth.x();
        float finalY = ((sceneH - acc.origin.y()) / sceneH) * m_height + parallaxOffsetY * layer.parallaxDepth.y();

        float spriteW = 0.0f;
        float spriteH = 0.0f;

        if (layer.size.x() > 0.0f && layer.size.y() > 0.0f) {
            spriteW = (layer.size.x() / sceneW) * m_width * acc.scale.x();
            spriteH = (layer.size.y() / sceneH) * m_height * acc.scale.y();
        } else {
            spriteW = layer.image.width() * scaleX * acc.scale.x();
            spriteH = layer.image.height() * scaleY * acc.scale.y();
        }

        float animOffsetX = 0.0f;
        float animOffsetY = 0.0f;
        float animRotation = 0.0f;
        float animScale = 1.0f;

        bool hasMeshDeform = false;
        float deformSpeed = 1.0f;
        float deformStrength = 0.05f;
        float deformDirection = 0.0f;

        // Evaluate Effect Chains & Mesh Deformation Flags
        for (const auto& eff : layer.effects) {
            if (!eff.visible) continue;

            switch (eff.type) {
                case EffectType::Breath: {
                    float breathPhase = time * eff.speed * 2.0f;
                    animScale *= (1.0f + 0.02f * std::sin(breathPhase));
                    animOffsetY += std::sin(breathPhase) * 4.0f;
                    break;
                }
                case EffectType::Pulse: {
                    // Live audio modulates the pulse when capture is active;
                    // deterministic time-based pulse (batch-safe).
                    const float band = m_audioVisualizer.isLive()
                        ? m_audioVisualizer.getBand(0)
                        : 0.0f;
                    const float pulsePhase = time * eff.speed * 3.0f;
                    animScale *= (1.0f + eff.strength * std::sin(pulsePhase)
                                         + eff.strength * 2.0f * band);
                    break;
                }
                case EffectType::Wind:
                case EffectType::WaterWaves:
                case EffectType::WaterRipple:
                case EffectType::FoliageSway: {
                    hasMeshDeform = true;
                    deformSpeed = eff.speed;
                    deformStrength = eff.strength;
                    deformDirection = eff.direction;
                    break;
                }
                case EffectType::Shake: {
                    float shakePhase = time * eff.speed * 10.0f;
                    animOffsetX += std::sin(shakePhase * 1.3f) * eff.strength * 5.0f;
                    animOffsetY += std::cos(shakePhase * 1.7f) * eff.strength * 5.0f;
                    break;
                }
                case EffectType::Blur:
                    // Blur handled post-composite via RenderGraph/Vulkan; no per-layer deform
                    break;
                case EffectType::FilmGrain:
                case EffectType::ColorAdjust:
                case EffectType::Tint:
                case EffectType::Unknown:
                default:
                    break;
            }
        }

        painter.translate(finalX + animOffsetX, finalY + animOffsetY);
        // Apply accumulated rotation from parent chain
        if (acc.angle != 0.0f) painter.rotate(acc.angle);
        if (animRotation != 0.0f) painter.rotate(animRotation);
        if (animScale != 1.0f) painter.scale(animScale, animScale);

        if (!layer.bones.empty()) {
            // Puppet-warp path: bones parsed from model json, deformed via
            // CPU skinning stub (identity transform in rest pose).
            std::vector<Render::MeshVertex> verts;
            std::vector<Render::MeshTriangle> indices;
            Render::MeshDeformer::generateDefaultGrid(8, 8, spriteW, spriteH, verts, indices);

            // Translate SceneLayer::Bone into render-side DeformBone.
            // Animated state left at identity until bone animation is decoded
            // from scene.json/puppet wiggle data.
            std::vector<Render::DeformBone> deformBones;
            deformBones.reserve(layer.bones.size());
            for (const auto& bone : layer.bones) {
                Render::DeformBone db;
                db.name = bone.name;
                db.parent = bone.parent;
                db.pos = bone.pos;
                db.angle = bone.angle;
                db.weight = bone.weight;
                deformBones.push_back(std::move(db));
            }

            Render::MeshDeformer::boneWeightedDeform(verts, deformBones, spriteW, spriteH);
            Render::MeshDeformer::renderDeformedMesh(painter, layer.image, verts, indices, finalX, finalY, spriteW, spriteH);
        } else if (hasMeshDeform) {
            // Apply Mesh Deformation Grid
            std::vector<Render::MeshVertex> verts;
            std::vector<Render::MeshTriangle> indices;
            Render::MeshDeformer::generateDefaultGrid(8, 8, spriteW, spriteH, verts, indices);
            Render::MeshDeformer::deformVertices(verts, time, deformSpeed, deformStrength, deformDirection);
            Render::MeshDeformer::renderDeformedMesh(painter, layer.image, verts, indices, finalX, finalY, spriteW, spriteH);
        } else {
            // Always center image on translated origin; scene-space coords already handled
            painter.drawImage(QRectF(-spriteW / 2.0f, -spriteH / 2.0f, spriteW, spriteH), layer.image);
        }

        // Render GodRays / LightShafts overlay
        for (const auto& eff : layer.effects) {
            if (eff.visible && (eff.type == EffectType::GodRays || eff.type == EffectType::Shine)) {
                painter.save();
                painter.setCompositionMode(QPainter::CompositionMode_Plus);

                float rayPulse = 0.8f + 0.2f * std::sin(time * eff.speed * 1.5f);
                float rayRadius = std::max(spriteW, spriteH) * eff.length * rayPulse;

                QRadialGradient rayGrad(0, 0, rayRadius);
                QColor centerCol = eff.color;
                centerCol.setAlphaF(std::clamp(eff.intensity * 0.4f * rayPulse, 0.0f, 1.0f));
                QColor edgeCol = centerCol;
                edgeCol.setAlphaF(0.0f);

                rayGrad.setColorAt(0.0f, centerCol);
                rayGrad.setColorAt(0.5f, QColor(centerCol.red(), centerCol.green(), centerCol.blue(), static_cast<int>(centerCol.alpha() * 0.4f)));
                rayGrad.setColorAt(1.0f, edgeCol);

                painter.setBrush(rayGrad);
                painter.setPen(Qt::NoPen);
                painter.drawEllipse(QPointF(0, 0), rayRadius, rayRadius);
                painter.restore();
            }
        }

        painter.restore();
    }

    // 3. Render Particle Systems
    m_particleEngine.update(dt, m_width, m_height);
    m_particleEngine.render(painter, m_width, m_height);

    painter.end();

    // 3b. Scene-level post-processing: film grain over the composite frame.
    // GPU when compute is available, CPU fallback otherwise. Grain timing is
    // quantized to whole frames so each rendered frame stays deterministic.
    if (m_hasFilmGrain && m_grainPower > 0.0f) {
        const float grainFrame = std::floor(time);
        bool grained = false;
        if (m_hasCompute) {
            grained = m_compute.applyFilmGrain(m_canvas, m_grainPower, m_grainScale, grainFrame);
        }
        if (!grained) {
            const int w = m_canvas.width();
            const int h = m_canvas.height();
            const float scale = std::max(m_grainScale, 0.001f);
            // Same interleaved gradient noise as the GPU pass (byte-wise so
            // Format_RGBA8888 channel order is preserved).
            for (int y = 0; y < h; ++y) {
                auto* line = m_canvas.scanLine(y);
                for (int x = 0; x < w; ++x) {
                    const float gx = static_cast<float>(x) / scale + grainFrame * 137.0f;
                    const float gy = static_cast<float>(y) / scale + grainFrame * 137.0f;
                    const float d = gx * 0.06711056f + gy * 0.00583715f;
                    const float fd = d - std::floor(d);
                    float noise = fd + 52.9829189f * fd;
                    noise -= std::floor(noise);
                    const float g = (noise - 0.5f) * m_grainPower * 255.0f;
                    uint8_t* px = line + static_cast<ptrdiff_t>(x) * 4;
                    px[0] = static_cast<uint8_t>(std::clamp(static_cast<int>(px[0]) + static_cast<int>(g), 0, 255));
                    px[1] = static_cast<uint8_t>(std::clamp(static_cast<int>(px[1]) + static_cast<int>(g), 0, 255));
                    px[2] = static_cast<uint8_t>(std::clamp(static_cast<int>(px[2]) + static_cast<int>(g), 0, 255));
                }
            }
        }
    }

    // 4. Upload composite frame to Vulkan Context
    m_vulkanCtx->uploadSceneImage(m_width, m_height,
        std::span<const uint8_t>(m_canvas.constBits(), m_canvas.sizeInBytes()));
}

} // namespace WallpaperEngine::Scene
