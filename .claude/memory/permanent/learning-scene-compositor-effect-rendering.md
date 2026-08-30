---
id: learning-scene-compositor-effect-rendering
title: Scene compositor effect rendering
type: learning
scope: project
project: plasma-wallpaper-engine
created: "2026-08-30T00:59:51.784Z"
updated: "2026-08-30T01:00:04.248Z"
tags:
  - wallpaperscene
  - effects
  - compositor
  - project
---

Wallpaper Engine scene_compositor.cpp requires explicit handling for FilmGrain (random noise overlay), BlurPrecise (gaussian blur), Iris (radial mask), FoliageSway (sin-based x-deform). These effects have type enums but no default render path. Must include <cstdlib> for FilmGrain randomness.
