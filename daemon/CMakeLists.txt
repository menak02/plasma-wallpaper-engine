set(CMAKE_AUTOMOC ON)

# Shader compilation (requires glslangValidator)
find_program(GLSLANG_VALIDATOR glslangValidator)
if(GLSLANG_VALIDATOR)
    file(GLOB SHADER_COMPS "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/*.comp")
    foreach(SHADER ${SHADER_COMPS})
        get_filename_component(SHADER_NAME ${SHADER} NAME_WE)
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders_spv_${SHADER_NAME}.h.check"
            COMMAND ${GLSLANG_VALIDATOR} -V "${SHADER}" -o "/tmp/${SHADER_NAME}.spv"
            COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/shaders_spv_${SHADER_NAME}.h.check"
            DEPENDS "${SHADER}"
            COMMENT "Validating ${SHADER_NAME}.comp"
        )
    endforeach()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)

# Optional WebEngine
find_package(Qt6 QUIET COMPONENTS WebEngineWidgets WebEngineCore)
if(Qt6WebEngineWidgets_FOUND)
    message(STATUS "Qt6 WebEngine: ENABLED")
    set(HAVE_WEBENGINE TRUE)
else()
    message(STATUS "Qt6 WebEngine: DISABLED (not found)")
    set(HAVE_WEBENGINE FALSE)
endif()
find_package(Qt6 QUIET COMPONENTS Multimedia)
if(Qt6Multimedia_FOUND)
    message(STATUS "Qt6 Multimedia: ENABLED")
    set(HAVE_MULTIMEDIA TRUE)
else()
    message(STATUS "Qt6 Multimedia: DISABLED")
    set(HAVE_MULTIMEDIA FALSE)
endif()

# ---------- Daemon ----------
add_executable(plasma-wallpaper-engine-daemon
    src/main.cpp
    src/assets/pkg_reader.cpp
    src/assets/tex_parser.cpp
    src/assets/dxt_decoder.cpp
    src/assets/video_decoder.cpp
    src/assets/library_scanner.h
    src/assets/library_scanner.cpp
    src/scene/scene_parser.cpp
    src/scene/js_engine.cpp
    src/scene/web_wallpaper.cpp
    src/scene/web_wallpaper.h
    src/scene/particle_engine.cpp
    src/scene/scene_compositor.cpp
    src/render/render_graph.cpp
    src/render/mesh_renderer.cpp
    src/audio/audio_player.h
    src/audio/audio_player.cpp
    src/audio/audio_visualizer.h
    src/audio/audio_visualizer.cpp
    src/common/managed_process.h
    src/common/managed_process.cpp
    src/scene/compositor_backend.h
    src/scene/compositor_backend.cpp
    src/scene/pause_gate.h
    src/scene/pause_gate.cpp
    src/vulkan/vulkan_context.cpp
    src/ipc/wallpaper_service.h
    src/ipc/wallpaper_service.cpp
    src/ipc/wallpaper_service_adaptor.h
    src/ipc/wallpaper_service_adaptor.cpp
    src/plugin/wallpaper_plugin.h
    src/plugin/wallpaper_plugin.cpp
    src/render/gpu_quad_compositor.h
    src/render/gpu_quad_compositor.cpp
    src/render/vulkan_compute.h
    src/render/vulkan_compute.cpp
)

target_include_directories(plasma-wallpaper-engine-daemon
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LZ4_INCLUDE_DIRS}
        ${AVFORMAT_INCLUDE_DIRS}
        ${AVCODEC_INCLUDE_DIRS}
        ${AVUTIL_INCLUDE_DIRS}
        ${SWSCALE_INCLUDE_DIRS}
)

target_link_libraries(plasma-wallpaper-engine-daemon
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::DBus
        Qt6::Network
        Vulkan::Vulkan
        ${LZ4_LIBRARIES}
        ${AVFORMAT_LIBRARIES}
        ${AVCODEC_LIBRARIES}
        ${AVUTIL_LIBRARIES}
        ${SWSCALE_LIBRARIES}
)
if(HAVE_WEBENGINE)
    target_link_libraries(plasma-wallpaper-engine-daemon PRIVATE Qt6::WebEngineWidgets Qt6::WebEngineCore)
    target_compile_definitions(plasma-wallpaper-engine-daemon PRIVATE HAVE_WEBENGINE=1)
endif()
if(HAVE_MULTIMEDIA)
    target_link_libraries(plasma-wallpaper-engine-daemon PRIVATE Qt6::Multimedia)
    target_compile_definitions(plasma-wallpaper-engine-daemon PRIVATE HAVE_MULTIMEDIA=1)
endif()

target_compile_options(plasma-wallpaper-engine-daemon PRIVATE -fexceptions)

# ---------- Batch Verifier Diagnostic Tool ----------
add_executable(plasma-wallpaper-engine-verifier
    src/tools/batch_verifier.cpp
    src/assets/pkg_reader.cpp
    src/assets/tex_parser.cpp
    src/assets/dxt_decoder.cpp
    src/assets/video_decoder.cpp
    src/assets/library_scanner.h
    src/assets/library_scanner.cpp
    src/scene/scene_parser.cpp
    src/scene/js_engine.cpp
    src/scene/web_wallpaper.cpp
    src/scene/web_wallpaper.h
    src/scene/particle_engine.cpp
    src/scene/scene_compositor.cpp
    src/render/render_graph.cpp
    src/render/mesh_renderer.cpp
    src/audio/audio_player.h
    src/audio/audio_player.cpp
    src/audio/audio_visualizer.h
    src/audio/audio_visualizer.cpp
    src/common/managed_process.h
    src/common/managed_process.cpp
    src/scene/compositor_backend.h
    src/scene/compositor_backend.cpp
    src/scene/pause_gate.h
    src/scene/pause_gate.cpp
    src/scene/compositor_backend_fake.h
    src/vulkan/vulkan_context.cpp
    src/ipc/wallpaper_service.h
    src/ipc/wallpaper_service.cpp
    src/ipc/wallpaper_service_adaptor.h
    src/ipc/wallpaper_service_adaptor.cpp
    src/plugin/wallpaper_plugin.h
    src/plugin/wallpaper_plugin.cpp
    src/render/gpu_quad_compositor.h
    src/render/gpu_quad_compositor.cpp
    src/render/vulkan_compute.h
    src/render/vulkan_compute.cpp
)

target_include_directories(plasma-wallpaper-engine-verifier
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LZ4_INCLUDE_DIRS}
        ${AVFORMAT_INCLUDE_DIRS}
        ${AVCODEC_INCLUDE_DIRS}
        ${AVUTIL_INCLUDE_DIRS}
        ${SWSCALE_INCLUDE_DIRS}
)

target_link_libraries(plasma-wallpaper-engine-verifier
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::DBus
        Qt6::Network
        Vulkan::Vulkan
        ${LZ4_LIBRARIES}
        ${AVFORMAT_LIBRARIES}
        ${AVCODEC_LIBRARIES}
        ${AVUTIL_LIBRARIES}
        ${SWSCALE_LIBRARIES}
)
if(HAVE_WEBENGINE)
    target_link_libraries(plasma-wallpaper-engine-verifier PRIVATE Qt6::WebEngineWidgets Qt6::WebEngineCore)
    target_compile_definitions(plasma-wallpaper-engine-verifier PRIVATE HAVE_WEBENGINE=1)
endif()
if(HAVE_MULTIMEDIA)
    target_link_libraries(plasma-wallpaper-engine-verifier PRIVATE Qt6::Multimedia)
    target_compile_definitions(plasma-wallpaper-engine-verifier PRIVATE HAVE_MULTIMEDIA=1)
endif()

target_compile_options(plasma-wallpaper-engine-verifier PRIVATE -fexceptions)

install(TARGETS plasma-wallpaper-engine-daemon DESTINATION ${KDE_INSTALL_LIBEXECDIR})

# Autostart (T5) intentionally removed: idle power draw (16-17W) is too high
# until GPU composition fully replaces the CPU painter path. Unit template
# lives in git history (packaging/plasma-wallpaper-engine-daemon.service.in).

# ---------- Headless pause gate test ----------
# Registered at this scope so ctest sees it directly. The test binary is
# built in the daemon build tree and linked to Qt6::Core plus the pause gate
# implementation.
add_executable(pause_gate_test
    ../tests/pause_gate_test.cpp
    src/scene/compositor_backend_fake.h
)

target_include_directories(pause_gate_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scene
)

target_link_libraries(pause_gate_test
    PRIVATE
        Qt6::Core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/pause_gate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/compositor_backend.cpp
)

# ---------- GPU deform pipeline probe ----------
# Headless Vulkan probe: renders a mixed plain+deform scene offscreen and
# validates the composited boundary against CPU MeshDeformer math. Exits 77
# (skip) when no Vulkan device is present, so GPU-less environments pass.
add_executable(gpu_deform_probe
    ../tests/gpu_deform_probe.cpp
    src/render/gpu_quad_compositor.cpp
    src/vulkan/vulkan_context.cpp
    src/render/mesh_renderer.cpp
)

target_include_directories(gpu_deform_probe
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(gpu_deform_probe
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Vulkan::Vulkan
)

target_compile_options(gpu_deform_probe PRIVATE -fexceptions)

# Test registered at top level so ctest sees it directly.
# (pause_gate_headless is added in the top-level CMakeLists.txt.)

# Test registered at top level so ctest sees it directly.
# (pause_gate_headless is added in the top-level CMakeLists.txt.)
