# Optional wlroots layer-shell wallpaper client.
# Requires LayerShellQt (wlr-layer-shell protocol binding). Skipped with a
# status note when the dependency is missing so core daemon/viewer builds
# stay dependency-minimal.
find_package(LayerShellQt QUIET)

set(HAVE_LAYERSHELL OFF)
if(LayerShellQt_FOUND)
    set(HAVE_LAYERSHELL ON)
    message(STATUS "Layer-shell client: ENABLED (LayerShellQt found)")
else()
    message(STATUS "Layer-shell client: DISABLED (LayerShellQt not found)")
endif()

if(HAVE_LAYERSHELL)
    set(CMAKE_AUTOMOC ON)

    add_executable(plasma-wallpaper-engine-layerclient
        src/main.cpp
        src/wallpaper_layer.h
        src/wallpaper_layer.cpp
    )

    target_include_directories(plasma-wallpaper-engine-layerclient
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    # LAYER_QML_SOURCE_DIR: the QML scene ships as a real file so it can be
    # edited without touching the binary; the executable embeds its location
    # at build time.
    target_compile_definitions(plasma-wallpaper-engine-layerclient PRIVATE
        LAYER_QML_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src"
    )

    target_link_libraries(plasma-wallpaper-engine-layerclient
        PRIVATE
            Qt6::Core
            Qt6::Gui
            Qt6::Quick
            Qt6::Qml
            Qt6::DBus
            LayerShellQt::Interface
    )

    install(TARGETS plasma-wallpaper-engine-layerclient DESTINATION ${KDE_INSTALL_BINDIR})
endif()
