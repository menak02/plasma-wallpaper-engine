# Plasma Wallpaper Engine - Project Maturity Roadmap

This document serves as the central planning board and roadmap for the project.

## Current Status
- **Branch**: feat/hyprland-pause
- **Latest commit**: ad7fff7 (chore: add GitHub project configuration and CI workflows)
- **Main branch**: master (up-to-date with all commits through September 4)

## Repository Structure
- **Main branch (master)**: Stable commits, all development through September 3
- **Testing branch (feat/hyprland-pause)**: New features being tested, includes:
  - S3: Pause gate implementation
  - Managed process wrapper
  - Headless decision tests
  - GitHub Actions CI setup

## Completed Work

### S1-S2: Foundation
- [x] Vulkan compute pipeline
- [x] Video wallpaper support  
- [x] Web wallpaper with JS engine
- [x] Audio-reactive wallpapers
- [x] Film grain post-processing
- [x] Multi-output DmaBuf buffers

### S3: Pause Gate (Current)
- [x] ManagedProcess wrapper with deterministic lifecycle
- [x] AudioPlayer/AudioVisualizer cleanup
- [x] CompositorBackend interface
- [x] Hyprland IPC backend (real implementation with hyprctl polling)
- [x] Pause gate logic (per-output, toggleable)
- [x] Headless decision tests
- [x] CI/CD workflows
- [x] Connection test for CI validation

## Upcoming Work

### High Priority
1. **Expose pause settings via D-Bus** (#10) - Runtime configurability
   
   Note: Hyprland IPC backend is now implemented (#61369f7)

### Medium Priority  
3. **Add musl/Alpine CI runner** (#11) - Cross-compiler testing
4. **Nightly ASan builds** (#12) - Memory issue detection
5. **Fix pactl probe churn** (#13) - Performance optimization

### Lower Priority
6. **Settings consolidation** (#14) - Review other wallpaper engines
7. **Multi-output support** (#15) - Proper per-output coverage
8. **QT6 settings UI** (#16) - User-friendly configuration

## Architecture Decisions

### Detection Model
- **Covered when**: fullscreen-family window OR >=90% tiling coverage
- **Per-output default**: one monitor covered doesn't pause others  
- **Pause-all toggle**: optional global pause behavior (Windows DWM-like)

### Performance Requirements
- Lightweight daemon (~256MB VRAM target)
- No stale processes - instant kill capability
- Clean resource management
- Event-driven for instant resume

### Pause Semantics
- Pause = tick skip for affected output(s)
- Scene stays loaded, last frame stays available
- Resume is zero-deferred
- OST: muted by default while paused (user-toggleable)

## GitHub Project Setup

### Issues Created
Total: 16 issues tracking all work items

### Workflows
- **build-and-test.yml**: Build + ctest + repo-quality gate on push/PR (GREEN)
- **security-scan.yml**: Secret scanning + vulnerability checks (GREEN)

### Issue Templates
- feature_request.md
- bug_report.md
- config.yml

## Testing Strategy

### Headless Testing
- Daemon starts without compositor
- D-Bus service exports
- Render gate toggleable
- Managed processes clean up properly

### CI Matrix
- Compiler: gcc, clang
- Build type: Debug, Release
- Sanitizers: ASan (nightly)

## Security Considerations

### Current
- DBus loadWallpaper restricted to trusted library roots
- No hardcoded secrets in codebase

### CI Security
- Secret scanning in workflows
- Dependency vulnerability checks
