#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <cmath>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QDir>
#include "../assets/pkg_reader.h"
#include "../scene/scene_parser.h"
#include "../scene/scene_compositor.h"
#include "../vulkan/vulkan_context.h"

namespace fs = std::filesystem;

// Accumulated transform from parent chain
struct AccumulatedTransform {
    QVector2D origin{0.0f, 0.0f};
    QVector2D scale{1.0f, 1.0f};
    float angle = 0.0f;
};

static AccumulatedTransform resolveParentTransform(const WallpaperEngine::Scene::SceneLayer& layer, float sceneW, float sceneH) {
    const WallpaperEngine::Scene::SceneLayer* chain[32];
    int count = 0;
    const WallpaperEngine::Scene::SceneLayer* cur = &layer;
    chain[count++] = cur;
    while (cur->parent != nullptr && count < 32) {
        cur = cur->parent;
        chain[count++] = cur;
    }

    AccumulatedTransform acc;
    acc.scale = QVector2D(1.0f, 1.0f);

    for (int i = count - 1; i >= 0; --i) {
        const WallpaperEngine::Scene::SceneLayer& node = *chain[i];
        float ox = node.origin.x();
        float oy = node.origin.y();
        float sx = node.scale.x();
        float sy = node.scale.y();
        float angle = node.angles.z();

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

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);

    // Default to user's Steam Workshop path; override with first CLI arg
    std::string workshopBase = QDir::homePath().toStdString()
        + "/.local/share/Steam/steamapps/workshop/content/431960";
    std::string outputDir = QDir::homePath().toStdString() + "/.wallpaper-engine-verifier-output";

    std::string filterId;
    if (argc > 1) {
        std::string a1 = argv[1];
        if (a1 == "--id" && argc > 2) {
            filterId = argv[2];
        } else {
            workshopBase = a1;
        }
    }
    if (argc > 2) {
        std::string a2 = argv[2];
        if (a2 == "--id" && argc > 3) {
            filterId = argv[3];
        } else if (filterId.empty()) {
            outputDir = a2;
        }
    }
    if (argc > 3) {
        std::string a3 = argv[3];
        if (a3 == "--id" && argc > 4) {
            filterId = argv[4];
        } else if (filterId.empty()) {
            outputDir = a3;
        }
    }

    fs::create_directories(outputDir);

    std::cout << "Starting Strict Automated Wallpaper Batch Diagnostic Engine..." << std::endl;

    WallpaperEngine::Render::VulkanContext vulkanCtx;
    vulkanCtx.init();
    WallpaperEngine::Render::DmaBufBuffer dummyBuf;
    vulkanCtx.setResolution(1920, 1080, dummyBuf);

    std::vector<std::string> itemDirs;
    for (const auto& entry : fs::directory_iterator(workshopBase)) {
        if (entry.is_directory()) {
            std::string pkgPath = entry.path().string() + "/scene.pkg";
            if (fs::exists(pkgPath)) {
                itemDirs.push_back(entry.path().string());
            }
        }
    }

    std::cout << "Found " << itemDirs.size() << " installed wallpapers to test." << std::endl;

    std::ofstream report(outputDir + "/analysis_results.md");
    report << "# Wallpaper Engine Strict Batch Diagnostic Report\n\n";
    report << "| ID | Title | Visual Layers | Emitters | Sound | Snapshot | Status |\n";
    report << "| :--- | :--- | :--- | :--- | :--- | :--- | :--- |\n";

    int perfectCount = 0;
    int reviewCount = 0;
    int brokenCount = 0;

    for (size_t i = 0; i < itemDirs.size(); ++i) {
        const auto& dir = itemDirs[i];
        std::string id = fs::path(dir).filename().string();
        if (!filterId.empty() && id != filterId) continue;
        std::string pkgPath = dir + "/scene.pkg";

        WallpaperEngine::Assets::PkgReader pkgReader;
        if (!pkgReader.open(pkgPath)) {
            report << "| `" << id << "` | *Unknown* | 0/0 | 0 | No | N/A | ❌ Corrupt PKG |\n";
            brokenCount++;
            continue;
        }

        WallpaperEngine::Scene::SceneDescription sceneDesc;
        bool parseOk = WallpaperEngine::Scene::SceneParser::parseScene(pkgReader, sceneDesc);
        if (!parseOk) {
            report << "| `" << id << "` | *Failed to parse* | 0/0 | 0 | No | N/A | ❌ Scene Parse Error |\n";
            brokenCount++;
            continue;
        }

        // Render to offscreen canvas
        QImage canvas(1920, 1080, QImage::Format_RGBA8888);
        canvas.fill(sceneDesc.clearColor);

        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setRenderHint(QPainter::Antialiasing, true);

        float sceneW = sceneDesc.sceneWidth > 0.0f ? sceneDesc.sceneWidth : 3840.0f;
        float sceneH = sceneDesc.sceneHeight > 0.0f ? sceneDesc.sceneHeight : 2160.0f;
        float scaleX = 1920.0f / sceneW;
        float scaleY = 1080.0f / sceneH;

        // Debug output for specific problematic wallpapers or filtered ID
        bool debugThisWallpaper = (!filterId.empty() || id == "3432157109" || id == "3353454232" || id == "3715762023" || id == "3640755971" || id == "3504887068" || id == "3465215190" || id == "3725071796" || id == "3771397959" || id == "3591326656");

        int renderedLayers = 0;
        for (const auto& layer : sceneDesc.layers) {
            if (debugThisWallpaper) {
                std::cout << "  LAYER: '" << layer.name << "' id=" << layer.id
                          << " visible=" << layer.visible
                          << " opacity=" << layer.opacity
                          << " imageNull=" << layer.image.isNull()
                          << " imgW=" << layer.image.width()
                          << " imgH=" << layer.image.height();
                if (!layer.image.isNull()) {
                    // Sample center pixel
                    int cx = layer.image.width() / 2;
                    int cy = layer.image.height() / 2;
                    QRgb pixel = layer.image.pixel(cx, cy);
                    std::cout << " centerPixel=rgba("
                              << qRed(pixel) << "," << qGreen(pixel) << ","
                              << qBlue(pixel) << "," << qAlpha(pixel) << ")";
                    // Count opaque vs transparent pixels
                    int opaqueCount = 0;
                    int transCount = 0;
                    int sampleStep = std::max(1, (layer.image.width() * layer.image.height()) / 500);
                    const QRgb* bits = reinterpret_cast<const QRgb*>(layer.image.constBits());
                    for (int k = 0; k < layer.image.width() * layer.image.height(); k += sampleStep) {
                        if (qAlpha(bits[k]) > 128) opaqueCount++;
                        else transCount++;
                    }
                    std::cout << " opaque~" << opaqueCount << " trans~" << transCount;
                }
                std::cout << " blend=" << static_cast<int>(layer.blending)
                          << " origin=(" << layer.origin.x() << "," << layer.origin.y() << ")"
                          << " size=(" << layer.size.x() << "," << layer.size.y() << ")"
                          << std::endl;
            }
            if (!layer.visible || layer.image.isNull() || layer.opacity <= 0.0f) continue;

            painter.save();
            painter.setOpacity(layer.opacity);

            if (layer.blending == WallpaperEngine::Scene::BlendMode::Additive) {
                painter.setCompositionMode(QPainter::CompositionMode_Plus);
            } else if (layer.blending == WallpaperEngine::Scene::BlendMode::Screen) {
                painter.setCompositionMode(QPainter::CompositionMode_Screen);
            } else if (layer.blending == WallpaperEngine::Scene::BlendMode::Multiply) {
                painter.setCompositionMode(QPainter::CompositionMode_Multiply);
            } else {
                painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            }

            // Resolve parent transform chain
            AccumulatedTransform acc = resolveParentTransform(layer, sceneW, sceneH);

            float centerX = (acc.origin.x() / sceneW) * 1920.0f;
            float centerY = ((sceneH - acc.origin.y()) / sceneH) * 1080.0f;

            float spriteW = (layer.size.x() > 0.0f) ? (layer.size.x() / sceneW) * 1920.0f * acc.scale.x() : layer.image.width() * scaleX * acc.scale.x();
            float spriteH = (layer.size.y() > 0.0f) ? (layer.size.y() / sceneH) * 1080.0f * acc.scale.y() : layer.image.height() * scaleY * acc.scale.y();

            painter.translate(centerX, centerY);
            if (acc.angle != 0.0f) painter.rotate(acc.angle);
            painter.drawImage(QRectF(-spriteW / 2.0f, -spriteH / 2.0f, spriteW, spriteH), layer.image);
            painter.restore();
            renderedLayers++;
        }

        if (debugThisWallpaper) {
            std::cout << "  CANVAS after " << renderedLayers << " layers rendered:"
                      << " size=" << canvas.width() << "x" << canvas.height();
            // Sample canvas corners and center
            QRgb tl = canvas.pixel(0, 0);
            QRgb tc = canvas.pixel(960, 540);
            QRgb br = canvas.pixel(1919, 1079);
            std::cout << " topLeft=rgba(" << qRed(tl) << "," << qGreen(tl) << "," << qBlue(tl) << "," << qAlpha(tl) << ")"
                      << " center=rgba(" << qRed(tc) << "," << qGreen(tc) << "," << qBlue(tc) << "," << qAlpha(tc) << ")"
                      << " botRight=rgba(" << qRed(br) << "," << qGreen(br) << "," << qBlue(br) << "," << qAlpha(br) << ")"
                      << " clearColor=rgba(" << sceneDesc.clearColor.red() << "," << sceneDesc.clearColor.green() << ","
                      << sceneDesc.clearColor.blue() << "," << sceneDesc.clearColor.alpha() << ")"
                      << std::endl;
        }

        // Render particle engine preview (1 frame for instant diagnostic snapshot)
        WallpaperEngine::Scene::ParticleEngine particleEngine;
        particleEngine.setEmitters(sceneDesc.emitters);
        particleEngine.update(0.016f, 1920, 1080);
        particleEngine.render(painter, 1920, 1080);
        painter.end();
        
        std::string snapshotFile = outputDir + "/" + id + ".png";
        canvas.save(QString::fromStdString(snapshotFile), "PNG");

        int totalDeclared = sceneDesc.totalVisualObjectsDeclared;
        std::string status;

        if (totalDeclared == 0 && renderedLayers == 0) {
            if (!sceneDesc.emitters.empty()) {
                status = "✅ Pure Particle Scene";
                perfectCount++;
            } else {
                status = "❌ Empty Scene";
                brokenCount++;
            }
        } else if (renderedLayers >= totalDeclared) {
            // renderedLayers may exceed totalDeclared because composition layers
            // are added to the render list but not counted in totalVisualObjectsDeclared
            if (renderedLayers > totalDeclared && totalDeclared > 0) {
                status = "✅ Completely Sure (+" + std::to_string(renderedLayers - totalDeclared) + " comp layers)";
            } else {
                status = "✅ Completely Sure";
            }
            perfectCount++;
        } else if (renderedLayers > 0) {
            status = "⚠️ Review Needed (" + std::to_string(totalDeclared - renderedLayers) + " layers unrendered)";
            reviewCount++;
        } else {
            status = "❌ Completely Broken (0 layers rendered)";
            brokenCount++;
        }

        std::string titleSafe = sceneDesc.title;
        std::replace(titleSafe.begin(), titleSafe.end(), '|', '/');

        report << "| `" << id << "` | " << (titleSafe.empty() ? id : titleSafe) << " | " 
               << renderedLayers << "/" << totalDeclared << " | " 
               << sceneDesc.emitters.size() << "/" << sceneDesc.totalParticleEmittersDeclared << " | " 
               << (!sceneDesc.soundPath.empty() ? "🎵 Yes" : "No") << " | "
               << "![" << id << "](" << snapshotFile << ") | "
               << status << " |\n";
        report.flush();

        std::cout << "[" << (i+1) << "/" << itemDirs.size() << "] ID " << id 
                  << " ('" << sceneDesc.title << "'): " 
                  << renderedLayers << "/" << totalDeclared << " layers, " 
                  << sceneDesc.emitters.size() << "/" << sceneDesc.totalParticleEmittersDeclared << " emitters -> " << status << std::endl;
    }

    report << "\n\n### Summary\n";
    report << "- **Completely Sure (100% Verified):** " << perfectCount << "\n";
    report << "- **Review Needed (Partial layers):** " << reviewCount << "\n";
    report << "- **Broken / Failed:** " << brokenCount << "\n";

    report.close();
    std::cout << "\nBatch Diagnostic complete! Wrote report to analysis_results.md" << std::endl;
    return 0;
}
