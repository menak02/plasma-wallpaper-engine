---
id: learning-worktree-isolation-for-cpp-edits
title: Worktree isolation for cpp edits
type: learning
scope: project
project: plasma-wallpaper-engine
created: "2026-08-30T00:59:55.115Z"
updated: "2026-08-30T01:00:04.249Z"
tags:
  - git
  - worktree
  - qt-build
  - project
---

When using EnterWorktree for C++ Qt/CMake projects, build dir gets nested. Edit in worktree, copy files to main, then remove worktree dir to avoid nested .git tracking. Use git reset to clean merge artifacts.
