#include <iostream>
#include "assets/pkg_reader.h"
#include "scene/scene_parser.h"

int main() {
    WallpaperEngine::Assets::PkgReader pkgReader;
    if (pkgReader.open("/home/mena/.steam/steam/steamapps/workshop/content/431960/1103032583/scene.pkg")) {
        WallpaperEngine::Scene::SceneDescription desc;
        WallpaperEngine::Scene::SceneParser::parseScene(pkgReader, desc);
        for (const auto& layer : desc.layers) {
            std::cout << "Layer: " << layer.name << " image size: " << layer.image.width() << "x" << layer.image.height() << std::endl;
        }
    }
    return 0;
}
