---
id: learning-video-decoder-uaf-fix
title: video-decoder-uaf-fix
type: learning
scope: project
project: plasma-wallpaper-engine
created: "2026-08-30T16:49:25.795Z"
updated: "2026-08-30T16:49:39.174Z"
tags:
  - security
  - memory-safety
  - video
  - project
---

VideoDecoder use-after-free fixed by changing m_dataCopy from std::vector<uint8_t> to std::shared_ptr<std::vector<uint8_t>> to ensure memory validity during async FFmpeg callbacks.
