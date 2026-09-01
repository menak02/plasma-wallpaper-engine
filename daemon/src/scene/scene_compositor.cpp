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
    return m_compute.getPipeline("blur") != nullptr;
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
    m_hasScene = true;
    std::cout << "SceneCompositor: Loaded scene '" << m_scene.title 
              << "' with " << m_scene.layers.size() << " layers" << std::endl;
    return true;
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
void SceneCompositor::setWebProperty(const QString& key, const QVariant& value) {
    if (m_web) m_web->setProperty(key, value);
}

void SceneCompositor::updateAndRender(float dt, float time) {
    if (!m_hasScene || !m_vulkanCtx) return;

    // Web: refresh from QWebEngineView each frame (throttled)
    if (m_isWeb && m_web) {
        QImage webImg = m_web->grabImage();
        if (!webImg.isNull() && !m_scene.layers.empty()) {
            m_scene.layers[0].image = std::move(webImg);
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

        // scene-space (center-based, Y-UP) → screen-space (top-left-based, Y-DOWN)
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
                    float pulsePhase = time * eff.speed * 3.0f;
                    animScale *= (1.0f + eff.strength * std::sin(pulsePhase));
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

        if (hasMeshDeform) {
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

    // 4. Upload composite frame to Vulkan Context
    m_vulkanCtx->uploadSceneImage(m_width, m_height,
        std::span<const uint8_t>(m_canvas.constBits(), m_canvas.sizeInBytes()));
}

} // namespace WallpaperEngine::Scene
