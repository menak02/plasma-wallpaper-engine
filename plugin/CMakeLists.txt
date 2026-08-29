qt_add_qml_module(plasma_wallpaper_engine_plugin
    URI org.antigravity.plasma.wallpaperengine
    VERSION 1.0
    SOURCES
        src/texture_item.cpp
        src/texture_item.h
)

target_link_libraries(plasma_wallpaper_engine_plugin
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
)
target_include_directories(plasma_wallpaper_engine_plugin PRIVATE src)

install(TARGETS plasma_wallpaper_engine_plugin DESTINATION ${KDE_INSTALL_QMLDIR}/org/antigravity/plasma/wallpaperengine)
install(DIRECTORY package/ DESTINATION ${KDE_INSTALL_DATADIR}/plasma/wallpapers/org.antigravity.plasma.wallpaperengine)
