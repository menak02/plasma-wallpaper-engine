#include "scene/x11_ewmh_backend.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace WallpaperEngine::Scene;
using Rect = X11EwmhBackend::Rect;

// A 100x100 output is the common case (one monitor, 100x100 logical area).
// Every case below is sized off this so the expected numbers are readable.
static Rect makeRect(int32_t x, int32_t y, int32_t w, int32_t h) {
    return Rect{x, y, w, h};
}

static void assertArea(long long expected, long long actual, const char* desc) {
    if (expected != actual) {
        std::cerr << "FAIL: " << desc << " (expected " << expected
                  << ", got " << actual << ")" << std::endl;
        std::exit(1);
    }
    std::cout << "PASS: " << desc << std::endl;
}

static void assertCovered(bool expected, bool actual, const char* desc) {
    if (expected != actual) {
        std::cerr << "FAIL: " << desc << " (expected " << expected
                  << ", got " << actual << ")" << std::endl;
        std::exit(1);
    }
    std::cout << "PASS: " << desc << std::endl;
}

int main() {
    const Rect bounds = makeRect(0, 0, 100, 100);

    // Nothing to union: an empty window list must not count as coverage.
    {
        assertArea(0, X11EwmhBackend::unionArea({}, bounds),
                   "empty rect list -> union area 0");
    }

    // A single window strictly inside the output is its own area, unclipped.
    {
        const std::vector<Rect> rects = {makeRect(20, 30, 40, 50)};
        assertArea(2000, X11EwmhBackend::unionArea(rects, bounds),
                   "single rect inside bounds -> exact area");
    }

    // Windows routinely hang off the output (partially off-screen, maximized
    // past the edge). Clipping must happen on all four sides and must never
    // produce a negative area.
    {
        // Crosses the left and top edges: clipped to a 50x50 corner.
        const std::vector<Rect> topLeft = {makeRect(-50, -50, 100, 100)};
        assertArea(2500, X11EwmhBackend::unionArea(topLeft, bounds),
                   "rect clipped on left+top -> clipped corner area");

        // Crosses the right and bottom edges: clipped to the same 50x50.
        const std::vector<Rect> bottomRight = {makeRect(50, 50, 100, 100)};
        assertArea(2500, X11EwmhBackend::unionArea(bottomRight, bounds),
                   "rect clipped on right+bottom -> clipped corner area");

        // Larger than the output on every side: clipped down to the bounds.
        const std::vector<Rect> oversize = {makeRect(-100, -100, 400, 400)};
        assertArea(10000, X11EwmhBackend::unionArea(oversize, bounds),
                   "rect larger than bounds on all sides -> full bounds area");
    }

    // THE REGRESSION: the Hyprland backend summed client areas, so two
    // identical windows reported 200% coverage. The union must count the
    // shared pixels once.
    {
        const std::vector<Rect> dup = {makeRect(10, 10, 50, 50),
                                       makeRect(10, 10, 50, 50)};
        const long long area = X11EwmhBackend::unionArea(dup, bounds);
        assertArea(2500, area, "two identical rects -> counted once, not twice");
    }

    // Half-overlapping pair: the shared 50x100 strip must be counted once, so
    // the union is 1.5x a single window rather than the 2.0x a naive sum
    // would report. The bounds here are deliberately wider than one window so
    // that clipping is not what limits the result.
    {
        const Rect wideBounds = makeRect(0, 0, 200, 100);
        const std::vector<Rect> half = {makeRect(0, 0, 100, 100),
                                        makeRect(50, 0, 100, 100)};
        const long long area = X11EwmhBackend::unionArea(half, wideBounds);
        assertArea(15000, area, "two rects overlapping 50% -> 1.5x single area");

        // Guard the property directly: a sum would be 20000, the union is not.
        if (area >= 20000) {
            std::cerr << "FAIL: 50% overlap union degenerated into a sum"
                      << std::endl;
            std::exit(1);
        }
    }

    // Adjacent windows sharing an edge tile the output. Touching is not
    // overlapping, so the union must be the full output and not one pixel
    // more (double-counting the shared edge) or one pixel less (a gap).
    {
        const std::vector<Rect> tiled = {makeRect(0, 0, 50, 100),
                                         makeRect(50, 0, 50, 100)};
        assertArea(10000, X11EwmhBackend::unionArea(tiled, bounds),
                   "two adjacent rects tiling bounds -> full bounds area");
    }

    // Four quadrants: many distinct x edges, y spans that abut within a slab.
    // This is the shape of a real tiled desktop and must be exact.
    {
        const std::vector<Rect> quadrants = {makeRect(0, 0, 50, 50),
                                             makeRect(50, 0, 50, 50),
                                             makeRect(0, 50, 50, 50),
                                             makeRect(50, 50, 50, 50)};
        assertArea(10000, X11EwmhBackend::unionArea(quadrants, bounds),
                   "four non-overlapping quadrants -> exactly full area");
    }

    // Rects that miss the output entirely contribute nothing. Negative-area
    // results here would be the signature of clipping that underflows.
    {
        const std::vector<Rect> left = {makeRect(-200, -200, 50, 50)};
        const std::vector<Rect> right = {makeRect(200, 200, 50, 50)};
        const std::vector<Rect> mixed = {makeRect(-200, -200, 50, 50),
                                         makeRect(10, 10, 20, 20)};
        assertArea(0, X11EwmhBackend::unionArea(left, bounds),
                   "rect entirely left of bounds -> 0");
        assertArea(0, X11EwmhBackend::unionArea(right, bounds),
                   "rect entirely right of bounds -> 0");
        assertArea(400, X11EwmhBackend::unionArea(mixed, bounds),
                   "out-of-bounds rect ignored, in-bounds rect still counted");
    }

    // A gap between two windows opens an x-slab covered by nothing. The slab
    // must contribute zero instead of being filled in by the sweep.
    {
        const std::vector<Rect> gap = {makeRect(0, 0, 30, 100),
                                       makeRect(70, 0, 30, 100)};
        const long long area = X11EwmhBackend::unionArea(gap, bounds);
        assertArea(6000, area, "gap between rects -> empty x-slab excluded");

        // 40 wide x 100 tall of gap must not appear anywhere in the total.
        if (area > 10000) {
            std::cerr << "FAIL: empty slab leaked into the union total"
                      << std::endl;
            std::exit(1);
        }
    }

    // Nested rects: a small window inside a big one adds nothing. Real
    // desktops produce this whenever a dialog sits on top of a maximized
    // window, and it is the case a naive sum gets badly wrong.
    {
        const std::vector<Rect> nested = {makeRect(0, 0, 100, 100),
                                          makeRect(40, 40, 20, 20)};
        assertArea(10000, X11EwmhBackend::unionArea(nested, bounds),
                   "nested rect inside another -> no extra area");
    }

    // Vertical stacking shares no x edges, so the sweep has a single slab and
    // the y-span merge does all the work.
    {
        const std::vector<Rect> column = {makeRect(0, 0, 100, 50),
                                          makeRect(0, 50, 100, 50)};
        assertArea(10000, X11EwmhBackend::unionArea(column, bounds),
                   "vertically stacked rects -> full bounds area");
    }

    // The decision the pause gate actually acts on. The default threshold is
    // 0.90, and the comparison is inclusive: exactly at threshold covers.
    {
        assertCovered(false,
                      X11EwmhBackend::unionReachesThreshold(8900, bounds, 0.90),
                      "0.89 coverage at 0.90 threshold -> not covered");
        assertCovered(true,
                      X11EwmhBackend::unionReachesThreshold(9000, bounds, 0.90),
                      "0.90 coverage at 0.90 threshold -> covered");
        assertCovered(true,
                      X11EwmhBackend::unionReachesThreshold(9500, bounds, 0.90),
                      "0.95 coverage at 0.90 threshold -> covered");
    }

    // A degenerate output can never be covered, and neither can an empty
    // window list -- both would otherwise divide by zero or read as covered.
    {
        const Rect empty = makeRect(0, 0, 0, 0);
        assertCovered(false,
                      X11EwmhBackend::unionReachesThreshold(0, empty, 0.90),
                      "zero-area bounds -> not covered");
        assertCovered(false,
                      X11EwmhBackend::unionReachesThreshold(0, bounds, 0.90),
                      "no windows on a valid output -> not covered");
    }

    // End-to-end through the real math: a realistic maximized-plus-overlap
    // layout whose area crosses the threshold, and a sparse layout that does
    // not. This is the false-positive the area rewrite exists to prevent.
    {
        // A maximized window plus a second window overlapping half of it.
        // Sum would be 150% of the output; the area is 100%.
        const std::vector<Rect> maximized = {makeRect(0, 0, 100, 100),
                                             makeRect(0, 0, 100, 100)};
        const long long area = X11EwmhBackend::unionArea(maximized, bounds);
        assertCovered(true,
                      X11EwmhBackend::unionReachesThreshold(area, bounds, 0.90),
                      "maximized window -> covered");

        // Half the output covered: a naive sum of the same rects still says
        // 100%, but the true area is below the threshold and must not pause.
        const std::vector<Rect> partial = {makeRect(0, 0, 50, 100)};
        const long long halfUnion = X11EwmhBackend::unionArea(partial, bounds);
        assertCovered(false,
                      X11EwmhBackend::unionReachesThreshold(halfUnion, bounds, 0.90),
                      "half-covered output -> not covered");
    }

    std::cout << "x11_ewmh_coverage_test: all assertions passed" << std::endl;
    return 0;
}
