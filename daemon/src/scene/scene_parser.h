#pragma once

#include <string>
#include <vector>
#include <memory>
#include <QVector3D>
#include <QVector2D>
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include "../assets/pkg_reader.h"
#include "../assets/tex_parser.h"
#include "../assets/video_decoder.h"

namespace WallpaperEngine::Scene {

enum class BlendMode {
    Translucent,
    Additive,
    Multiply,
    Screen,
    ColorDodge,
    Overlay,
    SoftLight,
    Opaque
};

enum class EffectType {
    None,
    WaterWaves,
    WaterRipple,
    Flow,
    Wind,
    Breath,
    Pulse,
    Scroll,
    GodRays,
    Shine,
    Tint,
    Shake,
    OpacityMask,
    FoliageSway,
    FilmGrain,
    Blur,
    ColorAdjust,
    Unknown
};

struct LayerEffect {
    std::string name;
    std::string file;
    EffectType type = EffectType::None;
    bool visible = true;

    float speed = 1.0f;
    float strength = 0.05f;
    float direction = 0.0f;
    float scale = 30.0f;
    float exponent = 1.0f;

    QVector2D center{0.5f, 0.5f};
    QVector2D scrollSpeed{0.0f, 0.0f};
    QColor color{255, 255, 255, 255};
    float intensity = 1.0f;
    float length = 0.5f;

    std::string maskPath;
    QImage maskImage;
};

struct SceneLayer {
    int id = -1;            // Unique integer ID from scene.json "id" field
    int parentId = -1;      // Parent layer ID (-1 = root scene canvas)
    std::string name;
    std::string type;
    std::string attachpoint; // For puppet warp / bone hierarchy
    bool visible = true;
    float opacity = 1.0f;
    int zOrder = 0;

    QVector3D origin{0.0f, 0.0f, 0.0f};
    QVector3D scale{1.0f, 1.0f, 1.0f};
    QVector3D angles{0.0f, 0.0f, 0.0f}; // z = rotation in degrees
    QVector2D size{0.0f, 0.0f};
    QVector2D cropOffset{0.0f, 0.0f};
    bool hasCropOffset = false;
    QVector3D parallaxDepth{0.0f, 0.0f, 0.0f};

    std::string modelPath;
    std::string materialPath;
    std::string texturePath;
    std::string particlePath;
    BlendMode blending = BlendMode::Translucent;

    bool isParticle = false;
    bool isText = false;
    bool isInteractive = false;
    bool isClock = false;
    bool isDate = false;
    bool isCompositionLayer = false;
    std::string textContent;
    std::string textFormat;
    QColor textColor{255, 255, 255, 255};
    float fontSize = 24.0f;

    std::vector<LayerEffect> effects;
    QImage image; // Decoded RGBA image for this layer
    std::shared_ptr<Assets::VideoDecoder> videoDecoder; // Video decoder for MP4 textures

    // Hierarchy support (resolved after parsing)
    SceneLayer* parent = nullptr;
    std::vector<SceneLayer*> children;
};

struct ParticleEmitterConfig {
    std::string name;
    std::string texturePath;
    BlendMode blending = BlendMode::Additive;
    float rate = 5.0f;
    float lifetime = 4.0f;
    float minSpeed = 10.0f;
    float maxSpeed = 40.0f;
    QVector3D direction{0.0f, -0.5f, 0.0f};
    QVector3D gravity{0.0f, 5.0f, 0.0f};
    float minSize = 2.0f;
    float maxSize = 6.0f;
    QColor color{255, 240, 200, 180};
    QImage particleSprite;
};

struct SceneDescription {
    std::string title;
    QColor clearColor{0, 0, 0, 255};
    float sceneWidth = 3840.0f;
    float sceneHeight = 2160.0f;
    int totalVisualObjectsDeclared = 0;
    int totalParticleEmittersDeclared = 0;
    std::vector<SceneLayer> layers;
    std::vector<ParticleEmitterConfig> emitters;
    std::string soundPath;
};

class SceneParser {
public:
    static bool parseScene(Assets::PkgReader& pkgReader, SceneDescription& outScene);

private:
    static bool resolveMaterial(Assets::PkgReader& pkgReader, const std::string& matPath, SceneLayer& layer);
    static bool resolveModel(Assets::PkgReader& pkgReader, const std::string& modelPath, SceneLayer& layer);
    static bool resolveTexture(Assets::PkgReader& pkgReader, const std::string& texName, QImage& outImage);
    // Overload that also returns a VideoDecoder for MP4 textures
    static bool resolveTexture(Assets::PkgReader& pkgReader, const std::string& texName, QImage& outImage, std::shared_ptr<Assets::VideoDecoder>& outVideoDecoder);
    static bool resolveParticle(Assets::PkgReader& pkgReader, const std::string& particlePath, ParticleEmitterConfig& outEmitter);
    static bool resolveEffect(Assets::PkgReader& pkgReader, const QJsonObject& effObj, LayerEffect& outEffect);
    static void generateLiveTextImage(SceneLayer& layer);
};

} // namespace WallpaperEngine::Scene
