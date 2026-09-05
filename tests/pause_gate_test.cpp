#include "scene/pause_gate.h"
#include "scene/compositor_backend_fake.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace WallpaperEngine::Scene;

static void assertRender(bool expected, bool actual, const char* desc) {
    if (expected != actual) {
        std::cerr << "FAIL: " << desc << " (expected " << expected
                  << ", got " << actual << ")" << std::endl;
        std::exit(1);
    }
    std::cout << "PASS: " << desc << std::endl;
}

int main() {
    FakeCompositorBackend backend;

    // One covered output should cause pause in per-output mode only if every
    // output is covered. With a single output, cover it -> pause.
    {
        backend.setCovered("screen0", true);
        PauseGateConfig cfg;
        cfg.enabled = true;
        cfg.pauseAllOutputs = false;
        assertRender(false, shouldRender(backend, cfg),
                     "single covered output, per-output mode -> no render");
    }

    // One uncovered output should keep rendering in per-output mode.
    {
        backend.setCovered("screen0", false);
        PauseGateConfig cfg;
        cfg.enabled = true;
        cfg.pauseAllOutputs = false;
        assertRender(true, shouldRender(backend, cfg),
                     "single uncovered output, per-output mode -> render");
    }

    // Pause-all mode: one covered output -> pause.
    {
        backend.setCovered("screen0", true);
        PauseGateConfig cfg;
        cfg.enabled = true;
        cfg.pauseAllOutputs = true;
        assertRender(false, shouldRender(backend, cfg),
                     "single covered output, pause-all mode -> no render");
    }

    // Pause-all mode: all uncovered -> render.
    {
        backend.setCovered("screen0", false);
        PauseGateConfig cfg;
        cfg.enabled = true;
        cfg.pauseAllOutputs = true;
        assertRender(true, shouldRender(backend, cfg),
                     "all uncovered, pause-all mode -> render");
    }

    // Multi-output per-output: one covered, one uncovered -> render.
    {
        backend.setCovered("screen0", true);
        backend.setCovered("screen1", false);
        PauseGateConfig cfg;
        cfg.enabled = true;
        cfg.pauseAllOutputs = false;
        assertRender(true, shouldRender(backend, cfg),
                     "one covered + one uncovered, per-output -> render");
    }

    // Multi-output per-output: both covered -> no render.
    {
        backend.setCovered("screen0", true);
        backend.setCovered("screen1", true);
        PauseGateConfig cfg;
        cfg.enabled = true;
        cfg.pauseAllOutputs = false;
        assertRender(false, shouldRender(backend, cfg),
                     "both covered, per-output -> no render");
    }

    // Multi-output pause-all: one covered -> no render.
    {
        backend.setCovered("screen0", true);
        backend.setCovered("screen1", false);
        PauseGateConfig cfg;
        cfg.enabled = true;
        cfg.pauseAllOutputs = true;
        assertRender(false, shouldRender(backend, cfg),
                     "one covered, pause-all -> no render");
    }

    // When the gate is disabled, covered outputs still render.
    {
        backend.setCovered("screen0", true);
        PauseGateConfig cfg;
        cfg.enabled = false;
        cfg.pauseAllOutputs = false;
        assertRender(true, shouldRender(backend, cfg),
                     "gate disabled -> always render");
    }

    // No backend outputs -> always render.
    {
        PauseGateConfig cfg;
        cfg.enabled = true;
        cfg.pauseAllOutputs = false;
        assertRender(true, shouldRender(backend, cfg),
                     "no tracked outputs -> always render");
    }

    std::cout << "pause_gate_test: all assertions passed" << std::endl;
    return 0;
}
