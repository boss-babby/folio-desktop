// folio-platform/linux/XiValuator.h
//
// The one piece of XInput2 decoding that is easy to get wrong, isolated so
// it can be tested without an X server, a tablet, or a finger.
//
// XI2 does not send a value per axis. It sends a bitmask of which axes are
// present followed by a tightly packed array holding only those values, in
// ascending axis order. So the value for axis N is not `values[N]` - it is
// `values[popcount(mask bits below N)]`, and reading it as `values[N]` gives
// plausible-looking garbage: a pen whose pressure is really its tilt.
#ifndef FOLIO_PLATFORM_LINUX_XIVALUATOR_H
#define FOLIO_PLATFORM_LINUX_XIVALUATOR_H

namespace folio::platform::x11detail {

/// Reads valuator `axis` out of an XI2 packed valuator set.
/// Returns false (leaving `out` untouched) when that axis is not present in
/// this event, which is normal: a device only reports the axes that moved.
inline bool valuator(const unsigned char* mask, int maskLen,
                     const double* values, int axis, double* out) {
    if (!mask || !values || !out || axis < 0) return false;
    const double* v = values;
    for (int i = 0; i < maskLen * 8; ++i) {
        // XIMaskIsSet, open-coded so this header needs no X11 at all.
        if (!(mask[i >> 3] & (1 << (i & 7)))) continue;
        if (i == axis) {
            *out = *v;
            return true;
        }
        ++v;
    }
    return false;
}

}  // namespace folio::platform::x11detail

#endif
