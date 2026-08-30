---
id: learning-js-visibility-for-wallpaper-engine-layers
title: JS visibility for Wallpaper Engine layers
type: learning
scope: project
project: plasma-wallpaper-engine
created: "2026-08-30T00:59:50.237Z"
updated: "2026-08-30T01:00:04.248Z"
tags:
  - wallpaperscene
  - qjsengine
  - visibility
  - project
---

Wallpaper Engine uses JS expressions like {"user":"clockdateday","value":false} for layer visibility. Must use QJSEngine to evaluate, not parse as plain bool. Hidden layers (clock/date/day) correctly show visible=0 after eval.
