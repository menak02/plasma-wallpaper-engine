cmake_minimum_required(VERSION 3.20)
project(plasma-wallpaper-engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------- Core deps (always required) ----------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick DBus Network Widgets)
find_package(Vulkan REQUIRED)

# ---------- Optional: KDE/Plasma plugin ----------
# Only needed when building the Plasma wallpaper plugin.
# The daemon and viewer are standalone — no KDE deps required.
option(ENABLE_PLASMA_PLUGIN "Build KDE Plasma wallpaper plugin (requires ECM, KF6, Plasma)" OFF)

set(HAVE_PLASMA FALSE)
if(ENABLE_PLASMA_PLUGIN)
    find_package(ECM 6.0.0 QUIET NO_MODULE)
    if(ECM_FOUND)
        set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_MODULE_PATH})
        include(KDEInstallDirs)
        include(KDECMakeSettings)
        include(KDECompilerSettings NO_POLICY_SCOPE)
        find_package(Plasma QUIET)
        find_package(KF6 QUIET COMPONENTS CoreAddons Package Config)
        if(Plasma_FOUND AND KF6_FOUND)
            set(HAVE_PLASMA TRUE)
            message(STATUS "Plasma plugin: ENABLED (Plasma + KF6 found)")
        else()
            message(WARNING "Plasma plugin: DISABLED (Plasma or KF6 not found)")
        endif()
    else()
        message(WARNING "Plasma plugin: DISABLED (ECM not found)")
    endif()
endif()

if(NOT HAVE_PLASMA)
    message(STATUS "Plasma plugin: DISABLED (use -DENABLE_PLASMA_PLUGIN=ON to enable)")
    # Provide stub KDEInstallDirs vars so install() calls in subdirs don't error
    set(KDE_INSTALL_LIBEXECDIR "${CMAKE_INSTALL_PREFIX}/libexec" CACHE PATH "")
    set(KDE_INSTALL_BINDIR "${CMAKE_INSTALL_PREFIX}/bin" CACHE PATH "")
    set(KDE_INSTALL_QMLDIR "${CMAKE_INSTALL_PREFIX}/lib/qt6/qml" CACHE PATH "")
    set(KDE_INSTALL_DATADIR "${CMAKE_INSTALL_PREFIX}/share" CACHE PATH "")
endif()

# ---------- Build targets ----------
add_subdirectory(daemon)
add_subdirectory(viewer)

if(HAVE_PLASMA)
    add_subdirectory(plugin)
endif()
