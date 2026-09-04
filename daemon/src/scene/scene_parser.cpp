#include "scene_parser.h"
#include "js_engine.h"
#include "../assets/dxt_decoder.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QPainter>
#include <QDateTime>
#include <QDebug>
#include <QProcess>
#include <QTemporaryFile>
#include <QDir>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <QCache>

namespace WallpaperEngine::Scene {

static QCache<QString, Assets::TexImage> g_texCache(64);

static float resolveUserFloat(const QJsonValue& val, float defaultVal = 0.0f) {
    if (val.isDouble()) return static_cast<float>(val.toDouble());
    if (val.isObject()) {
        return static_cast<float>(val.toObject().value(QStringLiteral("value")).toDouble(defaultVal));
    }
    return defaultVal;
}

static bool resolveUserBool(const QJsonValue& val, bool defaultVal = true) {
    if (val.isBool()) return val.toBool();
    if (val.isObject()) {
        QJsonObject obj = val.toObject();
        if (obj.contains(QStringLiteral("value"))) {
            return resolveUserBool(obj.value(QStringLiteral("value")), defaultVal);
        }
        if (obj.contains(QStringLiteral("user"))) {
            return resolveUserBool(obj.value(QStringLiteral("user")), defaultVal);
        }
    }
    return defaultVal;
}

static BlendMode parseBlendMode(const QString& str) {
    QString s = str.toLower();
    if (s == QStringLiteral("additive") || s == QStringLiteral("linear_dodge")) return BlendMode::Additive;
    if (s == QStringLiteral("multiply")) return BlendMode::Multiply;
    if (s == QStringLiteral("screen") || s == QStringLiteral("lighten")) return BlendMode::Screen;
    if (s == QStringLiteral("colordodge") || s == QStringLiteral("color_dodge")) return BlendMode::ColorDodge;
    if (s == QStringLiteral("overlay")) return BlendMode::Overlay;
    if (s == QStringLiteral("softlight") || s == QStringLiteral("soft_light")) return BlendMode::SoftLight;
    if (s == QStringLiteral("opaque")) return BlendMode::Opaque;
    return BlendMode::Translucent;
}

static QVector3D parseVector3D(const QJsonValue& val, const QVector3D& defaultVal = QVector3D()) {
    if (val.isObject()) {
        QJsonObject obj = val.toObject();
        if (obj.contains(QStringLiteral("value"))) {
            return parseVector3D(obj.value(QStringLiteral("value")), defaultVal);
        }
    }
    if (val.isArray()) {
        QJsonArray arr = val.toArray();
        return QVector3D(
            static_cast<float>(arr.at(0).toDouble(defaultVal.x())),
            static_cast<float>(arr.at(1).toDouble(defaultVal.y())),
            static_cast<float>(arr.at(2).toDouble(defaultVal.z()))
        );
    } else if (val.isString()) {
        QStringList parts = val.toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 3) {
            return QVector3D(parts[0].toFloat(), parts[1].toFloat(), parts[2].toFloat());
        }
    }
    return defaultVal;
}

static QVector2D parseVector2D(const QJsonValue& val, const QVector2D& defaultVal = QVector2D()) {
    if (val.isObject()) {
        QJsonObject obj = val.toObject();
        if (obj.contains(QStringLiteral("value"))) {
            return parseVector2D(obj.value(QStringLiteral("value")), defaultVal);
        }
    }
    if (val.isArray()) {
        QJsonArray arr = val.toArray();
        return QVector2D(
            static_cast<float>(arr.at(0).toDouble(defaultVal.x())),
            static_cast<float>(arr.at(1).toDouble(defaultVal.y()))
        );
    } else if (val.isString()) {
        QStringList parts = val.toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            return QVector2D(parts[0].toFloat(), parts[1].toFloat());
        }
    }
    return defaultVal;
}

void SceneParser::generateLiveTextImage(SceneLayer& layer) {
    int tw = layer.size.x() > 0 ? static_cast<int>(layer.size.x()) : 400;
    int th = layer.size.y() > 0 ? static_cast<int>(layer.size.y()) : 120;
    if (tw < 100) tw = 400;
    if (th < 50) th = 120;

    QImage txtImg(tw, th, QImage::Format_RGBA8888);
    txtImg.fill(Qt::transparent);
    QPainter txtPainter(&txtImg);
    txtPainter.setRenderHint(QPainter::Antialiasing, true);
    txtPainter.setRenderHint(QPainter::TextAntialiasing, true);

    QString textToRender;
    QDateTime now = QDateTime::currentDateTime();

    if (layer.isClock) {
        textToRender = now.toString(QStringLiteral("hh:mm"));
    } else if (layer.isDate) {
        textToRender = now.toString(QStringLiteral("dddd, MMMM d"));
    } else if (!layer.textContent.empty()) {
        textToRender = QString::fromStdString(layer.textContent);
    } else {
        textToRender = QString::fromStdString(layer.name);
    }

    QFont f = txtPainter.font();
    f.setPointSizeF(std::max(layer.fontSize, 18.0f));
    f.setBold(true);
    txtPainter.setFont(f);

    // Drop shadow
    txtPainter.setPen(QColor(0, 0, 0, 180));
    txtPainter.drawText(txtImg.rect().translated(2, 2), Qt::AlignCenter, textToRender);

    // Foreground text
    txtPainter.setPen(layer.textColor);
    txtPainter.drawText(txtImg.rect(), Qt::AlignCenter, textToRender);
    txtPainter.end();

    layer.image = txtImg;
}

static void applyOpacityMasks(SceneLayer& layer) {
    if (layer.image.isNull()) return;

    for (const auto& eff : layer.effects) {
        if (eff.type != EffectType::OpacityMask) continue;
        if (eff.maskImage.isNull()) continue;

        // Scale mask to match layer image size
        QImage mask = eff.maskImage.scaled(layer.image.width(), layer.image.height(),
                                           Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        mask = mask.convertToFormat(QImage::Format_ARGB32);

        int w = layer.image.width();
        int h = layer.image.height();

        // Ensure layer image is ARGB32
        layer.image = layer.image.convertToFormat(QImage::Format_ARGB32);

        // Apply mask: multiply layer alpha by mask grayscale value
        for (int y = 0; y < h; ++y) {
            const QRgb* maskLine = reinterpret_cast<const QRgb*>(mask.constScanLine(y));
            QRgb* layerLine = reinterpret_cast<QRgb*>(layer.image.scanLine(y));
            for (int x = 0; x < w; ++x) {
                QRgb m = maskLine[x];
                QRgb l = layerLine[x];
                // Use mask red channel as grayscale alpha multiplier
                int maskAlpha = qRed(m);
                int newAlpha = (qAlpha(l) * maskAlpha) / 255;
                layerLine[x] = qRgba(qRed(l), qGreen(l), qBlue(l), newAlpha);
            }
        }
    }
}

bool SceneParser::parseScene(Assets::PkgReader& pkgReader, SceneDescription& outScene) {
    return parseScene(pkgReader, outScene, {});
}
bool SceneParser::parseScene(Assets::PkgReader& pkgReader, SceneDescription& outScene, const std::unordered_map<std::string, QVariant>& overrideProperties) {
    std::string sceneJsonStr = pkgReader.readTextFile("scene.json");
    if (sceneJsonStr.empty()) {
        return false;
    }

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(sceneJsonStr));
    if (!doc.isObject()) {
        return false;
    }

    QJsonObject root = doc.object();
    QJsonObject general = root.value(QStringLiteral("general")).toObject();
    outScene.title = general.value(QStringLiteral("title")).toString().toStdString();

    QJsonObject ortho = general.value(QStringLiteral("orthogonalprojection")).toObject();
    if (!ortho.isEmpty()) {
        outScene.sceneWidth = static_cast<float>(ortho.value(QStringLiteral("width")).toDouble(3840.0));
        outScene.sceneHeight = static_cast<float>(ortho.value(QStringLiteral("height")).toDouble(2160.0));
    }

    if (general.contains(QStringLiteral("clearcolor"))) {
        QVector3D cc = parseVector3D(general.value(QStringLiteral("clearcolor")), QVector3D(0.0f, 0.0f, 0.0f));
        outScene.clearColor = QColor::fromRgbF(qBound(0.0f, cc.x(), 1.0f), qBound(0.0f, cc.y(), 1.0f), qBound(0.0f, cc.z(), 1.0f));
    }

    // Load project.json properties for JS engine
    std::unordered_map<std::string, QVariant> propertiesMap;
    std::string projectJsonStr = pkgReader.readTextFile("project.json");
    if (!projectJsonStr.empty()) {
        QJsonDocument projDoc = QJsonDocument::fromJson(QByteArray::fromStdString(projectJsonStr));
        if (projDoc.isObject()) {
            QJsonObject projRoot = projDoc.object();
            QJsonObject properties = projRoot.value(QStringLiteral("properties")).toObject();
            for (auto it = properties.begin(); it != properties.end(); ++it) {
                propertiesMap[it.key().toStdString()] = it->toVariant();
            }
        }
    }
    // Apply overrides from WallpaperService live properties
    for (auto& [k,v] : overrideProperties) propertiesMap[k] = v;

    JSEngine jsEngine;
    jsEngine.init(propertiesMap);
    jsEngine.update(0.0f, 0.0f);

    // Sound OST detection
    auto allFiles = pkgReader.listFiles();
    for (const auto& file : allFiles) {
        if (file.ends_with(".mp3") || file.ends_with(".ogg") || file.ends_with(".wav") || file.ends_with(".flac")) {
            outScene.soundPath = file;
            break;
        }
    }

    QJsonArray objects = root.value(QStringLiteral("objects")).toArray();
    int zOrder = 0;
    outScene.totalVisualObjectsDeclared = 0;
    outScene.totalParticleEmittersDeclared = 0;

    for (const auto& objVal : objects) {
        if (!objVal.isObject()) continue;
        QJsonObject obj = objVal.toObject();

        QString imageField = obj.value(QStringLiteral("image")).toString();
        QString modelField = obj.value(QStringLiteral("model")).toString();
        if (obj.contains(QStringLiteral("instance"))) {
            QJsonObject instObj = obj.value(QStringLiteral("instance")).toObject();
            QJsonArray instTexs = instObj.value(QStringLiteral("textures")).toArray();
            if (!instTexs.isEmpty()) {
                QString tStr = instTexs.at(0).toString();
                if (!tStr.isEmpty()) {
                    if (!tStr.startsWith(QStringLiteral("materials/")) && !tStr.startsWith(QStringLiteral("textures/"))) {
                        tStr = QStringLiteral("materials/") + tStr;
                    }
                    if (!tStr.endsWith(QStringLiteral(".tex")) && !tStr.endsWith(QStringLiteral(".png"))) {
                        tStr += QStringLiteral(".tex");
                    }
                    imageField = tStr;
                }
            }
        }
        QString particleField = obj.value(QStringLiteral("particle")).toString();
        QString presetField = obj.value(QStringLiteral("preset")).toString();
        QJsonObject textObj = obj.value(QStringLiteral("text")).toObject();

        // 1. Particle System Object
        if (!particleField.isEmpty() || !presetField.isEmpty()) {
            outScene.totalParticleEmittersDeclared++;
            std::string pPath = !particleField.isEmpty() ? particleField.toStdString() : presetField.toStdString();
            ParticleEmitterConfig emitter;
            if (resolveParticle(pkgReader, pPath, emitter)) {
                outScene.emitters.push_back(std::move(emitter));
            }
            continue;
        }

        // Check if object is a visual layer
        bool isVisual = (!imageField.isEmpty() || !modelField.isEmpty() || !textObj.isEmpty());
        if (!isVisual) {
            continue;
        }

        SceneLayer layer;
        // Parse unique integer ID and parent ID for hierarchy
        layer.id = obj.value(QStringLiteral("id")).toInt(-1);
        layer.parentId = obj.value(QStringLiteral("parent")).toInt(-1);
        layer.attachpoint = obj.value(QStringLiteral("attachpoint")).toString().toStdString();
        
        if (obj.contains(QStringLiteral("sort"))) {
            layer.zOrder = obj.value(QStringLiteral("sort")).toInt(zOrder++);
        } else {
            layer.zOrder = zOrder++;
        }
        layer.name = obj.value(QStringLiteral("name")).toString().toStdString();
        // Use JSEngine for visibility evaluation (handles script/user properties)
        layer.visible = jsEngine.evaluateVisibility(obj.value(QStringLiteral("visible")));
        layer.opacity = resolveUserFloat(obj.value(QStringLiteral("opacity")), 1.0f);

        layer.origin = parseVector3D(obj.value(QStringLiteral("origin")), QVector3D(outScene.sceneWidth / 2.0f, outScene.sceneHeight / 2.0f, 0.0f));
        layer.scale = parseVector3D(obj.value(QStringLiteral("scale")), QVector3D(1.0f, 1.0f, 1.0f));
        layer.angles = parseVector3D(obj.value(QStringLiteral("angles")), QVector3D(0.0f, 0.0f, 0.0f));
        layer.size = parseVector2D(obj.value(QStringLiteral("size")), QVector2D(0.0f, 0.0f));
        layer.parallaxDepth = parseVector3D(obj.value(QStringLiteral("parallaxDepth")), QVector3D(0.0f, 0.0f, 0.0f));

        QString nameLower = QString::fromStdString(layer.name).toLower();
        if (nameLower.contains(QStringLiteral("clock")) || nameLower.contains(QStringLiteral("time"))) {
            layer.isClock = true;
        } else if (nameLower.contains(QStringLiteral("date")) || nameLower.contains(QStringLiteral("day"))) {
            layer.isDate = true;
        }
        if (nameLower.contains(QStringLiteral("hover")) || nameLower.contains(QStringLiteral("jill")) || nameLower.contains(QStringLiteral("zombie")) || nameLower.contains(QStringLiteral("switch"))) {
            layer.isInteractive = true;
        }

        // 2. Text / Widget Layer
        if (!textObj.isEmpty() || layer.isClock || layer.isDate) {
            layer.isText = true;
            layer.textContent = textObj.value(QStringLiteral("value")).toString().toStdString();
            layer.fontSize = static_cast<float>(obj.value(QStringLiteral("pointsize")).toDouble(24.0));
            generateLiveTextImage(layer);
            outScene.totalVisualObjectsDeclared++;
            outScene.layers.push_back(std::move(layer));
            continue;
        }

        // 3. Image / Model Layer
        std::string targetModel = !imageField.isEmpty() ? imageField.toStdString() : modelField.toStdString();
        if (!targetModel.empty()) {
            if (targetModel.find("util/white") != std::string::npos) {
                QImage sysImg(64, 64, QImage::Format_RGBA8888);
                sysImg.fill(Qt::white);
                layer.image = sysImg;
            } else if (targetModel.find("solidlayer") != std::string::npos) {
                QImage sysImg(64, 64, QImage::Format_RGBA8888);
                QVector3D c = obj.contains(QStringLiteral("color")) ? parseVector3D(obj.value(QStringLiteral("color"))) : QVector3D(0, 0, 0);
                float r = qBound(0.0f, c.x(), 1.0f);
                float g = qBound(0.0f, c.y(), 1.0f);
                float b = qBound(0.0f, c.z(), 1.0f);
                float alphaVal = resolveUserFloat(obj.value(QStringLiteral("alpha")), 1.0f);
                sysImg.fill(QColor::fromRgbF(r, g, b, alphaVal));
                layer.image = sysImg;

                if (r < 0.01f && g < 0.01f && b < 0.01f) {
                    QString n = QString::fromStdString(layer.name).toLower();
                    if (n.contains(QStringLiteral("bloqueo")) || n.contains(QStringLiteral("boundary")) || 
                        n.contains(QStringLiteral("collision")) ||
                        n.contains(QStringLiteral("clouds")) || n.contains(QStringLiteral("mask")) ||
                        n.contains(QStringLiteral("drag")) || n.contains(QStringLiteral("interactive")) ||
                        n.contains(QStringLiteral("bar"))) {
                        layer.visible = false;
                    }
                }
            } else if (targetModel.find("composelayer") != std::string::npos || 
                targetModel.find("projectlayer") != std::string::npos ||
                targetModel.find("fullscreenlayer") != std::string::npos ||
                targetModel.find("solid_instance_model") != std::string::npos) {
                if (layer.image.isNull()) {
                    layer.isCompositionLayer = true;
                    QImage sysImg(64, 64, QImage::Format_RGBA8888);
                    sysImg.fill(QColor(0, 0, 0, 0));
                    layer.image = sysImg;
                }
            } else if (targetModel.ends_with(".json")) {
                resolveModel(pkgReader, targetModel, layer);
            } else if (targetModel.ends_with(".tex")) {
                resolveTexture(pkgReader, targetModel, layer.image);
            }
        }

        // 4. Parse Layer Effects Chain
        QJsonArray effectsArr = obj.value(QStringLiteral("effects")).toArray();
        for (const auto& effVal : effectsArr) {
            if (effVal.isObject()) {
                LayerEffect eff;
                if (resolveEffect(pkgReader, effVal.toObject(), eff)) {
                    layer.effects.push_back(std::move(eff));
                }
            }
        }

        // 5. Apply opacity masks to layer image (before compositor)
        applyOpacityMasks(layer);

        QString layerNameLower = QString::fromStdString(layer.name).toLower();
        if (layerNameLower.contains(QStringLiteral("bloqueo")) || layerNameLower.contains(QStringLiteral("boundary")) || layerNameLower.contains(QStringLiteral("collision"))) {
            layer.visible = false;
        }

        if (!layer.image.isNull() && !layer.isCompositionLayer) {
            outScene.totalVisualObjectsDeclared++;
            outScene.layers.push_back(std::move(layer));
        } else if (!layer.image.isNull()) {
            outScene.layers.push_back(std::move(layer));
        } else if (!layer.isText) {
            std::cout << "SceneParser: Failed to load image for visual layer '" << layer.name 
                      << "' with targetModel '" << targetModel << "'" << std::endl;
        }
    }

    // Build id -> layer lookup map for hierarchy resolution
    std::unordered_map<int, SceneLayer*> idToLayer;
    idToLayer.reserve(outScene.layers.size());
    for (auto& layer : outScene.layers) {
        if (layer.id >= 0) {
            idToLayer[layer.id] = &layer;
        }
    }

    // Resolve parent-child relationships
    for (auto& layer : outScene.layers) {
        if (layer.parentId >= 0) {
            auto it = idToLayer.find(layer.parentId);
            if (it != idToLayer.end()) {
                layer.parent = it->second;
                it->second->children.push_back(&layer);
            } else {
                std::cout << "SceneParser: Layer '" << layer.name << "' references non-existent parent ID " << layer.parentId << std::endl;
            }
        }
    }

    // Topological sort: parents before children, then by zOrder
    // This ensures composition layers render before their children
    std::vector<SceneLayer*> sortedLayers;
    sortedLayers.reserve(outScene.layers.size());
    std::unordered_set<int> visited;

    // Lambda for recursive topological sort — parents before children
    std::function<void(SceneLayer&)> addToSorted = [&](SceneLayer& layer) {
        if (visited.count(layer.id)) return;
        visited.insert(layer.id);
        sortedLayers.push_back(&layer);
        // Sort children by zOrder then recurse depth-first
        std::stable_sort(layer.children.begin(), layer.children.end(),
            [](SceneLayer* a, SceneLayer* b){ return a->zOrder < b->zOrder; });
        for (auto* child : layer.children) {
            addToSorted(*child);
        }
    };

    // Process root layers first (no parent), sorted by zOrder
    std::vector<SceneLayer*> roots;
    for (auto& layer : outScene.layers) {
        if (layer.parent == nullptr) {
            roots.push_back(&layer);
        }
    }
    std::stable_sort(roots.begin(), roots.end(), [](const SceneLayer* a, const SceneLayer* b) {
        return a->zOrder < b->zOrder;
    });
    for (auto* root : roots) {
        addToSorted(*root);
    }

    // Add any orphaned layers (referenced parent not found)
    for (auto& layer : outScene.layers) {
        if (!visited.count(layer.id)) {
            sortedLayers.push_back(&layer);
        }
    }

    // Rebuild layers in topological order
    std::vector<SceneLayer> sorted;
    sorted.reserve(sortedLayers.size());
    for (auto* l : sortedLayers) {
        sorted.push_back(std::move(*l));
    }
    outScene.layers = std::move(sorted);

    // Rebuild parent pointers (layers moved, pointers invalid)
    idToLayer.clear();
    for (auto& layer : outScene.layers) {
        if (layer.id >= 0) {
            idToLayer[layer.id] = &layer;
        }
    }
    for (auto& layer : outScene.layers) {
        if (layer.parentId >= 0) {
            auto it = idToLayer.find(layer.parentId);
            if (it != idToLayer.end()) {
                layer.parent = it->second;
            }
        }
        // Rebuild children pointers
        layer.children.clear();
    }
    for (auto& layer : outScene.layers) {
        if (layer.parent != nullptr) {
            layer.parent->children.push_back(&layer);
        }
    }

    // Apply visibility inheritance: hidden parent hides all descendants
    std::function<void(SceneLayer&)> propagateVisibility = [&](SceneLayer& layer) {
        if (!layer.visible) {
            for (auto* child : layer.children) {
                child->visible = false;
                propagateVisibility(*child);
            }
        }
    };
    for (auto& layer : outScene.layers) {
        if (layer.parent == nullptr) {
            propagateVisibility(layer);
        }
    }

    std::cout << "SceneParser: Parsed " << outScene.layers.size() << "/" << outScene.totalVisualObjectsDeclared 
              << " visual layers and " << outScene.emitters.size() << "/" << outScene.totalParticleEmittersDeclared 
              << " particle emitters" << std::endl;
    return true;
}

bool SceneParser::resolveEffect(Assets::PkgReader& pkgReader, const QJsonObject& effObj, LayerEffect& outEffect) {
    outEffect.file = effObj.value(QStringLiteral("file")).toString().toStdString();
    outEffect.name = effObj.value(QStringLiteral("name")).toString().toStdString();
    outEffect.visible = resolveUserBool(effObj.value(QStringLiteral("visible")), true);

    std::string fileLower = outEffect.file;
    std::transform(fileLower.begin(), fileLower.end(), fileLower.begin(), ::tolower);
    std::string nameLower = outEffect.name;
    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

    if (fileLower.find("waterwaves") != std::string::npos || fileLower.find("waterripple") != std::string::npos || nameLower.find("water") != std::string::npos) {
        outEffect.type = EffectType::WaterWaves;
    } else if (fileLower.find("wind") != std::string::npos || nameLower.find("wind") != std::string::npos || nameLower.find("cape") != std::string::npos) {
        outEffect.type = EffectType::Wind;
    } else if (fileLower.find("breath") != std::string::npos || nameLower.find("breath") != std::string::npos) {
        outEffect.type = EffectType::Breath;
    } else if (fileLower.find("pulse") != std::string::npos || nameLower.find("pulse") != std::string::npos) {
        outEffect.type = EffectType::Pulse;
    } else if (fileLower.find("scroll") != std::string::npos) {
        outEffect.type = EffectType::Scroll;
    } else if (fileLower.find("godrays") != std::string::npos || fileLower.find("lightshafts") != std::string::npos) {
        outEffect.type = EffectType::GodRays;
    } else if (fileLower.find("shine") != std::string::npos) {
        outEffect.type = EffectType::Shine;
    } else if (fileLower.find("opacity") != std::string::npos || nameLower.find("opacity") != std::string::npos) {
        outEffect.type = EffectType::OpacityMask;
    } else if (fileLower.find("tint") != std::string::npos || fileLower.find("blend") != std::string::npos) {
        outEffect.type = EffectType::Tint;
    } else if (fileLower.find("shake") != std::string::npos) {
        outEffect.type = EffectType::Shake;
    } else if (fileLower.find("foliage") != std::string::npos || fileLower.find("sway") != std::string::npos) {
        outEffect.type = EffectType::FoliageSway;
    } else if (fileLower.find("filmgrain") != std::string::npos || fileLower.find("grain") != std::string::npos || fileLower.find("noise") != std::string::npos) {
        outEffect.type = EffectType::FilmGrain;
    } else if (fileLower.find("blur") != std::string::npos) {
        outEffect.type = EffectType::Blur;
    } else if (fileLower.find("color") != std::string::npos || fileLower.find("adjust") != std::string::npos || fileLower.find("grading") != std::string::npos) {
        outEffect.type = EffectType::ColorAdjust;
    } else {
        outEffect.type = EffectType::Unknown;
    }

    QJsonArray passes = effObj.value(QStringLiteral("passes")).toArray();
    for (const auto& passVal : passes) {
        if (!passVal.isObject()) continue;
        QJsonObject passObj = passVal.toObject();
        QJsonObject consts = passObj.value(QStringLiteral("constantshadervalues")).toObject();

        if (consts.contains(QStringLiteral("speed"))) {
            outEffect.speed = resolveUserFloat(consts.value(QStringLiteral("speed")), outEffect.speed);
        }
        if (consts.contains(QStringLiteral("strength"))) {
            outEffect.strength = resolveUserFloat(consts.value(QStringLiteral("strength")), outEffect.strength);
        }
        // Film grain passes expose grainpower/power and grainscale constants
        // (user props filmgrainpower / filmgrainscale). Power drives overlay
        // intensity; grain timing is engine-side, so the WE "time" constant
        // is intentionally not mapped to speed.
        if (consts.contains(QStringLiteral("grainpower"))) {
            outEffect.strength = resolveUserFloat(consts.value(QStringLiteral("grainpower")), outEffect.strength);
        } else if (consts.contains(QStringLiteral("power"))) {
            outEffect.strength = resolveUserFloat(consts.value(QStringLiteral("power")), outEffect.strength);
        }
        if (consts.contains(QStringLiteral("grainscale"))) {
            outEffect.scale = resolveUserFloat(consts.value(QStringLiteral("grainscale")), outEffect.scale);
        }
        if (consts.contains(QStringLiteral("scale"))) {
            outEffect.scale = resolveUserFloat(consts.value(QStringLiteral("scale")), outEffect.scale);
        }
        if (consts.contains(QStringLiteral("direction"))) {
            outEffect.direction = resolveUserFloat(consts.value(QStringLiteral("direction")), outEffect.direction);
        }
        if (consts.contains(QStringLiteral("rayintensity"))) {
            outEffect.intensity = resolveUserFloat(consts.value(QStringLiteral("rayintensity")), outEffect.intensity);
        }
        if (consts.contains(QStringLiteral("raylength"))) {
            outEffect.length = resolveUserFloat(consts.value(QStringLiteral("raylength")), outEffect.length);
        }
        if (consts.contains(QStringLiteral("center"))) {
            outEffect.center = parseVector2D(consts.value(QStringLiteral("center")), outEffect.center);
        }

        // Parse mask texture if present
        QJsonArray texArr = passObj.value(QStringLiteral("textures")).toArray();
        for (const auto& t : texArr) {
            if (t.isString()) {
                QString maskName = t.toString();
                if (!maskName.isEmpty()) {
                    std::string maskPath = maskName.toStdString();
                    if (!maskPath.ends_with(".tex")) maskPath += ".tex";
                    if (!maskPath.starts_with("materials/")) maskPath = "materials/" + maskPath;
                    outEffect.maskPath = maskPath;
                    resolveTexture(pkgReader, maskPath, outEffect.maskImage);
                }
            }
        }
    }

    return true;
}

bool SceneParser::resolveModel(Assets::PkgReader& pkgReader, const std::string& modelPath, SceneLayer& layer) {
    layer.modelPath = modelPath;
    if (modelPath.find("solidlayer") != std::string::npos ||
        modelPath.find("composelayer") != std::string::npos ||
        modelPath.find("projectlayer") != std::string::npos ||
        modelPath.find("fullscreenlayer") != std::string::npos ||
        modelPath.find("solid_instance_model") != std::string::npos) {
        QImage sysImg(64, 64, QImage::Format_RGBA8888);
        sysImg.fill(QColor(0, 0, 0, 0));
        layer.image = sysImg;
        return true;
    }
    std::string modelStr = pkgReader.readTextFile(modelPath);
    if (modelStr.empty()) {
        for (const auto& file : pkgReader.listFiles()) {
            if (file.ends_with(modelPath) || file.ends_with("/" + modelPath)) {
                modelStr = pkgReader.readTextFile(file);
                break;
            }
        }
    }
    if (modelStr.empty()) {
        return false;
    }

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(modelStr));
    if (!doc.isObject()) return false;

    QJsonObject obj = doc.object();
    if (obj.contains(QStringLiteral("cropoffset"))) {
        layer.cropOffset = parseVector2D(obj.value(QStringLiteral("cropoffset")), QVector2D(0.0f, 0.0f));
        layer.hasCropOffset = true;
    }
    // Puppet bones stub parse (Almamu puppet/bones/weights)
    if (obj.contains(QStringLiteral("bones")) || obj.contains(QStringLiteral("puppet")) || obj.contains(QStringLiteral("skeleton"))) {
        QJsonArray bonesArr = obj.value(QStringLiteral("bones")).toArray();
        if (bonesArr.isEmpty()) bonesArr = obj.value(QStringLiteral("puppet")).toObject().value(QStringLiteral("bones")).toArray();
        for (auto bVal : bonesArr) {
            if (!bVal.isObject()) continue;
            QJsonObject bObj = bVal.toObject();
            SceneLayer::Bone b;
            b.name = bObj.value(QStringLiteral("name")).toString().toStdString();
            b.parent = bObj.value(QStringLiteral("parent")).toString().toStdString();
            b.pos = parseVector3D(bObj.value(QStringLiteral("pos")), QVector3D());
            b.angle = parseVector3D(bObj.value(QStringLiteral("angle")), QVector3D());
            b.weight = static_cast<float>(bObj.value(QStringLiteral("weight")).toDouble(1.0));
            layer.bones.push_back(std::move(b));
        }
        if (!layer.bones.empty()) std::cout << "SceneParser: puppet bones " << layer.bones.size() << " for " << layer.name << std::endl;
    }

    QString matPath = obj.value(QStringLiteral("material")).toString();
    if (!matPath.isEmpty()) {
        return resolveMaterial(pkgReader, matPath.toStdString(), layer);
    }
    return false;
}

bool SceneParser::resolveMaterial(Assets::PkgReader& pkgReader, const std::string& matPath, SceneLayer& layer) {
    layer.materialPath = matPath;
    std::string matStr = pkgReader.readTextFile(matPath);
    if (matStr.empty()) {
        for (const auto& file : pkgReader.listFiles()) {
            if (file.ends_with(matPath) || file.ends_with("/" + matPath)) {
                matStr = pkgReader.readTextFile(file);
                break;
            }
        }
    }

    if (matStr.empty()) return false;

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(matStr));
    if (!doc.isObject()) return false;

    QJsonArray passes = doc.object().value(QStringLiteral("passes")).toArray();
    if (passes.isEmpty()) return false;

    QJsonObject pass0 = passes.at(0).toObject();
    layer.blending = parseBlendMode(pass0.value(QStringLiteral("blending")).toString());

    QJsonArray textures = pass0.value(QStringLiteral("textures")).toArray();
    for (const auto& texVal : textures) {
        QString texName = texVal.isString() ? texVal.toString() : texVal.toObject().value(QStringLiteral("name")).toString();
        if (texName.isEmpty()) continue;

        std::string fullTexPath = texName.toStdString();
        if (!fullTexPath.ends_with(".tex")) {
            fullTexPath += ".tex";
        }
        if (!fullTexPath.starts_with("materials/") && !fullTexPath.starts_with("textures/")) {
            fullTexPath = "materials/" + fullTexPath;
        }

        layer.texturePath = fullTexPath;
        QString shaderName = pass0.value(QStringLiteral("shader")).toString().toLower();
        std::vector<uint8_t> videoBytes;
        if (resolveTexture(pkgReader, fullTexPath, layer.image, layer.videoDecoder, videoBytes)) {
            if (!videoBytes.empty()) layer.videoData = std::move(videoBytes);
            // GenericImage/genericimage2 shaders should NOT modify the texture.
            // The old qRgba(r,g,b,r) code used the RED channel as alpha, which
            // destroyed images (made dark-red areas transparent, changed colors).
            // The texture's own alpha channel is already correct from decoding.
            return true;
        }
    }

    return false;
}

bool SceneParser::resolveTexture(Assets::PkgReader& pkgReader, const std::string& texName, QImage& outImage) {
    auto bytes = pkgReader.readFile(texName);
    if (bytes.empty()) {
        std::string nameOnly = texName;
        size_t slashPos = nameOnly.find_last_of('/');
        if (slashPos != std::string::npos) {
            nameOnly = nameOnly.substr(slashPos + 1);
        }

        for (const auto& file : pkgReader.listFiles()) {
            if (file.ends_with(texName) || file.ends_with(nameOnly) || file.ends_with("/" + nameOnly)) {
                bytes = pkgReader.readFile(file);
                if (!bytes.empty()) break;
            }
        }
    }

    if (bytes.empty()) return false;

    QString cacheKey = QString::fromStdString(texName);
    Assets::TexImage texImg;
    bool fromCache = false;
    if (auto* cached = g_texCache.object(cacheKey)) {
        texImg = *cached;
        fromCache = true;
    } else {
        if (!Assets::TexParser::parse(bytes, texImg)) return false;
        auto* copy = new Assets::TexImage(texImg);
        g_texCache.insert(cacheKey, copy, static_cast<int>(texImg.mipmaps.empty()?1:texImg.mipmaps[0].data.size()/1024));
    }
    if (true) {
        auto rgba = Assets::DxtDecoder::decodeToRgba(texImg, 0);
        if (!rgba.empty()) {
            // CRITICAL: Use mip.width for QImage stride — the DXT decoder outputs
            // at mip.width (which may be padded to 4-pixel alignment for DXT blocks),
            // but texImg.width is the unpadded image width from the TEXI header.
            // Using the wrong width causes horizontal banding/row misalignment.
            uint32_t decodedWidth = texImg.width;
            uint32_t decodedHeight = texImg.height;
            if (!texImg.mipmaps.empty()) {
                if (texImg.mipmaps[0].width > 0) decodedWidth = texImg.mipmaps[0].width;
                if (texImg.mipmaps[0].height > 0) decodedHeight = texImg.mipmaps[0].height;
            }
            if (decodedWidth == 0) decodedWidth = texImg.textureWidth;
            if (decodedHeight == 0) decodedHeight = texImg.textureHeight;

            QImage img(rgba.data(), decodedWidth, decodedHeight,
                       static_cast<qsizetype>(decodedWidth) * 4, QImage::Format_RGBA8888);
            img = img.copy();
            img = img.convertToFormat(QImage::Format_ARGB32);

            // Grayscale mask detection

            // Grayscale mask detection
            bool isGrayscaleMask = true;
            const QRgb* bits = reinterpret_cast<const QRgb*>(img.constBits());
            int numPixels = img.width() * img.height();
            if (numPixels > 0) {
                int sampleStep = std::max(1, numPixels / 1000);
                for (int k = 0; k < numPixels; k += sampleStep) {
                    QRgb p = bits[k];
                    if (qAlpha(p) < 250 || qRed(p) != qGreen(p) || qGreen(p) != qBlue(p)) {
                        isGrayscaleMask = false;
                        break;
                    }
                }
                if (isGrayscaleMask && numPixels > 100) {
                    for (int y = 0; y < img.height(); ++y) {
                        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
                        for (int x = 0; x < img.width(); ++x) {
                            QRgb p = line[x];
                            line[x] = qRgba(qRed(p), qGreen(p), qBlue(p), qRed(p));
                        }
                    }
                }
            }

            outImage = img;
            return true;
        }

        // Check for MP4 video data in mipmap
        if (!texImg.mipmaps.empty()) {
            const auto& mip = texImg.mipmaps[0];
            if (mip.data.size() >= 8) {
                const uint8_t* d = mip.data.data();
                if (d[4] == 'f' && d[5] == 't' && d[6] == 'y' && d[7] == 'p') {
                    std::cerr << "SceneParser: MP4 video texture detected: " << texName << std::endl;
                    QTemporaryFile tmpMp4(QDir::tempPath() + "/wp_XXXXXX.mp4");
                    tmpMp4.setAutoRemove(false);
                    if (tmpMp4.open()) {
                        tmpMp4.write(reinterpret_cast<const char*>(mip.data.data()), mip.data.size());
                        tmpMp4.close();
                        QTemporaryFile tmpPng(QDir::tempPath() + "/wp_XXXXXX.png");
                        tmpPng.setAutoRemove(false);
                        if (tmpPng.open()) {
                            tmpPng.close();
                            QProcess ffmpeg;
                            ffmpeg.setProcessChannelMode(QProcess::SeparateChannels);
                            ffmpeg.start("ffmpeg", {"-y", "-i", tmpMp4.fileName(),
                                                    "-vframes", "1", "-q:v", "2",
                                                    tmpPng.fileName()});
                            ffmpeg.waitForFinished(10000);
                            QImage frame(tmpPng.fileName());
                            if (!frame.isNull()) {
                                outImage = frame.convertToFormat(QImage::Format_ARGB32);
                                QFile::remove(tmpMp4.fileName());
                                QFile::remove(tmpPng.fileName());
                                return true;
                            }
                            QFile::remove(tmpPng.fileName());
                        }
                        QFile::remove(tmpMp4.fileName());
                    }
                    return false;
                }
            }
        }
    }
    return false;
}

bool SceneParser::resolveTexture(Assets::PkgReader& pkgReader, const std::string& texName, QImage& outImage, std::shared_ptr<Assets::VideoDecoder>& outVideoDecoder) {
    std::vector<uint8_t> dummy;
    return resolveTexture(pkgReader, texName, outImage, outVideoDecoder, dummy);
}
bool SceneParser::resolveTexture(Assets::PkgReader& pkgReader, const std::string& texName, QImage& outImage, std::shared_ptr<Assets::VideoDecoder>& outVideoDecoder, std::vector<uint8_t>& outVideoBytes) {
    auto bytes = pkgReader.readFile(texName);
    if (bytes.empty()) {
        std::string nameOnly = texName;
        size_t slashPos = nameOnly.find_last_of('/');
        if (slashPos != std::string::npos) {
            nameOnly = nameOnly.substr(slashPos + 1);
        }

        for (const auto& file : pkgReader.listFiles()) {
            if (file.ends_with(texName) || file.ends_with(nameOnly) || file.ends_with("/" + nameOnly)) {
                bytes = pkgReader.readFile(file);
                if (!bytes.empty()) break;
            }
        }
    }

    if (bytes.empty()) return false;

    Assets::TexImage texImg;
    if (Assets::TexParser::parse(bytes, texImg)) {
        // Check for MP4 video data FIRST
        if (!texImg.mipmaps.empty()) {
            const auto& mip = texImg.mipmaps[0];
            if (mip.data.size() >= 8) {
                const uint8_t* d = mip.data.data();
                if (d[4] == 'f' && d[5] == 't' && d[6] == 'y' && d[7] == 'p') {
                    std::cerr << "SceneParser: MP4 video texture detected: " << texName << std::endl;
                    outVideoBytes = mip.data; // keep for lazy VideoDecoder in SceneCompositor
                    // Verifier & daemon first-frame via ffmpeg CLI (stable, no multi-decoder heap issue)
                    // VideoDecoder C++ path kept for daemon tick lazy creation (see SceneCompositor)
                    // Fallback: ffmpeg CLI single-frame extract (requires ffmpeg binary)
                    QTemporaryFile tmpMp4(QDir::tempPath() + "/wp_XXXXXX.mp4");
                    tmpMp4.setAutoRemove(false);
                    if (tmpMp4.open()) {
                        tmpMp4.write(reinterpret_cast<const char*>(mip.data.data()), mip.data.size());
                        tmpMp4.close();
                        QTemporaryFile tmpPng(QDir::tempPath() + "/wp_XXXXXX.png");
                        tmpPng.setAutoRemove(false);
                        if (tmpPng.open()) {
                            tmpPng.close();
                            QProcess ffmpeg;
                            ffmpeg.setProcessChannelMode(QProcess::SeparateChannels);
                            ffmpeg.start("ffmpeg", {"-y", "-i", tmpMp4.fileName(),
                                                    "-vframes", "1", "-q:v", "2",
                                                    tmpPng.fileName()});
                            ffmpeg.waitForFinished(10000);
                            QImage frame(tmpPng.fileName());
                            if (!frame.isNull()) {
                                outImage = frame.convertToFormat(QImage::Format_ARGB32);
                                QFile::remove(tmpMp4.fileName());
                                QFile::remove(tmpPng.fileName());
                                return true;
                            }
                            QFile::remove(tmpPng.fileName());
                        }
                        QFile::remove(tmpMp4.fileName());
                    }
                    return false;
                }
            }
        }

        auto rgba = Assets::DxtDecoder::decodeToRgba(texImg, 0);
        if (!rgba.empty()) {
            uint32_t decodedWidth = texImg.width;
            uint32_t decodedHeight = texImg.height;
            if (!texImg.mipmaps.empty()) {
                if (texImg.mipmaps[0].width > 0) decodedWidth = texImg.mipmaps[0].width;
                if (texImg.mipmaps[0].height > 0) decodedHeight = texImg.mipmaps[0].height;
            }
            if (decodedWidth == 0) decodedWidth = texImg.textureWidth;
            if (decodedHeight == 0) decodedHeight = texImg.textureHeight;

            QImage img(rgba.data(), decodedWidth, decodedHeight,
                       static_cast<qsizetype>(decodedWidth) * 4, QImage::Format_RGBA8888);
            img = img.copy();
            img = img.convertToFormat(QImage::Format_ARGB32);

            outImage = img;
            return true;
        }
    }
    return false;
}

bool SceneParser::resolveParticle(Assets::PkgReader& pkgReader, const std::string& particlePath, ParticleEmitterConfig& outEmitter) {
    std::string fullPath = particlePath;
    if (!fullPath.ends_with(".json")) fullPath += ".json";
    if (!fullPath.starts_with("particles/")) fullPath = "particles/" + fullPath;

    std::string jsonStr = pkgReader.readTextFile(fullPath);
    if (jsonStr.empty()) {
        for (const auto& f : pkgReader.listFiles()) {
            if (f.ends_with(particlePath) || f.ends_with(particlePath + ".json")) {
                jsonStr = pkgReader.readTextFile(f);
                break;
            }
        }
    }

    if (jsonStr.empty()) return false;

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(jsonStr));
    if (!doc.isObject()) return false;

    QJsonObject obj = doc.object();
    outEmitter.name = particlePath;
    
    float rate = static_cast<float>(obj.value(QStringLiteral("rate")).toDouble(3.0));
    QJsonArray emitterArr = obj.value(QStringLiteral("emitter")).toArray();
    if (!emitterArr.isEmpty()) {
        QJsonObject e0 = emitterArr.at(0).toObject();
        if (e0.contains(QStringLiteral("rate"))) {
            rate = static_cast<float>(e0.value(QStringLiteral("rate")).toDouble(rate));
        }
    }
    outEmitter.rate = std::clamp(rate, 0.2f, 15.0f);
    outEmitter.lifetime = static_cast<float>(obj.value(QStringLiteral("lifetime")).toDouble(5.0));

    QString mat = obj.value(QStringLiteral("material")).toString();
    if (!mat.isEmpty()) {
        SceneLayer tmpLayer;
        resolveMaterial(pkgReader, mat.toStdString(), tmpLayer);
        outEmitter.particleSprite = tmpLayer.image;
        outEmitter.blending = tmpLayer.blending;
    }

    return true;
}

} // namespace WallpaperEngine::Scene
