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

# ---------- Regression tests ----------
# PNG-diff comparison of the batch verifier output against a reference
# baseline. Baseline lives in tests/regression/baseline/ (one PNG per
# wallpaper ID, generated from a verified 55-pass/17-review/0-fail run).
# Prereq: the verifier must have produced fresh PNGs in
# ~/.wallpaper-engine-verifier-output/ before running ctest.
option(ENABLE_REGRESSION_TESTS "Enable PNG-diff regression tests (ctest)" ON)

if(ENABLE_REGRESSION_TESTS)
    enable_testing()

    set(REGRESSION_DIR "${CMAKE_SOURCE_DIR}/tests/regression")
    set(VERIFIER_OUTPUT_DIR "$ENV{HOME}/.wallpaper-engine-verifier-output")

    find_program(PYTHON3_EXECUTABLE NAMES python3 REQUIRED)

    if(EXISTS "${REGRESSION_DIR}/baseline" AND
       EXISTS "${VERIFIER_OUTPUT_DIR}")
        add_test(
            NAME png_regression_batch
            COMMAND ${PYTHON3_EXECUTABLE}
                    ${REGRESSION_DIR}/compare_images.py
                    ${REGRESSION_DIR}/baseline
                    ${VERIFIER_OUTPUT_DIR}
        )
        set_tests_properties(png_regression_batch PROPERTIES
            TIMEOUT 900
            ENVIRONMENT "PYTHONUNBUFFERED=1"
        )
        message(STATUS "Regression tests: ENABLED (baseline: ${REGRESSION_DIR}/baseline)")
    else()
        message(STATUS "Regression tests: SKIPPED (missing baseline dir or verifier output)")
    endif()
endif()
