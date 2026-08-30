---
id: decision-js-engine-choice
title: Use QJSEngine for JS visibility evaluation
type: decision
scope: project
project: plasma-wallpaper-engine
created: "2026-08-30T00:59:53.321Z"
updated: "2026-08-30T01:00:04.247Z"
tags:
  - project
---

Use QJSEngine (Qt5Qml) for JS expression evaluation in scene parser. Provides safe sandboxed execution for Wallpaper Engine user-property visibility patterns without exposing full V8/JSC.
