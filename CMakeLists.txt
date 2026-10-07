cmake_minimum_required(VERSION 3.20)
project(plasma-wallpaper-engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Emit compile_commands.json so editors and clangd can resolve the Qt6
# headers. Qt6 headers are not on the default include path (they live under
# /usr/include/qt6/QtGui and friends), so without this every #include <QImage>
# and friends is reported as unresolved by the editor even though the
# compiler is perfectly happy.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------- Core deps (always required) ----------
find_package(Qt6 QUIET COMPONENTS Core Gui Quick DBus Network Widgets QuickControls2)
if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "Qt6 is required but was not found. On Ubuntu install: qt6-base-dev qt6-tools-dev-tools")
endif()
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
# Detected here (not in the daemon subdirectory) so the test registration
# below can see it. Directory-scoped set() inside daemon/CMakeLists.txt does
# not reliably propagate up to this scope, and the x11_ewmh_coverage test
# must only be registered when that backend was actually compiled in.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(TOPLEVEL_XCB QUIET xcb)
endif()

add_subdirectory(daemon)
add_subdirectory(viewer)
# Optional wlroots layer-shell wallpaper client (needs LayerShellQt).
add_subdirectory(layerclient)

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

    # Pause gate headless test (built in the daemon tree).
    add_test(NAME pause_gate_headless
             COMMAND ${CMAKE_CURRENT_BINARY_DIR}/daemon/pause_gate_test)
    set_tests_properties(pause_gate_headless PROPERTIES TIMEOUT 60)

    # End-to-end GPU scene regression: the 72-wallpaper PNG suite renders on
    # the CPU painter only, so nothing else in CI can catch a broken Vulkan
    # path. This one loads the committed fixture scene through the real GPU
    # path and asserts the exported DmaBuf holds a frame, not a flat fill.
    if(TARGET gpu_scene_e2e AND
       EXISTS "${REGRESSION_DIR}/fixture_test_scene/scene.pkg")
        add_test(NAME gpu_scene_e2e
                 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/daemon/gpu_scene_e2e
                         ${REGRESSION_DIR}/fixture_test_scene/scene.pkg)
        set_tests_properties(gpu_scene_e2e PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            SKIP_RETURN_CODE "77;78"
        )
    endif()

    # X11 EWMH coverage math: union-area sweep, exercised headlessly. Built
    # in the daemon tree and only registered when the X11 backend was
    # actually detected, so it does not fail on a pure-Wayland build.
    if(TOPLEVEL_XCB_FOUND AND TARGET x11_ewmh_coverage_test)
        add_test(NAME x11_ewmh_coverage
                 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/daemon/x11_ewmh_coverage_test)
        set_tests_properties(x11_ewmh_coverage PROPERTIES TIMEOUT 60)
    endif()

    # GPU deform probe: offscreen Vulkan render of a mixed plain+deform
    # scene, boundary compared against CPU deform math. Skips (exit 77) on
    # GPU-less machines; SKIP_RETURN_CODE keeps ctest green there.
    add_test(NAME gpu_deform_probe
             COMMAND ${CMAKE_CURRENT_BINARY_DIR}/daemon/gpu_deform_probe)
    set_tests_properties(gpu_deform_probe PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        SKIP_RETURN_CODE 77
    )

    # blitIntoShared DmaBuf probe: the live wallpaper client consumes the
    # exported DmaBuf, not readback(), so this covers the one GPU path the
    # deform probe cannot see. Skips (exit 77) on GPU-less machines.
    if(TARGET blit_shared_probe)
        add_test(NAME blit_shared_probe
                 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/daemon/blit_shared_probe)
        # Measured 32s on a discrete GPU and 76ms under lavapipe; the spread is
        # dmabuf allocation, so leave real headroom for slower CI hosts.
        set_tests_properties(blit_shared_probe PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            SKIP_RETURN_CODE "77;78"
        )
    endif()

    # GPU quad placement probe: several textured quads at known, distinct
    # screen positions, asserting each lands in its own rect. This is the
    # only probe that can see a degenerate per-instance stream, which the
    # full-screen layer sets in the other two probes cannot. Skips (exit 77)
    # on GPU-less machines.
    if(TARGET gpu_quad_placement_probe)
        add_test(NAME gpu_quad_placement_probe
                 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/daemon/gpu_quad_placement_probe)
        set_tests_properties(gpu_quad_placement_probe PROPERTIES
            TIMEOUT 120
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            SKIP_RETURN_CODE 77
        )
    endif()

    # Self-contained fixture regression: renders a committed deterministic
    # scene.pkg through the verifier and PNG-diffs it against a committed
    # baseline. Runs everywhere (no Steam library, no GPU required) so CI
    # exercises the verifier + render + compare pipeline on every push.
    if(TARGET plasma-wallpaper-engine-verifier AND
       EXISTS "${REGRESSION_DIR}/fixture_test_scene/scene.pkg" AND
       EXISTS "${REGRESSION_DIR}/fixture_baseline/fixture_test_scene.png")
        set(FIXTURE_OUT "${CMAKE_BINARY_DIR}/tests/fixture-output")
        add_test(
            NAME fixture_regression
            COMMAND ${CMAKE_COMMAND}
                    -DFIXTURE_PKG_DIR=${REGRESSION_DIR}/fixture_test_scene
                    -DBASELINE_DIR=${REGRESSION_DIR}/fixture_baseline
                    -DVERIFIER_BIN=$<TARGET_FILE:plasma-wallpaper-engine-verifier>
                    -DOUT_DIR=${FIXTURE_OUT}
                    -DPYTHON=${PYTHON3_EXECUTABLE}
                    -DCOMPARE_SCRIPT=${REGRESSION_DIR}/compare_images.py
                    -P ${CMAKE_SOURCE_DIR}/tests/regression/run_fixture_test.cmake
        )
        set_tests_properties(fixture_regression PROPERTIES
            TIMEOUT 120
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PYTHONUNBUFFERED=1"
        )
        message(STATUS "Fixture regression test: ENABLED")
    else()
        message(STATUS "Fixture regression test: SKIPPED (missing fixture or verifier)")
    endif()

    # Hyprland IPC backend connection test. Integration-only: the test binary
    # exits 77 when no Hyprland instance is running, so non-Hyprland desktops
    # (XFCE, labwc) report the test as skipped instead of failed.
    add_executable(hyprland_backend_test tests/hyprland_backend_test.cpp)
    target_compile_features(hyprland_backend_test PRIVATE cxx_std_17)
    if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/hyprland_backend_test")
        add_test(NAME hyprland_backend_connect
                 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/hyprland_backend_test)
        set_tests_properties(hyprland_backend_connect PROPERTIES
            LABELS "hyprland;integration"
            TIMEOUT 30
            SKIP_RETURN_CODE 77
        )
    endif()
endif()

# Lightweight doc/validity checks that don't require a full build.
find_program(VERIFY_DBUS_INTERFACE_SCRIPT "verify_dbus_interface.sh" PATHS "${CMAKE_SOURCE_DIR}/scripts")
if(VERIFY_DBUS_INTERFACE_SCRIPT)
    add_custom_target(check_dbus_interface
        COMMAND ${VERIFY_DBUS_INTERFACE_SCRIPT}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Validating D-Bus interface XML against Qt tooling (if available)"
    )
endif()







