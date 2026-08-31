set(CMAKE_AUTOMOC ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)

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
    src/scene/particle_engine.cpp
    src/scene/scene_compositor.cpp
    src/render/render_graph.cpp
    src/render/mesh_renderer.cpp
    src/audio/audio_player.h
    src/audio/audio_player.cpp
    src/audio/audio_visualizer.h
    src/audio/audio_visualizer.cpp
    src/vulkan/vulkan_context.cpp
    src/ipc/wallpaper_service.h
    src/ipc/wallpaper_service.cpp
    src/plugin/wallpaper_plugin.h
    src/plugin/wallpaper_plugin.cpp
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
    src/scene/particle_engine.cpp
    src/scene/scene_compositor.cpp
    src/render/render_graph.cpp
    src/render/mesh_renderer.cpp
    src/audio/audio_player.h
    src/audio/audio_player.cpp
    src/audio/audio_visualizer.h
    src/audio/audio_visualizer.cpp
    src/vulkan/vulkan_context.cpp
    src/ipc/wallpaper_service.h
    src/ipc/wallpaper_service.cpp
    src/plugin/wallpaper_plugin.h
    src/plugin/wallpaper_plugin.cpp
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

target_compile_options(plasma-wallpaper-engine-verifier PRIVATE -fexceptions)

install(TARGETS plasma-wallpaper-engine-daemon DESTINATION ${KDE_INSTALL_LIBEXECDIR})
