 PLASMA WALLPAPER ENGINE - REMEDIATION ROADMAP

  CURRENT STATE SUMMARY
  - ✅ Completed: VideoDecoder UAF fix, DBus loadWallpaper security hardening
  - ⚠️ OPEN: Compilation errors in Qt stubs and missing QRgb/qsizetype definitions
  - 🎯 NEXT: Fix compilation, implement security fixes, add plugin architecture

  ---

  DOCUMENTATION: REMEDIATION ROADMAP

  PHASE 1: CRITICAL COMPILATION FIXES (High Priority)

  Duration: 60 minutes | Easiest to Hardest

  1.1 FIX QRgb / qsizetype DEFINITIONS (5 minutes)
  - File: daemon/src/assets/video_decoder.h
  - Problem: Missing QRgb and qsizetype type definitions causing compilation errors
  - Solution: Add proper typedefs for QImage stub compatibility

  1.2 EXPAND QT STUBS (15 minutes)
  - File: daemon/src/ipc/wallpaper_service.h
  - Problem: Stub types incomplete (missing QRgb, qsizetype)
  - Solution: Add complete stub implementations for all Qt types used

  1.3 FIX QIMAGE STUB IMPLEMENTATION (10 minutes)
  - File: daemon/src/ipc/wallpaper_service.cpp
  - Problem: QImage stub missing required methods (width(), height(), isNull(), etc.)
  - Solution: Implement complete QImage stub with all required methods

  1.4 IMPLEMENT QStringHelper CLASS (10 minutes)
  - File: daemon/src/ipc/wallpaper_service.cpp
  - Problem: Multiple QString methods used (toStdString, startsWith, contains, toLower)
  - Solution: Create helper class with proper string manipulation methods

  1.5 COMPILATION TEST (5 minutes)
  - Build verification that all compilation errors are resolved
  - Verify Qt stubs provide necessary functionality

  ---

  PHASE 2: SECURITY FIXES (Medium Priority)

  Duration: 90 minutes | Security Critical

  2.1 PATCH PATH TRAVERSAL VULNERABILITIES
  - Files: daemon/src/assets/pkg_reader.cpp, daemon/src/assets/tex_parser.cpp
  - Add bounds checking for file paths and string lengths
  - Reject "../", "..\" and other traversal patterns

  2.2 FIX LZ4 DECOMPRESSION BOUNDS CHECKING
  - File: daemon/src/assets/tex_parser.cpp
  - Validate uncompSize before LZ4_decompress_safe
  - Add integer overflow protection

  2.3 PATCH PKG READER UNBOUNDED MEMORY
  - File: daemon/src/assets/pkg_reader.cpp
  - Limit fileCount to reasonable values (e.g., max 1024 entries)
  - Add maximum string length limits (e.g., 256KB for JSON)

  2.4 IMPLEMENT SIGNAL HANDLER SAFETY
  - File: daemon/src/main.cpp
  - Replace Qt signal handlers with Qt-compatible alternatives

  ---

  PHASE 3: FEATURE ENHANCEMENT (Lower Priority)

  Duration: Variable | Feature Parity

  3.1 ADD VULKAN COMPUTE SHADER PIPELINE
  - Replace QPainter with Vulkan compute shaders
  - Implement GPU-accelerated scene rendering

  3.2 IMPLEMENT AUDIO VISUALIZER
  - Add PulseAudio/PipeWire FFT integration
  - Expose spectrum data to Scene/Web wallpapers

  3.3 CREATE PLUGIN ARCHITECTURE
  - Header-based plugin system for wallpaper extensions
  - Dynamic loading/unloading support

  3.4 ADD WEB WALLPAPER SUPPORT
  - Integrate CEF/QtWebEngine for HTML5 wallpapers
  - Add JavaScript-C++ bridge

  ---

  IMPLEMENTATION SEQUENCE

  Week 1: Compilation Fixes (Priority 1)
  1. Fix video_decoder.h QRgb/qsizetype (5 min)
  2. Complete Qt stubs (30 min)
  3. Compilation test (5 min)

  Week 2: Security Hardening (Priority 2)
  1. Patch path traversal vulnerabilities (30 min)
  2. Fix LZ4 bounds checking (20 min)
  3. Patch PKG reader memory limits (20 min)
  4. Implement signal handler safety (15 min)

  Week 3: Feature Development (Priority 3)
  1. Begin Vulkan compute pipeline (60 min)
  2. Add audio visualizer skeleton (30 min)
  3. Start plugin architecture (30 min)

  Week 4: Integration & Testing
  1. End-to-end testing
  2. Performance optimization
  3. Documentation updates

  ---

  CURRENT STATUS CHECK

  COMPLETED:
  - ✅ VideoDecoder UAF fix (shared_ptr)
  - ✅ DBus loadWallpaper security hardening
  - ✅ Basic Qt stubs (partial implementation)

  REMAINING:
  - ❌ QRgb/qsizetype definitions in video_decoder.h
  - ❌ Complete Qt stubs in wallpaper_service.h
  - ❌ QFileInfo, QJsonDocument stub implementations
  - ❌ QString helper methods
  - ❌ Path traversal fixes
  - ❌ LZ4 bounds checking

  ---

  NEXT STEPS EXECUTION

  I. START: Fix video_decoder.h (5 minutes)
  - Add QRgb and qsizetype type definitions
  - Ensure QImage stub compiles correctly

  II. CONTINUE: Expand Qt stubs (15 minutes)
  - Complete stub implementations for all Qt types
  - Add QRgb typedefs across header files

  III. PROCEED: Implement QString helpers (10 minutes)
  - Create helper class for string manipulation
  - Add toLower(), startsWith(), contains(), toStdString()

  IV. TEST: Compilation verification (5 minutes)
  - Run compiler to verify no errors
  - Fix any remaining stub issues