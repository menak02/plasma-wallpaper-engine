# Wallpaper display client.
#
# Two display paths, because "put this window behind everything" is not a
# portable concept:
#
#   Wayland  wlr-layer-shell background surface, via LayerShellQt.
#            Optional — the target still builds without it and then only
#            supports the X11 path.
#   X11      EWMH desktop-level window (_NET_WM_WINDOW_TYPE_DESKTOP plus the
#            _NET_WM_STATE set and _NET_WM_DESKTOP = 0xFFFFFFFF). Always
#            built, needs only libxcb.
#
# The path is picked at runtime from $WAYLAND_DISPLAY / $DISPLAY, not at
# configure time, so a single binary works on both.
find_package(LayerShellQt QUIET)

set(HAVE_LAYERSHELL OFF)
if(LayerShellQt_FOUND)
    set(HAVE_LAYERSHELL ON)
    message(STATUS "Wallpaper client: layer-shell path ENABLED (LayerShellQt found)")
else()
    message(STATUS "Wallpaper client: layer-shell path DISABLED (LayerShellQt not found)")
    message(STATUS "Wallpaper client: X11 desktop-window path only")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED xcb)
message(STATUS "Wallpaper client: X11 desktop-window path ENABLED (libxcb ${XCB_VERSION})")

set(CMAKE_AUTOMOC ON)

add_executable(plasma-wallpaper-engine-layerclient
    src/main.cpp
    src/wallpaper_layer.h
    src/wallpaper_layer.cpp
    src/x11_desktop_window.h
    src/x11_desktop_window.cpp
)

target_include_directories(plasma-wallpaper-engine-layerclient
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${XCB_INCLUDE_DIRS}
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
        ${XCB_LIBRARIES}
)

if(HAVE_LAYERSHELL)
    # PWE_HAVE_LAYERSHELL is what gates the #include and the shell
    # configuration in wallpaper_layer.cpp. LayerShellQt often installs into a
    # system include directory, so __has_include(<LayerShellQt/Window>) alone
    # cannot tell us whether the library is linkable; requiring both keeps a
    # build without the package from failing at link time.
    target_link_libraries(plasma-wallpaper-engine-layerclient
        PRIVATE
            LayerShellQt::Interface
    )
    target_compile_definitions(plasma-wallpaper-engine-layerclient
        PRIVATE
            PWE_HAVE_LAYERSHELL=1
    )
endif()

install(TARGETS plasma-wallpaper-engine-layerclient DESTINATION ${KDE_INSTALL_BINDIR})
