// labwc / wlroots (wlr-ipc-unstable-v2) backend for the pause gate.
//
// ---------------------------------------------------------------------------
// WHY THESE STRUCTS ARE HAND-WRITTEN
// ---------------------------------------------------------------------------
// libwayland-client is present but wlroots, a build-time wayland-scanner run,
// wlrctl and swaymsg are not, so wlr-foreign-toplevel-management,
// wlr-output-management and xdg-output are spoken by hand:
//
//   * opcodes, request/event order and argument signatures were derived from
//     the protocol XML, where the order of <request>/<event> elements IS the
//     wire order (opcode == index in that list);
//   * every `struct wl_interface` / `struct wl_message` table below mirrors
//     byte-for-byte what wayland-scanner emits for the same XML. libwayland
//     needs these tables for two things that cannot be bypassed:
//     wl_proxy_marshal_flags() reads proxy->interface->methods[opcode]
//     .signature to type the varargs, and the event demarshaller reads
//     interface->events[opcode].signature to unpack each incoming event.
//     A wrong signature silently corrupts the unpack, so the signature
//     strings are load-bearing, not documentation.
//
// LONG-TERM MAINTAINABLE PATH (prefer this over hand-editing below):
//   wayland-scanner private-code  wlr-foreign-toplevel-management-unstable-v1.xml
//   wayland-scanner private-code  wlr-output-management-unstable-v1.xml
//   wayland-scanner private-code  xdg-output-unstable-v1.xml
//   wayland-scanner client-header <each of the above>
// ...then link the generated code and delete the tables and the listeners in
// this file. The XMLs live at gitlab.freedesktop.org/wlroots/wlroots
// (protocol/) and gitlab.freedesktop.org/xdg/xdg-protocols
// (staging/xdg-output/).
//
// SAFETY RULES OBSERVED HERE (the daemon must never crash):
//   * every wl_proxy_marshal_flags() result is null-checked;
//   * every listener callback tolerates a null user_data and a null proxy;
//   * update() only pumps with a zero timeout, so it never blocks and stays
//     safe on a dead socket (wl_display_dispatch_timeout when CMake confirms
//     the symbol links, otherwise an explicit poll() fallback);
//   * once the display reports an error the backend latches to a dead state,
//     clears all cached state and stops touching the proxies.
//
// BUILD REQUIREMENT (not applied here: CMakeLists.txt belongs to another
// agent): this file needs `pkg-config --cflags/--libs wayland-client` and
// libwayland-client on the link line. Guard it with a CMake conditional such
// as `pkg_check_modules(WAYLAND_CLIENT wayland-client)` if the daemon is also
// expected to build where the Wayland client dev package is absent.
// ---------------------------------------------------------------------------

#include "labwc_backend.h"

#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <poll.h>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// ===========================================================================
// Opcodes (index in the XML <request>/<event> list == wire opcode)
// ===========================================================================

// wl_registry (core wayland.xml)
constexpr uint32_t kRegistryBind = 0;

// zwlr_foreign_toplevel_manager_v1
//   requests: stop
//   events:   toplevel, finished
constexpr uint32_t kToplevelManagerStop = 0;

// zwlr_foreign_toplevel_handle_v1
//   requests: set_maximized, unset_maximized, set_minimized,
//             unset_minimized, activate, close, set_rectangle, destroy,
//             set_fullscreen, unset_fullscreen
//   events:   title, app_id, output_enter, output_leave, state, done,
//             closed, parent
constexpr uint32_t kToplevelHandleDestroy = 7;

// zwlr_output_manager_v1
//   requests: create_configuration, stop
//   events:   head, done, finished
constexpr uint32_t kOutputManagerStop = 1;

// zxdg_output_manager_v1
//   requests: destroy, get_xdg_output
constexpr uint32_t kXdgOutputManagerGetXdgOutput = 1;

// zwlr_output_head_v1 / zwlr_output_mode_v1 have only `release`, which is never
// needed here: wl_display_disconnect() frees every proxy, so no request is
// marshalled on them.

// zwlr_foreign_toplevel_handle_v1.state values.
constexpr uint32_t kStateMaximized = 0;
constexpr uint32_t kStateMinimized = 1;
constexpr uint32_t kStateActivated = 2;
constexpr uint32_t kStateFullscreen = 3;

// wl_output.mode flags.
constexpr uint32_t kWlOutputModeCurrent = 0x1;

// Fixed-point 1.0, the default output scale.
constexpr int32_t kFixedOne = 1 << 16;

// Interface names, as they appear on the wire.
constexpr const char* kIfaceToplevelManager = "zwlr_foreign_toplevel_manager_v1";
constexpr const char* kIfaceToplevelHandle = "zwlr_foreign_toplevel_handle_v1";
constexpr const char* kIfaceOutputManager = "zwlr_output_manager_v1";
constexpr const char* kIfaceOutputHead = "zwlr_output_head_v1";
constexpr const char* kIfaceOutputMode = "zwlr_output_mode_v1";
constexpr const char* kIfaceXdgOutputManager = "zxdg_output_manager_v1";
constexpr const char* kIfaceXdgOutput = "zxdg_output_v1";
constexpr const char* kIfaceWlOutput = "wl_output";

// ===========================================================================
// Hand-rolled interface tables (see the file header)
// ===========================================================================

extern const struct wl_interface labwc_toplevel_manager_interface;
extern const struct wl_interface labwc_toplevel_handle_interface;
extern const struct wl_interface labwc_output_manager_interface;
extern const struct wl_interface labwc_output_head_interface;
extern const struct wl_interface labwc_output_mode_interface;
extern const struct wl_interface labwc_xdg_output_manager_interface;
extern const struct wl_interface labwc_xdg_output_interface;

// Type table for wlr_foreign_toplevel_management_unstable_v1.
static const struct wl_interface* toplevel_types[] = {
    nullptr,                                        // 0  (no interfaces)
    &labwc_toplevel_handle_interface,                // 1  toplevel handle
    nullptr,                                        // 2  wl_seat (activate)
    nullptr,                                        // 3  wl_surface (set_rectangle)
    nullptr, nullptr, nullptr, nullptr,              // 4-7 unused
    &wl_output_interface,                           // 8  set_fullscreen output
    &wl_output_interface,                           // 9  output_enter
    &wl_output_interface,                           // 10 output_leave
    &labwc_toplevel_handle_interface,                // 11 parent
};

static const struct wl_message toplevel_manager_requests[] = {
    {"stop", "", toplevel_types + 0},
};

static const struct wl_message toplevel_manager_events[] = {
    {"toplevel", "n", toplevel_types + 1},
    {"finished", "", toplevel_types + 0},
};

const struct wl_interface labwc_toplevel_manager_interface = {
    kIfaceToplevelManager, 3,
    1, toplevel_manager_requests,
    2, toplevel_manager_events,
};

static const struct wl_message toplevel_handle_requests[] = {
    {"set_maximized", "", toplevel_types + 0},
    {"unset_maximized", "", toplevel_types + 0},
    {"set_minimized", "", toplevel_types + 0},
    {"unset_minimized", "", toplevel_types + 0},
    {"activate", "o", toplevel_types + 2},
    {"close", "", toplevel_types + 0},
    {"set_rectangle", "oiiii", toplevel_types + 3},
    {"destroy", "", toplevel_types + 0},
    {"set_fullscreen", "2?o", toplevel_types + 8},
    {"unset_fullscreen", "2", toplevel_types + 0},
};

static const struct wl_message toplevel_handle_events[] = {
    {"title", "s", toplevel_types + 0},
    {"app_id", "s", toplevel_types + 0},
    {"output_enter", "o", toplevel_types + 9},
    {"output_leave", "o", toplevel_types + 10},
    {"state", "a", toplevel_types + 0},
    {"done", "", toplevel_types + 0},
    {"closed", "", toplevel_types + 0},
    {"parent", "3?o", toplevel_types + 11},
};

const struct wl_interface labwc_toplevel_handle_interface = {
    kIfaceToplevelHandle, 3,
    10, toplevel_handle_requests,
    8, toplevel_handle_events,
};

// Type table for wlr_output_management_unstable_v1.
static const struct wl_interface* output_types[] = {
    nullptr,                                        // 0
    nullptr, nullptr, nullptr,                       // 1-3
    nullptr,                                        // 4  configuration (unused)
    &labwc_output_head_interface,                    // 5
    &labwc_output_mode_interface,                    // 6
    &labwc_output_mode_interface,                    // 7
    nullptr,                                        // 8  configuration_head (unused)
    &labwc_output_head_interface,                    // 9
    &labwc_output_head_interface,                    // 10
    &labwc_output_mode_interface,                    // 11
};

static const struct wl_message output_manager_requests[] = {
    {"create_configuration", "nu", output_types + 4},
    {"stop", "", output_types + 0},
};

static const struct wl_message output_manager_events[] = {
    {"head", "n", output_types + 5},
    {"done", "u", output_types + 0},
    {"finished", "", output_types + 0},
};

const struct wl_interface labwc_output_manager_interface = {
    kIfaceOutputManager, 4,
    2, output_manager_requests,
    3, output_manager_events,
};

static const struct wl_message output_head_requests[] = {
    {"release", "3", output_types + 0},
};

static const struct wl_message output_head_events[] = {
    {"name", "s", output_types + 0},
    {"description", "s", output_types + 0},
    {"physical_size", "ii", output_types + 0},
    {"mode", "n", output_types + 6},
    {"enabled", "i", output_types + 0},
    {"current_mode", "o", output_types + 7},
    {"position", "ii", output_types + 0},
    {"transform", "i", output_types + 0},
    {"scale", "f", output_types + 0},
    {"finished", "", output_types + 0},
    {"make", "2s", output_types + 0},
    {"model", "2s", output_types + 0},
    {"serial_number", "2s", output_types + 0},
    {"adaptive_sync", "4u", output_types + 0},
};

const struct wl_interface labwc_output_head_interface = {
    kIfaceOutputHead, 4,
    1, output_head_requests,
    14, output_head_events,
};

static const struct wl_message output_mode_requests[] = {
    {"release", "3", output_types + 0},
};

static const struct wl_message output_mode_events[] = {
    {"size", "ii", output_types + 0},
    {"refresh", "i", output_types + 0},
    {"preferred", "", output_types + 0},
    {"finished", "", output_types + 0},
};

const struct wl_interface labwc_output_mode_interface = {
    kIfaceOutputMode, 3,
    1, output_mode_requests,
    4, output_mode_events,
};

// Type table for xdg_output_unstable_v1.
static const struct wl_interface* xdg_output_types[] = {
    nullptr,                                        // 0
    nullptr,                                        // 1
    &labwc_xdg_output_interface,                     // 2
    &wl_output_interface,                           // 3
};

static const struct wl_message xdg_output_manager_requests[] = {
    {"destroy", "", xdg_output_types + 0},
    {"get_xdg_output", "no", xdg_output_types + 2},
};

const struct wl_interface labwc_xdg_output_manager_interface = {
    kIfaceXdgOutputManager, 3,
    2, xdg_output_manager_requests,
    0, nullptr,
};

static const struct wl_message xdg_output_requests[] = {
    {"destroy", "", xdg_output_types + 0},
};

static const struct wl_message xdg_output_events[] = {
    {"logical_position", "ii", xdg_output_types + 0},
    {"logical_size", "ii", xdg_output_types + 0},
    {"done", "", xdg_output_types + 0},
    {"name", "2s", xdg_output_types + 0},
    {"description", "2s", xdg_output_types + 0},
};

const struct wl_interface labwc_xdg_output_interface = {
    kIfaceXdgOutput, 3,
    1, xdg_output_requests,
    5, xdg_output_events,
};

// ===========================================================================
// Listeners: one function pointer per event, in XML order, indexed by opcode.
// libwayland (>= 1.22) no longer prefixes a `struct wl_listener destroy`, so
// index N of these arrays is exactly event opcode N.
// ===========================================================================

struct LabwcRegistryListener {
    void (*global)(void* data, struct wl_registry* registry, uint32_t name,
                   const char* interface, uint32_t version);
    void (*global_remove)(void* data, struct wl_registry* registry, uint32_t name);
};

struct LabwcToplevelManagerListener {
    void (*toplevel)(void* data, struct wl_proxy* manager, struct wl_proxy* toplevel);
    void (*finished)(void* data, struct wl_proxy* manager);
};

struct LabwcToplevelHandleListener {
    void (*title)(void* data, struct wl_proxy* handle, const char* title);
    void (*app_id)(void* data, struct wl_proxy* handle, const char* app_id);
    void (*output_enter)(void* data, struct wl_proxy* handle, struct wl_proxy* output);
    void (*output_leave)(void* data, struct wl_proxy* handle, struct wl_proxy* output);
    void (*state)(void* data, struct wl_proxy* handle, struct wl_array* state);
    void (*done)(void* data, struct wl_proxy* handle);
    void (*closed)(void* data, struct wl_proxy* handle);
    void (*parent)(void* data, struct wl_proxy* handle, struct wl_proxy* parent);
};

struct LabwcOutputManagerListener {
    void (*head)(void* data, struct wl_proxy* manager, struct wl_proxy* head);
    void (*done)(void* data, struct wl_proxy* manager, uint32_t serial);
    void (*finished)(void* data, struct wl_proxy* manager);
};

struct LabwcOutputHeadListener {
    void (*name)(void* data, struct wl_proxy* head, const char* name);
    void (*description)(void* data, struct wl_proxy* head, const char* description);
    void (*physical_size)(void* data, struct wl_proxy* head, int32_t width, int32_t height);
    void (*mode)(void* data, struct wl_proxy* head, struct wl_proxy* mode);
    void (*enabled)(void* data, struct wl_proxy* head, int32_t enabled);
    void (*current_mode)(void* data, struct wl_proxy* head, struct wl_proxy* mode);
    void (*position)(void* data, struct wl_proxy* head, int32_t x, int32_t y);
    void (*transform)(void* data, struct wl_proxy* head, int32_t transform);
    void (*scale)(void* data, struct wl_proxy* head, int32_t scale);
    void (*finished)(void* data, struct wl_proxy* head);
    void (*make)(void* data, struct wl_proxy* head, const char* make);
    void (*model)(void* data, struct wl_proxy* head, const char* model);
    void (*serial_number)(void* data, struct wl_proxy* head, const char* serial_number);
    void (*adaptive_sync)(void* data, struct wl_proxy* head, uint32_t state);
};

struct LabwcOutputModeListener {
    void (*size)(void* data, struct wl_proxy* mode, int32_t width, int32_t height);
    void (*refresh)(void* data, struct wl_proxy* mode, int32_t refresh);
    void (*preferred)(void* data, struct wl_proxy* mode);
    void (*finished)(void* data, struct wl_proxy* mode);
};

struct LabwcXdgOutputListener {
    void (*logical_position)(void* data, struct wl_proxy* output, int32_t x, int32_t y);
    void (*logical_size)(void* data, struct wl_proxy* output, int32_t width, int32_t height);
    void (*done)(void* data, struct wl_proxy* output);
    void (*name)(void* data, struct wl_proxy* output, const char* name);
    void (*description)(void* data, struct wl_proxy* output, const char* description);
};

// wl_output is a core interface, but its C type is opaque in
// wayland-client-protocol.h, so the listener is declared with wl_proxy just
// like the hand-rolled ones. Event order matches wayland.xml:
// geometry, mode, done, scale, name, description.
struct LabwcWlOutputListener {
    void (*geometry)(void* data, struct wl_proxy* output, int32_t x, int32_t y,
                     int32_t physical_width, int32_t physical_height,
                     int32_t subpixel, const char* make, const char* model,
                     int32_t transform);
    void (*mode)(void* data, struct wl_proxy* output, uint32_t flags,
                 int32_t width, int32_t height, int32_t refresh);
    void (*done)(void* data, struct wl_proxy* output);
    void (*scale)(void* data, struct wl_proxy* output, int32_t factor);
    void (*name)(void* data, struct wl_proxy* output, const char* name);
    void (*description)(void* data, struct wl_proxy* output, const char* description);
};

// ===========================================================================
// Small helpers
// ===========================================================================

uint32_t clampVersion(uint32_t advertised, uint32_t wanted) {
    return advertised < wanted ? advertised : wanted;
}

/** True for the wl_output.transform values that swap width/height:
    90, 270, flipped_90, flipped_270. */
bool transformSwapsAxes(int32_t transform) {
    return transform == 1 || transform == 3 || transform == 5 || transform == 7;
}

/** mode size / scale, axes swapped when the transform says so. Matches what
    wlroots reports as the output's logical size. */
bool logicalSize(int32_t modeWidth, int32_t modeHeight, int32_t scale,
                 int32_t transform, int32_t* outWidth, int32_t* outHeight) {
    if (modeWidth <= 0 || modeHeight <= 0) {
        return false;
    }
    int32_t width = modeWidth;
    int32_t height = modeHeight;
    if (transformSwapsAxes(transform)) {
        std::swap(width, height);
    }
    if (scale <= 0) {
        scale = kFixedOne;
    }
    width = static_cast<int32_t>((static_cast<int64_t>(width) * kFixedOne) / scale);
    height = static_cast<int32_t>((static_cast<int64_t>(height) * kFixedOne) / scale);
    if (width <= 0 || height <= 0) {
        return false;
    }
    *outWidth = width;
    *outHeight = height;
    return true;
}

} // namespace

namespace WallpaperEngine::Scene {

// ===========================================================================
// Impl: all Wayland state lives here so the header stays wayland-free.
// ===========================================================================

struct LabwcBackend::Impl {
    struct Mode {
        int32_t width = 0;
        int32_t height = 0;
    };

    struct Head {
        struct wl_proxy* proxy = nullptr;
        std::string name;
        bool enabled = true;
        int32_t x = 0;
        int32_t y = 0;
        int32_t transform = 0;
        int32_t scale = kFixedOne;
        struct wl_proxy* currentMode = nullptr;
        std::unordered_map<struct wl_proxy*, Mode> modes;
        bool finished = false;
    };

    struct WlOutputState {
        struct wl_proxy* proxy = nullptr;
        std::string name;
        int32_t x = 0;
        int32_t y = 0;
        int32_t scale = kFixedOne;
        int32_t transform = 0;
        int32_t modeWidth = 0;
        int32_t modeHeight = 0;
        bool haveMode = false;
        bool finished = false;
    };

    struct XdgOutputState {
        struct wl_proxy* proxy = nullptr;
        struct wl_proxy* wlOutput = nullptr;
        std::string name;
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = 0;
        int32_t height = 0;
        bool haveSize = false;
    };

    struct Output {
        std::string name;
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = 0;
        int32_t height = 0;
        bool enabled = true;
        /** True once width/height are usable as a coverage denominator. */
        bool haveGeometry = false;
        struct wl_proxy* head = nullptr;
        struct wl_proxy* wlOutput = nullptr;
        struct wl_proxy* xdgOutput = nullptr;
    };

    struct Toplevel {
        struct wl_proxy* proxy = nullptr;
        std::string title;
        std::string appId;
        bool maximized = false;
        bool minimized = false;
        bool activated = false;
        bool fullscreen = false;
        /** wl_output objects this toplevel is visible on (output_enter). */
        std::vector<struct wl_proxy*> outputProxies;
        /** Parent handles (protocol v3), tracked but unused by the decision. */
        std::vector<struct wl_proxy*> parents;
    };

    ~Impl() { shutdown(); }

    void shutdown() {
        if (display) {
            if (!dead) {
                // Politely stop the managers first. Objects the compositor
                // already sent `finished` for are inert and must not be used.
                if (toplevelManager && !toplevelManagerFinished) {
                    requestStop(toplevelManager, kToplevelManagerStop);
                }
                if (outputManager && !outputManagerFinished) {
                    requestStop(outputManager, kOutputManagerStop);
                }
                wl_display_flush(display);
            }
            wl_display_disconnect(display);
            display = nullptr;
        }
        registry = nullptr;
        toplevelManager = nullptr;
        outputManager = nullptr;
        xdgOutputManager = nullptr;
        outputs.clear();
        heads.clear();
        modeOwner.clear();
        wlOutputs.clear();
        xdgOutputs.clear();
        toplevels.clear();
    }

    // ---------------------------------------------------------------------
    // Registry
    // ---------------------------------------------------------------------

    void onGlobal(uint32_t name, const char* interface, uint32_t version) {
        if (!interface) {
            return;
        }
        if (std::strcmp(interface, kIfaceToplevelManager) == 0) {
            bindToplevelManager(name, version);
        } else if (std::strcmp(interface, kIfaceOutputManager) == 0) {
            bindOutputManager(name, version);
        } else if (std::strcmp(interface, kIfaceXdgOutputManager) == 0) {
            bindXdgOutputManager(name, version);
        } else if (std::strcmp(interface, kIfaceWlOutput) == 0) {
            bindWlOutput(name, version);
        }
    }

    void onGlobalRemove(uint32_t) {
        // Hot-unplug arrives as head.finished / a missing wl_output and is
        // picked up by reconcileOutputs(); the registry name is not needed.
    }

    void bindToplevelManager(uint32_t name, uint32_t version) {
        if (toplevelManager) {
            return;
        }
        // v3 is the newest that adds anything (parent); v2 adds the
        // fullscreen state this backend keys its decision on.
        const uint32_t v = clampVersion(version, 3);
        struct wl_proxy* proxy = registryBind(name, &labwc_toplevel_manager_interface, v);
        if (!proxy) {
            std::cerr << "LabwcBackend: failed to bind " << kIfaceToplevelManager << std::endl;
            return;
        }
        if (!addListener(proxy, &toplevelManagerListener)) {
            std::cerr << "LabwcBackend: failed to listen on " << kIfaceToplevelManager << std::endl;
            return;
        }
        toplevelManager = proxy;
        if (v < 2) {
            std::cerr << "LabwcBackend: " << kIfaceToplevelManager
                      << " is only version " << v
                      << "; the fullscreen state will be invisible" << std::endl;
        }
    }

    void bindOutputManager(uint32_t name, uint32_t version) {
        if (outputManager) {
            return;
        }
        // Stay at version 3: v4 only adds the adaptive_sync event.
        const uint32_t v = clampVersion(version, 3);
        struct wl_proxy* proxy = registryBind(name, &labwc_output_manager_interface, v);
        if (!proxy) {
            std::cerr << "LabwcBackend: failed to bind " << kIfaceOutputManager << std::endl;
            return;
        }
        if (!addListener(proxy, &outputManagerListener)) {
            return;
        }
        outputManager = proxy;
    }

    void bindXdgOutputManager(uint32_t name, uint32_t version) {
        if (xdgOutputManager) {
            return;
        }
        const uint32_t v = clampVersion(version, 3);
        struct wl_proxy* proxy = registryBind(name, &labwc_xdg_output_manager_interface, v);
        if (!proxy) {
            return;
        }
        xdgOutputManager = proxy;
        // The manager may arrive after the wl_output globals.
        ensureXdgOutputs();
    }

    void bindWlOutput(uint32_t name, uint32_t version) {
        // Bound so that the wl_object* a toplevel's output_enter event
        // carries is a proxy this backend already owns and can name.
        const uint32_t v = clampVersion(version, 4);
        struct wl_proxy* proxy = registryBind(name, &wl_output_interface, v);
        if (!proxy) {
            return;
        }
        WlOutputState state;
        state.proxy = proxy;
        if (!addListener(proxy, &wlOutputListener)) {
            return;
        }
        wlOutputs[proxy] = state;
        ensureXdgOutputs();
    }

    /** Creates the zxdg_output_v1 objects, skipping ones already created. */
    void ensureXdgOutputs() {
        if (!xdgOutputManager || dead) {
            return;
        }
        const uint32_t version = wl_proxy_get_version(xdgOutputManager);
        for (const auto& entry : wlOutputs) {
            const struct wl_proxy* wlOutput = entry.first;
            if (xdgOutputFor(wlOutput)) {
                continue;
            }
            // get_xdg_output signature is "no": the new_id arg consumes one
            // ignored vararg (this is exactly what wayland-scanner emits),
            // then the wl_output object. `version` is a named parameter of
            // wl_proxy_marshal_flags, not a vararg.
            struct wl_proxy* xdgOutput =
                wl_proxy_marshal_flags(xdgOutputManager, kXdgOutputManagerGetXdgOutput,
                                       &labwc_xdg_output_interface, version, 0,
                                       nullptr, wlOutput);
            if (!xdgOutput) {
                continue;
            }
            XdgOutputState state;
            state.proxy = xdgOutput;
            state.wlOutput = const_cast<struct wl_proxy*>(wlOutput);
            xdgOutputs.push_back(state);
            if (!addListener(xdgOutput, &xdgOutputListener)) {
                xdgOutputs.pop_back();
                continue;
            }
        }
    }

    XdgOutputState* xdgOutputFor(const struct wl_proxy* wlOutput) {
        if (!wlOutput) {
            return nullptr;
        }
        for (auto& entry : xdgOutputs) {
            if (entry.wlOutput == wlOutput) {
                return &entry;
            }
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------
    // wl_output events (identity + geometry fallback)
    // ---------------------------------------------------------------------

    void onWlOutputGeometry(struct wl_proxy* output, int32_t x, int32_t y,
                            int32_t transform) {
        auto it = wlOutputs.find(output);
        if (it == wlOutputs.end()) {
            return;
        }
        it->second.x = x;
        it->second.y = y;
        it->second.transform = transform;
        reconcileOutputs();
    }

    void onWlOutputMode(struct wl_proxy* output, uint32_t flags, int32_t width, int32_t height) {
        auto it = wlOutputs.find(output);
        if (it == wlOutputs.end()) {
            return;
        }
        if (flags & kWlOutputModeCurrent) {
            it->second.modeWidth = width;
            it->second.modeHeight = height;
            it->second.haveMode = true;
            reconcileOutputs();
        }
    }

    void onWlOutputScale(struct wl_proxy* output, int32_t factor) {
        auto it = wlOutputs.find(output);
        if (it == wlOutputs.end()) {
            return;
        }
        it->second.scale = factor > 0 ? factor : kFixedOne;
        reconcileOutputs();
    }

    void onWlOutputName(struct wl_proxy* output, const char* name) {
        auto it = wlOutputs.find(output);
        if (it == wlOutputs.end() || !name) {
            return;
        }
        it->second.name = name;
        reconcileOutputs();
    }

    // ---------------------------------------------------------------------
    // zxdg_output_v1 (authoritative logical geometry)
    // ---------------------------------------------------------------------

    void onXdgLogicalPosition(struct wl_proxy* out, int32_t x, int32_t y) {
        XdgOutputState* s = findXdgOutput(out);
        if (!s) return;
        s->x = x;
        s->y = y;
        reconcileOutputs();
    }

    void onXdgLogicalSize(struct wl_proxy* out, int32_t width, int32_t height) {
        XdgOutputState* s = findXdgOutput(out);
        if (!s) return;
        s->width = width;
        s->height = height;
        s->haveSize = width > 0 && height > 0;
        reconcileOutputs();
    }

    void onXdgName(struct wl_proxy* out, const char* name) {
        XdgOutputState* s = findXdgOutput(out);
        if (!s || !name) return;
        s->name = name;
        reconcileOutputs();
    }

    XdgOutputState* findXdgOutput(struct wl_proxy* proxy) {
        if (!proxy) {
            return nullptr;
        }
        for (auto& entry : xdgOutputs) {
            if (entry.proxy == proxy) {
                return &entry;
            }
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------
    // zwlr_output_manager_v1 / head / mode
    // ---------------------------------------------------------------------

    void onHead(struct wl_proxy* head) {
        if (!head) {
            return;
        }
        Head info;
        info.proxy = head;
        if (!addListener(head, &outputHeadListener)) {
            return;
        }
        heads[head] = info;
    }

    void onOutputManagerDone() {
        reconcileOutputs();
    }

    void onOutputManagerFinished() {
        outputManagerFinished = true;
        outputManager = nullptr;
        heads.clear();
        modeOwner.clear();
        reconcileOutputs();
    }

    void onHeadName(struct wl_proxy* head, const char* name) {
        auto it = heads.find(head);
        if (it == heads.end() || !name) return;
        it->second.name = name;
    }

    void onHeadEnabled(struct wl_proxy* head, int32_t enabled) {
        auto it = heads.find(head);
        if (it == heads.end()) return;
        it->second.enabled = enabled != 0;
    }

    void onHeadPosition(struct wl_proxy* head, int32_t x, int32_t y) {
        auto it = heads.find(head);
        if (it == heads.end()) return;
        it->second.x = x;
        it->second.y = y;
    }

    void onHeadTransform(struct wl_proxy* head, int32_t transform) {
        auto it = heads.find(head);
        if (it == heads.end()) return;
        it->second.transform = transform;
    }

    void onHeadScale(struct wl_proxy* head, int32_t scale) {
        auto it = heads.find(head);
        if (it == heads.end()) return;
        it->second.scale = scale > 0 ? scale : kFixedOne;
    }

    void onHeadMode(struct wl_proxy* head, struct wl_proxy* mode) {
        if (!head || !mode) return;
        auto it = heads.find(head);
        if (it == heads.end()) return;
        if (!addListener(mode, &outputModeListener)) {
            return;
        }
        it->second.modes[mode] = Mode{};
        modeOwner[mode] = head;
    }

    void onHeadCurrentMode(struct wl_proxy* head, struct wl_proxy* mode) {
        auto it = heads.find(head);
        if (it == heads.end()) return;
        it->second.currentMode = mode;
    }

    void onHeadFinished(struct wl_proxy* head) {
        auto it = heads.find(head);
        if (it == heads.end()) return;
        it->second.finished = true;
        reconcileOutputs();
    }

    void onModeSize(struct wl_proxy* mode, int32_t width, int32_t height) {
        auto owner = modeOwner.find(mode);
        if (owner == modeOwner.end()) return;
        auto head = heads.find(owner->second);
        if (head == heads.end()) return;
        auto m = head->second.modes.find(mode);
        if (m == head->second.modes.end()) return;
        m->second.width = width;
        m->second.height = height;
        reconcileOutputs();
    }

    void onModeFinished(struct wl_proxy* mode) {
        auto owner = modeOwner.find(mode);
        if (owner != modeOwner.end()) {
            auto head = heads.find(owner->second);
            if (head != heads.end()) {
                head->second.modes.erase(mode);
                if (head->second.currentMode == mode) {
                    head->second.currentMode = nullptr;
                }
            }
            modeOwner.erase(owner);
        }
        reconcileOutputs();
    }

    // ---------------------------------------------------------------------
    // zwlr_foreign_toplevel_manager_v1 / handle
    // ---------------------------------------------------------------------

    void onToplevel(struct wl_proxy* toplevel) {
        if (!toplevel) return;
        if (!addListener(toplevel, &toplevelHandleListener)) {
            return;
        }
        Toplevel info;
        info.proxy = toplevel;
        toplevels[toplevel] = info;
    }

    void onToplevelManagerFinished() {
        toplevelManagerFinished = true;
        toplevelManager = nullptr;
    }

    void onToplevelTitle(struct wl_proxy* handle, const char* title) {
        auto it = toplevels.find(handle);
        if (it == toplevels.end() || !title) return;
        it->second.title = title;
    }

    void onToplevelAppId(struct wl_proxy* handle, const char* appId) {
        auto it = toplevels.find(handle);
        if (it == toplevels.end() || !appId) return;
        it->second.appId = appId;
    }

    void onToplevelOutputEnter(struct wl_proxy* handle, struct wl_proxy* output) {
        auto it = toplevels.find(handle);
        if (it == toplevels.end() || !output) return;
        auto& proxies = it->second.outputProxies;
        if (std::find(proxies.begin(), proxies.end(), output) == proxies.end()) {
            proxies.push_back(output);
        }
    }

    void onToplevelOutputLeave(struct wl_proxy* handle, struct wl_proxy* output) {
        auto it = toplevels.find(handle);
        if (it == toplevels.end() || !output) return;
        auto& proxies = it->second.outputProxies;
        proxies.erase(std::remove(proxies.begin(), proxies.end(), output), proxies.end());
    }

    void onToplevelState(struct wl_proxy* handle, struct wl_array* array) {
        auto it = toplevels.find(handle);
        if (it == toplevels.end() || !array) return;
        Toplevel& t = it->second;
        t.maximized = false;
        t.minimized = false;
        t.activated = false;
        t.fullscreen = false;
        if (array->size < sizeof(uint32_t)) {
            return;
        }
        const uint32_t* states = static_cast<const uint32_t*>(array->data);
        const size_t count = array->size / sizeof(uint32_t);
        for (size_t i = 0; i < count; ++i) {
            switch (states[i]) {
            case kStateMaximized: t.maximized = true; break;
            case kStateMinimized: t.minimized = true; break;
            case kStateActivated: t.activated = true; break;
            case kStateFullscreen: t.fullscreen = true; break;
            default: break;
            }
        }
    }

    void onToplevelParent(struct wl_proxy* handle, struct wl_proxy* parent) {
        auto it = toplevels.find(handle);
        if (it == toplevels.end()) return;
        auto& parents = it->second.parents;
        parents.erase(std::remove(parents.begin(), parents.end(), parent), parents.end());
        if (parent) {
            parents.push_back(parent);
        }
    }

    void onToplevelClosed(struct wl_proxy* handle) {
        auto it = toplevels.find(handle);
        if (it == toplevels.end()) return;
        toplevels.erase(it);
        if (!dead && handle) {
            // The handle is inert now; releasing it finalises the object.
            requestDestroy(handle, kToplevelHandleDestroy);
        }
    }

    // ---------------------------------------------------------------------
    // State reconciliation
    // ---------------------------------------------------------------------

    /**
     * Rebuilds the output table from every source that knows about outputs.
     *
     * Geometry priority, best first:
     *   1. zxdg_output_v1 logical_position/logical_size (compositor space)
     *   2. zwlr_output_head_v1 position + current mode / scale
     *   3. wl_output geometry + current mode + scale
     * Names come from zwlr_output_head_v1.name, wl_output.name (v4) or
     * zxdg_output_v1.name, in that order of preference.
     */
    void reconcileOutputs() {
        if (dead) {
            return;
        }
        std::map<std::string, Output> next;

        // --- heads: names, positions and derived logical size ---
        for (const auto& entry : heads) {
            const Head& head = entry.second;
            if (head.finished || head.name.empty()) {
                continue;
            }
            Output& out = next[head.name];
            out.name = head.name;
            out.head = head.proxy;
            out.enabled = head.enabled;
            out.x = head.x;
            out.y = head.y;
            if (head.currentMode) {
                auto mode = head.modes.find(head.currentMode);
                if (mode != head.modes.end()) {
                    int32_t width = 0;
                    int32_t height = 0;
                    if (logicalSize(mode->second.width, mode->second.height, head.scale,
                                    head.transform, &width, &height)) {
                        out.width = width;
                        out.height = height;
                        out.haveGeometry = true;
                    }
                }
            }
        }

        // --- wl_output: output identity + a geometry fallback ---
        for (const auto& entry : wlOutputs) {
            const WlOutputState& state = entry.second;
            if (state.finished || state.name.empty()) {
                continue;
            }
            Output& out = next[state.name];
            out.name = state.name;
            out.wlOutput = state.proxy;
            if (!out.haveGeometry) {
                int32_t width = 0;
                int32_t height = 0;
                if (logicalSize(state.modeWidth, state.modeHeight, state.scale,
                                state.transform, &width, &height)) {
                    out.x = state.x;
                    out.y = state.y;
                    out.width = width;
                    out.height = height;
                    out.haveGeometry = true;
                }
            }
        }

        // --- zxdg_output: authoritative logical geometry ---
        for (const auto& entry : xdgOutputs) {
            const XdgOutputState& state = entry;
            std::string name = state.name;
            if (name.empty() && state.wlOutput) {
                auto wlIt = wlOutputs.find(state.wlOutput);
                if (wlIt != wlOutputs.end()) {
                    name = wlIt->second.name;
                }
            }
            if (name.empty()) {
                continue;
            }
            Output& out = next[name];
            out.name = name;
            out.xdgOutput = state.proxy;
            if (state.haveSize) {
                out.x = state.x;
                out.y = state.y;
                out.width = state.width;
                out.height = state.height;
                out.haveGeometry = true;
            }
        }

        outputs.swap(next);
    }

    /** Output name owning a wl_output proxy, or "" if it is not named yet. */
    std::string outputNameFor(const struct wl_proxy* wlOutput) const {
        if (!wlOutput) {
            return std::string();
        }
        for (const auto& out : outputs) {
            if (out.second.wlOutput == wlOutput) {
                return out.first;
            }
        }
        return std::string();
    }

    // ---------------------------------------------------------------------
    // Connection
    // ---------------------------------------------------------------------

    bool connect(const char* displayName) {
        display = wl_display_connect(displayName);
        if (!display) {
            std::cerr << "LabwcBackend: no Wayland display ("
                      << (displayName ? displayName : "$WAYLAND_DISPLAY")
                      << "): " << std::strerror(errno) << std::endl;
            return false;
        }
        registry = reinterpret_cast<struct wl_proxy*>(wl_display_get_registry(display));
        if (!registry) {
            std::cerr << "LabwcBackend: wl_display_get_registry failed" << std::endl;
            shutdown();
            return false;
        }
        if (!addListener(registry, &registryListener)) {
            std::cerr << "LabwcBackend: wl_registry_add_listener failed" << std::endl;
            shutdown();
            return false;
        }
        if (wl_display_roundtrip(display) < 0) {
            std::cerr << "LabwcBackend: registry roundtrip failed" << std::endl;
            shutdown();
            return false;
        }
        if (!toplevelManager) {
            std::cerr << "LabwcBackend: " << kIfaceToplevelManager
                      << " not advertised: not a wlroots compositor"
                      << std::endl;
            shutdown();
            return false;
        }
        // Second roundtrip collects heads, their modes, the xdg_output
        // objects and every existing toplevel.
        if (wl_display_roundtrip(display) < 0) {
            std::cerr << "LabwcBackend: state roundtrip failed" << std::endl;
            shutdown();
            return false;
        }
        ensureXdgOutputs();
        reconcileOutputs();

        if (outputs.empty()) {
            std::cerr << "LabwcBackend: connected but no named outputs yet; "
                         "coverage stays off until they appear"
                      << std::endl;
        } else {
            for (const auto& out : outputs) {
                std::cerr << "LabwcBackend: output " << out.first << " "
                          << out.second.width << "x" << out.second.height << "+"
                          << out.second.x << "+" << out.second.y
                          << (out.second.enabled ? "" : " (disabled)") << std::endl;
            }
        }
        return true;
    }

    /** Non-blocking pump. Returns false once the connection is unusable. */
    bool pump() {
        if (!display || dead) {
            return false;
        }
        int ret;
#ifdef PWE_WL_NO_DISPATCH_TIMEOUT
        // wl_display_dispatch_timeout is not linkable here even though the
        // headers may declare it, so poll the display fd ourselves and
        // dispatch only when it is readable. This preserves the
        // never-blocking contract the pause gate depends on.
        ret = 0;
        for (;;) {
            struct pollfd pfd{};
            pfd.fd = wl_display_get_fd(display);
            pfd.events = POLLIN;
            const int pr = poll(&pfd, 1, 0);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ret = -1;
                break;
            }
            if (pr == 0) {
                break;  // nothing readable: non-blocking pump is done
            }
            if (wl_display_dispatch(display) < 0) {
                ret = -1;
                break;
            }
            break;
        }
#else
        // Zero timeout only, so this never blocks.
        const struct timespec zero = {0, 0};
        do {
            ret = wl_display_dispatch_timeout(display, &zero);
        } while (ret < 0 && errno == EINTR);
#endif

        while (wl_display_dispatch_pending(display) > 0) {
            // drain whatever the previous call queued
        }

        if (ret < 0 || wl_display_get_error(display) != 0) {
            markDead();
            return false;
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            markDead();
            return false;
        }
        return true;
    }

    /** Latches the backend into a safe, inert state: nothing is covered and
        nothing is marshalled again. */
    void markDead() {
        if (dead) {
            return;
        }
        dead = true;
        outputs.clear();
        heads.clear();
        modeOwner.clear();
        wlOutputs.clear();
        xdgOutputs.clear();
        toplevels.clear();
        toplevelManager = nullptr;
        outputManager = nullptr;
        xdgOutputManager = nullptr;
        std::cerr << "LabwcBackend: Wayland connection lost, pausing disabled"
                  << std::endl;
    }

    // --- marshalling helpers bound to this instance ---

    struct wl_proxy* registryBind(uint32_t name, const struct wl_interface* interface,
                                  uint32_t version) {
        if (!registry || !interface) {
            return nullptr;
        }
        // wl_registry.bind signature is "usun": name, interface name string,
        // version, new_id (value ignored, libwayland allocates the proxy).
        return wl_proxy_marshal_flags(registry, kRegistryBind, interface, version, 0,
                                      name, interface->name, version, nullptr);
    }

    void requestStop(struct wl_proxy* proxy, uint32_t opcode) {
        if (!proxy) {
            return;
        }
        // Result is intentionally ignored: on a broken connection this
        // returns NULL and latches the display error, which pump() handles.
        (void)wl_proxy_marshal_flags(proxy, opcode, nullptr,
                                     wl_proxy_get_version(proxy), 0);
    }

    /** Marshals a destructor request. WL_MARSHAL_FLAG_DESTROY frees the
        client-side proxy even when the request itself failed. */
    void requestDestroy(struct wl_proxy* proxy, uint32_t opcode) {
        if (!proxy) {
            return;
        }
        (void)wl_proxy_marshal_flags(proxy, opcode, nullptr,
                                     wl_proxy_get_version(proxy),
                                     WL_MARSHAL_FLAG_DESTROY);
    }

    bool addListener(struct wl_proxy* proxy, void* listenerStruct) {
        if (!proxy || !listenerStruct) {
            return false;
        }
        return wl_proxy_add_listener(proxy,
                                     reinterpret_cast<void (**)(void)>(listenerStruct),
                                     this) == 0;
    }

    // --- objects ---
    struct wl_display* display = nullptr;
    struct wl_proxy* registry = nullptr;
    struct wl_proxy* toplevelManager = nullptr;
    struct wl_proxy* outputManager = nullptr;
    struct wl_proxy* xdgOutputManager = nullptr;
    bool toplevelManagerFinished = false;
    bool outputManagerFinished = false;
    bool dead = false;

    std::map<std::string, Output> outputs;
    std::unordered_map<struct wl_proxy*, Head> heads;
    /** mode proxy -> owning head proxy. */
    std::unordered_map<struct wl_proxy*, struct wl_proxy*> modeOwner;
    std::unordered_map<struct wl_proxy*, WlOutputState> wlOutputs;
    std::vector<XdgOutputState> xdgOutputs;
    std::unordered_map<struct wl_proxy*, Toplevel> toplevels;

    LabwcRegistryListener registryListener{};
    LabwcToplevelManagerListener toplevelManagerListener{};
    LabwcToplevelHandleListener toplevelHandleListener{};
    LabwcOutputManagerListener outputManagerListener{};
    LabwcOutputHeadListener outputHeadListener{};
    LabwcOutputModeListener outputModeListener{};
    LabwcXdgOutputListener xdgOutputListener{};
    LabwcWlOutputListener wlOutputListener{};
};

// ===========================================================================
// C callbacks -> Impl methods. Each one tolerates a null user_data.
// ===========================================================================

namespace {

LabwcBackend::Impl* impl(void* data) {
    return data ? static_cast<LabwcBackend::Impl*>(data) : nullptr;
}

void registryGlobal(void* data, struct wl_registry* registry, uint32_t name,
                    const char* interface, uint32_t version) {
    (void)registry;
    if (!data || !interface) return;
    static_cast<LabwcBackend::Impl*>(data)->onGlobal(name, interface, version);
}

void registryGlobalRemove(void* data, struct wl_registry* registry, uint32_t name) {
    (void)registry;
    if (!data) return;
    static_cast<LabwcBackend::Impl*>(data)->onGlobalRemove(name);
}

void toplevelManagerToplevel(void* data, struct wl_proxy* manager, struct wl_proxy* toplevel) {
    (void)manager;
    if (LabwcBackend::Impl* p = impl(data)) p->onToplevel(toplevel);
}

void toplevelManagerFinished(void* data, struct wl_proxy* manager) {
    (void)manager;
    if (LabwcBackend::Impl* p = impl(data)) p->onToplevelManagerFinished();
}

void toplevelTitle(void* data, struct wl_proxy* handle, const char* title) {
    if (LabwcBackend::Impl* p = impl(data)) p->onToplevelTitle(handle, title);
}

void toplevelAppId(void* data, struct wl_proxy* handle, const char* app_id) {
    if (LabwcBackend::Impl* p = impl(data)) p->onToplevelAppId(handle, app_id);
}

void toplevelOutputEnter(void* data, struct wl_proxy* handle, struct wl_proxy* output) {
    if (LabwcBackend::Impl* p = impl(data)) p->onToplevelOutputEnter(handle, output);
}

void toplevelOutputLeave(void* data, struct wl_proxy* handle, struct wl_proxy* output) {
    if (LabwcBackend::Impl* p = impl(data)) p->onToplevelOutputLeave(handle, output);
}

void toplevelState(void* data, struct wl_proxy* handle, struct wl_array* state) {
    if (LabwcBackend::Impl* p = impl(data)) p->onToplevelState(handle, state);
}

void toplevelDone(void*, struct wl_proxy*) {
    // Marks the end of an atomic batch; nothing to do for a read-only client.
}

void toplevelClosed(void* data, struct wl_proxy* handle) {
    if (LabwcBackend::Impl* p = impl(data)) p->onToplevelClosed(handle);
}

void toplevelParent(void* data, struct wl_proxy* handle, struct wl_proxy* parent) {
    if (LabwcBackend::Impl* p = impl(data)) p->onToplevelParent(handle, parent);
}

void outputManagerHead(void* data, struct wl_proxy* manager, struct wl_proxy* head) {
    (void)manager;
    if (LabwcBackend::Impl* p = impl(data)) p->onHead(head);
}

void outputManagerDone(void* data, struct wl_proxy* manager, uint32_t serial) {
    (void)manager;
    (void)serial;
    if (LabwcBackend::Impl* p = impl(data)) p->onOutputManagerDone();
}

void outputManagerFinished(void* data, struct wl_proxy* manager) {
    (void)manager;
    if (LabwcBackend::Impl* p = impl(data)) p->onOutputManagerFinished();
}

void headName(void* data, struct wl_proxy* head, const char* name) {
    if (LabwcBackend::Impl* p = impl(data)) p->onHeadName(head, name);
}

void headDescription(void*, struct wl_proxy*, const char*) {}

void headPhysicalSize(void*, struct wl_proxy*, int32_t, int32_t) {}

void headMode(void* data, struct wl_proxy* head, struct wl_proxy* mode) {
    if (LabwcBackend::Impl* p = impl(data)) p->onHeadMode(head, mode);
}

void headEnabled(void* data, struct wl_proxy* head, int32_t enabled) {
    if (LabwcBackend::Impl* p = impl(data)) p->onHeadEnabled(head, enabled);
}

void headCurrentMode(void* data, struct wl_proxy* head, struct wl_proxy* mode) {
    if (LabwcBackend::Impl* p = impl(data)) p->onHeadCurrentMode(head, mode);
}

void headPosition(void* data, struct wl_proxy* head, int32_t x, int32_t y) {
    if (LabwcBackend::Impl* p = impl(data)) p->onHeadPosition(head, x, y);
}

void headTransform(void* data, struct wl_proxy* head, int32_t transform) {
    if (LabwcBackend::Impl* p = impl(data)) p->onHeadTransform(head, transform);
}

void headScale(void* data, struct wl_proxy* head, int32_t scale) {
    if (LabwcBackend::Impl* p = impl(data)) p->onHeadScale(head, scale);
}

void headFinished(void* data, struct wl_proxy* head) {
    if (LabwcBackend::Impl* p = impl(data)) p->onHeadFinished(head);
}

void headMake(void*, struct wl_proxy*, const char*) {}

void headModel(void*, struct wl_proxy*, const char*) {}

void headSerialNumber(void*, struct wl_proxy*, const char*) {}

void headAdaptiveSync(void*, struct wl_proxy*, uint32_t) {}

void modeSize(void* data, struct wl_proxy* mode, int32_t width, int32_t height) {
    if (LabwcBackend::Impl* p = impl(data)) p->onModeSize(mode, width, height);
}

void modeRefresh(void*, struct wl_proxy*, int32_t) {}

void modePreferred(void*, struct wl_proxy*) {}

void modeFinished(void* data, struct wl_proxy* mode) {
    if (LabwcBackend::Impl* p = impl(data)) p->onModeFinished(mode);
}

void xdgLogicalPosition(void* data, struct wl_proxy* out, int32_t x, int32_t y) {
    if (LabwcBackend::Impl* p = impl(data)) p->onXdgLogicalPosition(out, x, y);
}

void xdgLogicalSize(void* data, struct wl_proxy* out, int32_t width, int32_t height) {
    if (LabwcBackend::Impl* p = impl(data)) p->onXdgLogicalSize(out, width, height);
}

void xdgDone(void*, struct wl_proxy*) {}

void xdgName(void* data, struct wl_proxy* out, const char* name) {
    if (LabwcBackend::Impl* p = impl(data)) p->onXdgName(out, name);
}

void xdgDescription(void*, struct wl_proxy*, const char*) {}

void wlOutputGeometry(void* data, struct wl_proxy* out, int32_t x, int32_t y,
                      int32_t physical_width, int32_t physical_height,
                      int32_t subpixel, const char* make, const char* model,
                      int32_t transform) {
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    if (LabwcBackend::Impl* p = impl(data)) p->onWlOutputGeometry(out, x, y, transform);
}

void wlOutputMode(void* data, struct wl_proxy* out, uint32_t flags, int32_t width,
                  int32_t height, int32_t refresh) {
    (void)refresh;
    if (LabwcBackend::Impl* p = impl(data)) p->onWlOutputMode(out, flags, width, height);
}

void wlOutputDone(void*, struct wl_proxy*) {}

void wlOutputScale(void* data, struct wl_proxy* out, int32_t factor) {
    if (LabwcBackend::Impl* p = impl(data)) p->onWlOutputScale(out, factor);
}

void wlOutputName(void* data, struct wl_proxy* out, const char* name) {
    if (LabwcBackend::Impl* p = impl(data)) p->onWlOutputName(out, name);
}

void wlOutputDescription(void*, struct wl_proxy*, const char*) {}

} // namespace

// ===========================================================================
// LabwcBackend
// ===========================================================================

LabwcBackend::LabwcBackend()
    : m_impl(new Impl()) {
    static const LabwcRegistryListener kRegistryListener = {
        registryGlobal, registryGlobalRemove,
    };
    static const LabwcToplevelManagerListener kToplevelManagerListener = {
        toplevelManagerToplevel, toplevelManagerFinished,
    };
    static const LabwcToplevelHandleListener kToplevelHandleListener = {
        toplevelTitle, toplevelAppId, toplevelOutputEnter, toplevelOutputLeave,
        toplevelState, toplevelDone, toplevelClosed, toplevelParent,
    };
    static const LabwcOutputManagerListener kOutputManagerListener = {
        outputManagerHead, outputManagerDone, outputManagerFinished,
    };
    static const LabwcOutputHeadListener kOutputHeadListener = {
        headName, headDescription, headPhysicalSize, headMode, headEnabled,
        headCurrentMode, headPosition, headTransform, headScale, headFinished,
        headMake, headModel, headSerialNumber, headAdaptiveSync,
    };
    static const LabwcOutputModeListener kOutputModeListener = {
        modeSize, modeRefresh, modePreferred, modeFinished,
    };
    static const LabwcXdgOutputListener kXdgOutputListener = {
        xdgLogicalPosition, xdgLogicalSize, xdgDone, xdgName, xdgDescription,
    };
    static const LabwcWlOutputListener kWlOutputListener = {
        wlOutputGeometry, wlOutputMode, wlOutputDone, wlOutputScale,
        wlOutputName, wlOutputDescription,
    };

    m_impl->registryListener = kRegistryListener;
    m_impl->toplevelManagerListener = kToplevelManagerListener;
    m_impl->toplevelHandleListener = kToplevelHandleListener;
    m_impl->outputManagerListener = kOutputManagerListener;
    m_impl->outputHeadListener = kOutputHeadListener;
    m_impl->outputModeListener = kOutputModeListener;
    m_impl->xdgOutputListener = kXdgOutputListener;
    m_impl->wlOutputListener = kWlOutputListener;
}

LabwcBackend::~LabwcBackend() {
    // ~Impl disconnects the display, which frees every proxy we created.
}

bool LabwcBackend::initialize(const char* displayName) {
    if (!m_impl) {
        return false;
    }
    if (m_impl->display) {
        return true;
    }
    return m_impl->connect(displayName);
}

void LabwcBackend::update() {
    if (!m_impl) {
        return;
    }
    m_impl->pump();
}

std::vector<std::string> LabwcBackend::outputNames() const {
    std::vector<std::string> names;
    if (!m_impl) {
        return names;
    }
    names.reserve(m_impl->outputs.size());
    for (const auto& out : m_impl->outputs) {
        names.push_back(out.first);
    }
    return names;
}

bool LabwcBackend::isOutputCovered(const std::string& outputName) const {
    if (!m_impl || !m_impl->display || m_impl->dead) {
        return false;
    }
    auto it = m_impl->outputs.find(outputName);
    if (it == m_impl->outputs.end()) {
        // Unknown, hot-unplugged, or the connection died: assume the
        // wallpaper is visible so the gate never pauses a visible wallpaper.
        return false;
    }
    const Impl::Output& out = it->second;
    if (!out.enabled || !out.haveGeometry || out.width <= 0 || out.height <= 0) {
        return false;
    }
    const Rect bounds{out.x, out.y, out.width, out.height};

    // Rule (a): a fullscreen-family toplevel on this output covers it.
    //
    // Rule (b): the UNION (never the sum) of the remaining toplevel rects
    // reaching the coverage threshold.
    //
    // Rect source: wlr-foreign-toplevel-management carries no per-window
    // geometry -- title, app_id, output_enter/leave, state, done, closed and
    // parent are the entire event set -- so the only rects derivable here
    // are output-sized ones:
    //   * maximized: the window fills the output's work area.
    // Anything finer would be a guess, and a wrong guess pauses the
    // wallpaper while the desktop is on screen, which is worse than never
    // pausing. A real geometry source (labwc's JSON IPC socket needs no
    // wlrctl binary) only has to push rects into this vector: the union and
    // threshold math below is already exact and independent of where the
    // rects came from.
    std::vector<Rect> rects;
    rects.reserve(m_impl->toplevels.size());
    for (const auto& entry : m_impl->toplevels) {
        const Impl::Toplevel& t = entry.second;
        if (t.minimized) {
            continue;
        }
        bool onThisOutput = false;
        for (const struct wl_proxy* output : t.outputProxies) {
            if (m_impl->outputNameFor(output) == outputName) {
                onThisOutput = true;
                break;
            }
        }
        if (!onThisOutput) {
            continue;
        }
        if (t.fullscreen) {
            return true;
        }
        if (t.maximized) {
            rects.push_back(bounds);
        }
    }

    return unionReachesThreshold(unionArea(rects, bounds), bounds, m_threshold);
}

// ===========================================================================
// Coverage math (public so it is testable without a compositor)
// ===========================================================================

long long LabwcBackend::unionArea(const std::vector<Rect>& rects, const Rect& bounds) {
    // Clip to the output first: a window on a neighbouring output, or one
    // overhanging the edge, must not inflate this output's coverage.
    std::vector<Rect> clipped;
    clipped.reserve(rects.size());
    for (const auto& r : rects) {
        const int64_t x0 = std::max<int64_t>(r.x, bounds.x);
        const int64_t y0 = std::max<int64_t>(r.y, bounds.y);
        const int64_t x1 = std::min<int64_t>(static_cast<int64_t>(r.x) + r.width,
                                             static_cast<int64_t>(bounds.x) + bounds.width);
        const int64_t y1 = std::min<int64_t>(static_cast<int64_t>(r.y) + r.height,
                                             static_cast<int64_t>(bounds.y) + bounds.height);
        if (x1 > x0 && y1 > y0) {
            clipped.push_back({static_cast<int32_t>(x0), static_cast<int32_t>(y0),
                               static_cast<int32_t>(x1 - x0), static_cast<int32_t>(y1 - y0)});
        }
    }
    if (clipped.empty()) {
        return 0;
    }

    // Sweep line over the distinct x edges. Between two consecutive edges the
    // set of active rects is constant, so the covered y length is constant
    // across the slab and the slab area is that length times the slab width.
    std::vector<int32_t> xs;
    xs.reserve(clipped.size() * 2);
    for (const auto& r : clipped) {
        xs.push_back(r.x);
        xs.push_back(r.x + r.width);
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());

    long long total = 0;
    std::vector<std::pair<int32_t, int32_t>> spans;
    for (size_t i = 0; i + 1 < xs.size(); ++i) {
        const int32_t xa = xs[i];
        const int32_t xb = xs[i + 1];
        if (xb <= xa) {
            continue;
        }
        spans.clear();
        for (const auto& r : clipped) {
            if (r.x <= xa && r.x + r.width >= xb) {
                spans.emplace_back(r.y, r.y + r.height);
            }
        }
        if (spans.empty()) {
            continue;
        }
        std::sort(spans.begin(), spans.end());
        int32_t curStart = spans.front().first;
        int32_t curEnd = spans.front().second;
        long long coveredY = 0;
        for (size_t j = 1; j < spans.size(); ++j) {
            if (spans[j].first > curEnd) {
                coveredY += curEnd - curStart;
                curStart = spans[j].first;
                curEnd = spans[j].second;
            } else {
                curEnd = std::max(curEnd, spans[j].second);
            }
        }
        coveredY += curEnd - curStart;
        total += coveredY * static_cast<long long>(xb - xa);
    }
    return total;
}

std::unique_ptr<CompositorBackend> makeLabwcBackend() {
    auto backend = std::make_unique<LabwcBackend>();
    if (!backend->initialize()) {
        return nullptr;
    }
    return backend;
}

} // namespace WallpaperEngine::Scene
