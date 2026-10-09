// tests/platform/test_xi_valuator.cpp
//
// XInput2's packed valuator encoding, tested without an X server.
//
// This is the part of the XI2 backend that cannot be checked by looking at
// it. Everything else in X11Window's XI2 path is a field copy; this one
// decodes a sparse array, and getting it wrong does not crash or fail to
// compile - it silently returns a different axis. A stylus would report
// tilt as pressure and strokes would come out the wrong width, which is
// exactly the kind of bug that survives until someone plugs in hardware.
#include "../../folio-platform/linux/XiValuator.h"

#include <cstdio>

using folio::platform::x11detail::valuator;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

static void checkEq(double got, double want, const char* what) {
    if (got != want) {
        std::printf("  FAIL: %s (got %g, want %g)\n", what, got, want);
        ++failures;
    }
}

int main() {
    std::printf("xi valuator decoding\n");

    // A Wacom-shaped device: axes 0,1 are x/y, 2 is pressure, 3/4 are tilt.
    // All five present.
    {
        const unsigned char mask[1] = {0b00011111};
        const double values[5] = {100.0, 200.0, 512.0, -7.0, 11.0};
        double out = -1.0;
        check(valuator(mask, 1, values, 2, &out), "dense: pressure present");
        checkEq(out, 512.0, "dense: pressure value");
        check(valuator(mask, 1, values, 4, &out), "dense: tilt-y present");
        checkEq(out, 11.0, "dense: tilt-y value");
    }

    // The case that matters. Only x and pressure moved, so the packed array
    // is {x, pressure} and pressure lives at index 1 even though it is
    // axis 2. Indexing by axis number would return the y coordinate.
    {
        const unsigned char mask[1] = {0b00000101};  // axes 0 and 2
        const double values[2] = {100.0, 512.0};
        double out = -1.0;
        check(valuator(mask, 1, values, 2, &out), "sparse: pressure present");
        checkEq(out, 512.0, "sparse: pressure read from packed slot 1");
        checkEq(valuator(mask, 1, values, 1, &out), false,
                "sparse: absent axis reports absent");
    }

    // An absent axis must leave the destination alone rather than zero it:
    // a move event that carries no pressure should not slam the stroke
    // width to nothing mid-stroke.
    {
        const unsigned char mask[1] = {0b00000011};
        const double values[2] = {1.0, 2.0};
        double out = 0.75;
        check(!valuator(mask, 1, values, 5, &out), "absent axis returns false");
        checkEq(out, 0.75, "absent axis leaves destination untouched");
    }

    // Axes past the first mask byte: a device with many axes puts pressure
    // beyond bit 7, where an off-by-one in the byte indexing shows up.
    {
        unsigned char mask[2] = {0b00000001, 0b00000100};  // axes 0 and 10
        const double values[2] = {5.0, 909.0};
        double out = -1.0;
        check(valuator(mask, 2, values, 10, &out), "multi-byte: axis 10 present");
        checkEq(out, 909.0, "multi-byte: axis 10 value");
    }

    // Degenerate inputs must not walk off the array.
    {
        double out = 0.0;
        const unsigned char mask[1] = {0b00000001};
        const double values[1] = {1.0};
        check(!valuator(nullptr, 1, values, 0, &out), "null mask");
        check(!valuator(mask, 1, nullptr, 0, &out), "null values");
        check(!valuator(mask, 1, values, -1, &out), "negative axis");
        check(!valuator(mask, 0, values, 0, &out), "zero-length mask");
    }

    if (failures == 0) std::printf("  all checks passed\n");
    return failures == 0 ? 0 : 1;
}
