set(CMAKE_AUTOMOC ON)

add_executable(plasma-wallpaper-engine-viewer
    src/main.cpp
    src/viewer_window.h
    src/viewer_window.cpp
)

target_include_directories(plasma-wallpaper-engine-viewer
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(plasma-wallpaper-engine-viewer
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::Qml
        Qt6::QuickControls2
        Qt6::DBus
        Qt6::Widgets
)
