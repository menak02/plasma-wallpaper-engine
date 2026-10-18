#pragma once

#include <QImage>
#include <vector>

#include "scene_parser.h"

namespace WallpaperEngine::Scene
{

/**
 * Static (time-independent) visual effect passes — currently Blur and Tint.
 *
 * These are applied once at parse time, not per frame: the 60 Hz render loop
 * must not pay for effects whose output never changes between frames. The
 * animated passes (water, sway, god rays, shine, pulse, shake, scroll) are
 * handled in the compositor through the mesh-deform and transform paths.
 *
 * All effect constants are clamped at the parser boundary (untrusted scene
 * data) and re-clamped here; every pixel loop is bounded by the image size.
 */

// True when the chain contains a visible static pass worth baking.
bool hasStaticEffects(const std::vector<LayerEffect>& effects);

// Bake the visible static passes into a copy of src.
QImage applyStaticEffects(const QImage& src, const std::vector<LayerEffect>& effects);

} // namespace WallpaperEngine::Scene
