#include "folio/ui/Icons.h"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cmath>
#include <utility>
#include <cstdio>
#include <set>

namespace folio::ui {
namespace {

using Op = PathVerb::Op;

constexpr float kCircleK = 0.5522847498f;  // 4/3 * (sqrt(2) - 1)

struct Cursor {
    std::string_view s;
    std::size_t i = 0;

    void skipSep() {
        while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) ||
                                s[i] == ',')) {
            ++i;
        }
    }
    [[nodiscard]] bool atEnd() {
        skipSep();
        return i >= s.size();
    }
    /// Peek the next non-separator character without consuming it.
    [[nodiscard]] char peek() {
        skipSep();
        return i < s.size() ? s[i] : '\0';
    }
    /// Parse an SVG arc flag: exactly one '0' or '1', which may be written
    /// with no separator before the next number ("a2 2 0 00-2 2" packs
    /// largeArc=0, sweep=0 and dx=-2 together). A general number parser
    /// swallows "00" as a single zero and silently loses a flag, which is
    /// why this needs to be its own function.
    bool flag(bool& out) {
        skipSep();
        if (i >= s.size() || (s[i] != '0' && s[i] != '1')) return false;
        out = s[i] == '1';
        ++i;
        return true;
    }

    /// Parse a float. Returns false at a non-numeric token.
    bool number(float& out) {
        skipSep();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' ||
              c == '.')) {
            return false;
        }
        // from_chars does not accept a leading '+', which SVG allows.
        std::size_t begin = i;
        if (s[i] == '+') ++begin;
        const char* first = s.data() + begin;

#if defined(__cpp_lib_to_chars) && !defined(_LIBCPP_VERSION)
        // Preferred: locale-independent, non-allocating, and it reports
        // exactly where it stopped.
        float v = 0.f;
        const char* last = s.data() + s.size();
        auto [ptr, ec] = std::from_chars(first, last, v);
        if (ec != std::errc{}) return false;
        i = static_cast<std::size_t>(ptr - s.data());
#else
        // libc++ (the NDK's standard library) declares the floating-point
        // from_chars overloads but does not define them, so the call
        // resolves to the deleted bool overload and fails to compile.
        // strtof is the portable fallback. It is locale-sensitive in
        // principle; in practice this parses our own compiled-in icon
        // paths, and nothing in the process ever calls setlocale.
        char* end = nullptr;
        const float v = std::strtof(first, &end);
        if (end == first) return false;
        i = static_cast<std::size_t>(end - s.data());
#endif
        out = v;
        return true;
    }
};

/// Append an SVG elliptical arc as up to four cubic segments.
///
/// Implements the endpoint-to-centre parameterisation of SVG 1.1 appendix
/// F.6.5. The conversion is unavoidable: 18 of the icons in this file are
/// drawn with `a` commands (every rounded folder corner, every rotate
/// arrow), and a parser that bails on them renders those icons as a few
/// disconnected strokes - which is exactly what happened before this
/// existed, silently, because an unknown command just ended the path.
void appendArc(std::vector<PathVerb>& out, float x1, float y1, float rx, float ry,
               float xRotDeg, bool largeArc, bool sweep, float x2, float y2) {
    // Degenerate radii: the spec says treat as a straight line.
    if (rx == 0.f || ry == 0.f || (x1 == x2 && y1 == y2)) {
        out.push_back({Op::LineTo, x2, y2});
        return;
    }
    rx = std::fabs(rx);
    ry = std::fabs(ry);

    const float phi = xRotDeg * 3.14159265358979f / 180.f;
    const float cosP = std::cos(phi), sinP = std::sin(phi);

    // Step 1: translate so the midpoint is the origin, then derotate.
    const float dx2 = (x1 - x2) * 0.5f, dy2 = (y1 - y2) * 0.5f;
    const float x1p = cosP * dx2 + sinP * dy2;
    const float y1p = -sinP * dx2 + cosP * dy2;

    // Step 2: scale the radii up if they are too small to span the chord.
    const float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.f) {
        const float s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
    }

    // Step 3: the centre, in the derotated frame.
    const float rx2 = rx * rx, ry2 = ry * ry;
    const float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    const float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    float coef = (den > 0.f) ? std::sqrt(std::max(0.f, num / den)) : 0.f;
    if (largeArc == sweep) coef = -coef;
    const float cxp = coef * (rx * y1p / ry);
    const float cyp = coef * (-ry * x1p / rx);

    // Step 4: back to user space.
    const float cx = cosP * cxp - sinP * cyp + (x1 + x2) * 0.5f;
    const float cy = sinP * cxp + cosP * cyp + (y1 + y2) * 0.5f;

    // Step 5: start angle and sweep.
    const float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    const float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;

    const float theta1 = std::atan2(uy, ux);
    float dTheta = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    constexpr float kTwoPi = 6.28318530717959f;
    if (!sweep && dTheta > 0.f) dTheta -= kTwoPi;
    if (sweep && dTheta < 0.f) dTheta += kTwoPi;

    // Step 6: split into <= 90-degree pieces. A single cubic cannot
    // approximate more than a quarter turn to within a usable tolerance.
    const int segs = std::max(1, static_cast<int>(
                                     std::ceil(std::fabs(dTheta) / (kTwoPi / 4.f))));
    const float delta = dTheta / static_cast<float>(segs);
    // Standard tangent-length factor for a circular arc of angle `delta`.
    const float t = (4.f / 3.f) * std::tan(delta * 0.25f);

    float th = theta1;
    for (int i = 0; i < segs; ++i) {
        const float cosT = std::cos(th), sinT = std::sin(th);
        const float thNext = th + delta;
        const float cosN = std::cos(thNext), sinN = std::sin(thNext);

        // Points and derivatives on the unit circle, mapped through the
        // ellipse's scale and rotation.
        auto map = [&](float ex, float ey) {
            return std::pair<float, float>{cosP * rx * ex - sinP * ry * ey + cx,
                                           sinP * rx * ex + cosP * ry * ey + cy};
        };
        auto mapVec = [&](float ex, float ey) {
            return std::pair<float, float>{cosP * rx * ex - sinP * ry * ey,
                                           sinP * rx * ex + cosP * ry * ey};
        };

        const auto p1 = map(cosT, sinT);
        const auto p2 = map(cosN, sinN);
        const auto d1 = mapVec(-sinT, cosT);
        const auto d2 = mapVec(-sinN, cosN);

        out.push_back({Op::CubicTo, p1.first + t * d1.first, p1.second + t * d1.second,
                       p2.first - t * d2.first, p2.second - t * d2.second, p2.first,
                       p2.second});
        th = thNext;
    }
}

}  // namespace

std::vector<PathVerb> parseSvgPath(std::string_view d) {
    std::vector<PathVerb> out;
    Cursor cur{d};

    float px = 0.f, py = 0.f;     // current point
    float sx = 0.f, sy = 0.f;     // current subpath start
    float lcx = 0.f, lcy = 0.f;   // last cubic control (for S)
    float lqx = 0.f, lqy = 0.f;   // last quad control (for T)
    char prevCmd = '\0';
    char cmd = '\0';

    auto rel = [&](char c) { return std::islower(static_cast<unsigned char>(c)) != 0; };

    while (!cur.atEnd()) {
        const char c = cur.peek();
        if (std::isalpha(static_cast<unsigned char>(c))) {
            cmd = c;
            ++cur.i;
        } else if (cmd == '\0') {
            break;  // numbers before any command: malformed
        } else if (cmd == 'M') {
            cmd = 'L';  // implicit lineto after moveto, per SVG spec
        } else if (cmd == 'm') {
            cmd = 'l';
        }

        const bool r = rel(cmd);
        const float ox = r ? px : 0.f;
        const float oy = r ? py : 0.f;
        float a = 0, b = 0, e = 0, f = 0, g = 0, h = 0;

        switch (std::toupper(static_cast<unsigned char>(cmd))) {
            case 'M':
                if (!cur.number(a) || !cur.number(b)) return out;
                px = ox + a; py = oy + b;
                sx = px; sy = py;
                out.push_back({Op::MoveTo, px, py});
                break;
            case 'L':
                if (!cur.number(a) || !cur.number(b)) return out;
                px = ox + a; py = oy + b;
                out.push_back({Op::LineTo, px, py});
                break;
            case 'H':
                if (!cur.number(a)) return out;
                px = ox + a;
                out.push_back({Op::LineTo, px, py});
                break;
            case 'V':
                if (!cur.number(a)) return out;
                py = oy + a;
                out.push_back({Op::LineTo, px, py});
                break;
            case 'C':
                if (!cur.number(a) || !cur.number(b) || !cur.number(e) ||
                    !cur.number(f) || !cur.number(g) || !cur.number(h)) {
                    return out;
                }
                out.push_back({Op::CubicTo, ox + a, oy + b, ox + e, oy + f,
                               ox + g, oy + h});
                lcx = ox + e; lcy = oy + f;
                px = ox + g; py = oy + h;
                break;
            case 'S': {
                if (!cur.number(e) || !cur.number(f) || !cur.number(g) ||
                    !cur.number(h)) {
                    return out;
                }
                // Reflect the previous control point, but only if the
                // previous command was itself a cubic; otherwise the control
                // coincides with the current point (SVG 8.3.6).
                const bool chain = prevCmd == 'C' || prevCmd == 'c' ||
                                   prevCmd == 'S' || prevCmd == 's';
                const float rx = chain ? 2.f * px - lcx : px;
                const float ry = chain ? 2.f * py - lcy : py;
                out.push_back({Op::CubicTo, rx, ry, ox + e, oy + f, ox + g, oy + h});
                lcx = ox + e; lcy = oy + f;
                px = ox + g; py = oy + h;
                break;
            }
            case 'Q':
                if (!cur.number(a) || !cur.number(b) || !cur.number(e) ||
                    !cur.number(f)) {
                    return out;
                }
                out.push_back({Op::QuadTo, ox + a, oy + b, ox + e, oy + f});
                lqx = ox + a; lqy = oy + b;
                px = ox + e; py = oy + f;
                break;
            case 'T': {
                if (!cur.number(e) || !cur.number(f)) return out;
                const bool chain = prevCmd == 'Q' || prevCmd == 'q' ||
                                   prevCmd == 'T' || prevCmd == 't';
                const float rx = chain ? 2.f * px - lqx : px;
                const float ry = chain ? 2.f * py - lqy : py;
                out.push_back({Op::QuadTo, rx, ry, ox + e, oy + f});
                lqx = rx; lqy = ry;
                px = ox + e; py = oy + f;
                break;
            }
            case 'A': {
                float rx = 0, ry = 0, rot = 0;
                bool large = false, swp = false;
                if (!cur.number(rx) || !cur.number(ry) || !cur.number(rot) ||
                    !cur.flag(large) || !cur.flag(swp) || !cur.number(a) ||
                    !cur.number(b)) {
                    return out;
                }
                const float ex = ox + a, ey = oy + b;
                appendArc(out, px, py, rx, ry, rot, large, swp, ex, ey);
                px = ex; py = ey;
                break;
            }
            case 'Z':
                out.push_back({Op::Close});
                px = sx; py = sy;
                break;
            default:
                return out;  // unsupported command (e.g. arcs)
        }
        prevCmd = cmd;
    }
    return out;
}

void appendCircle(std::vector<PathVerb>& out, float cx, float cy, float r) {
    const float k = r * kCircleK;
    out.push_back({Op::MoveTo, cx + r, cy});
    out.push_back({Op::CubicTo, cx + r, cy + k, cx + k, cy + r, cx, cy + r});
    out.push_back({Op::CubicTo, cx - k, cy + r, cx - r, cy + k, cx - r, cy});
    out.push_back({Op::CubicTo, cx - r, cy - k, cx - k, cy - r, cx, cy - r});
    out.push_back({Op::CubicTo, cx + k, cy - r, cx + r, cy - k, cx + r, cy});
    out.push_back({Op::Close});
}

// ---------------------------------------------------------------------------
// The icon set
// ---------------------------------------------------------------------------
namespace {

struct Def {
    const char* name;
    const char* d;
};

// Authored on a 24x24 grid. "@c cx cy r" appends a circle; it keeps the
// table readable for the many icons that are a path plus a dot.
constexpr Def kIcons[] = {
    // --- window / chrome ---------------------------------------------------
    {"minimise",     "M5 12h14"},
    {"underline",    "M6 4v6a6 6 0 0 0 12 0V4M5 20h14"},
    {"strikethrough","M16 5H9.5a3 3 0 0 0 0 6M8 19h6.5a3 3 0 0 0 0-6M4 12h16"},
    {"maximise",     "M5 5h14v14H5z"},
    {"restore",      "M8 8V5h11v11h-3M5 8h11v11H5z"},
    {"close",        "M6 6l12 12M18 6L6 18"},
    {"menu",         "M4 7h16M4 12h16M4 17h16"},
    {"more",         "@c 5 12 1.3 @c 12 12 1.3 @c 19 12 1.3"},

    // --- navigation --------------------------------------------------------
    {"chevron-left",  "M15 5l-7 7 7 7"},
    {"chevron-right", "M9 5l7 7-7 7"},
    {"chevron-up",    "M5 15l7-7 7 7"},
    {"chevron-down",  "M5 9l7 7 7-7"},
    {"arrow-left",    "M19 12H5M11 6l-6 6 6 6"},
    {"arrow-right",   "M5 12h14M13 6l6 6-6 6"},
    {"arrow-up",      "M12 19V5M6 11l6-6 6 6"},
    {"arrow-down",    "M12 5v14M6 13l6 6 6-6"},

    // --- files -------------------------------------------------------------
    {"folder",       "M3 7a2 2 0 012-2h4l2 2h8a2 2 0 012 2v8a2 2 0 01-2 2H5a2 2 0 01-2-2z"},
    {"folder-open",  "M3 7a2 2 0 012-2h4l2 2h8a2 2 0 012 2v1H3zM3 10h18l-2 7a2 2 0 01-2 1H5a2 2 0 01-2-2z"},
    {"file",         "M14 3H7a2 2 0 00-2 2v14a2 2 0 002 2h10a2 2 0 002-2V8zM14 3v5h5"},
    {"file-text",    "M14 3H7a2 2 0 00-2 2v14a2 2 0 002 2h10a2 2 0 002-2V8zM14 3v5h5M8 13h8M8 17h5"},
    {"download",     "M12 4v10M8 11l4 4 4-4M4 19h16"},
    {"upload",       "M12 16V6M8 9l4-4 4 4M4 19h16"},
    {"print",        "M7 9V4h10v5M7 18H5a2 2 0 01-2-2v-4a2 2 0 012-2h14a2 2 0 012 2v4a2 2 0 01-2 2h-2M7 15h10v5H7z"},
    {"save",         "M5 4h11l3 3v13H5zM8 4v5h7V4M8 20v-6h8v6"},
    {"share",        "M12 15V4M8 7l4-3 4 3M5 13v6a1 1 0 001 1h12a1 1 0 001-1v-6"},
    {"trash",        "M4 7h16M9 7V5h6v2M6 7l1 13h10l1-13M10 11v6M14 11v6"},
    {"copy",         "M9 9h10v10H9zM5 15H3V5h10v2"},

    // --- view --------------------------------------------------------------
    {"search",       "@c 11 11 6 M20 20l-4.4-4.4"},
    {"zoom-in",      "@c 11 11 6 M20 20l-4.4-4.4 M8 11h6M11 8v6"},
    {"zoom-out",     "@c 11 11 6 M20 20l-4.4-4.4 M8 11h6"},
    {"fit-width",    "M3 6v12M21 6v12M7 12h10M9 9l-3 3 3 3M15 9l3 3-3 3"},
    {"fit-page",     "M6 3h12v18H6zM12 8v8M9.5 10.5 12 8l2.5 2.5M9.5 13.5 12 16l2.5-2.5"},
    {"single-page",  "M7 4h10v16H7z"},
    {"two-up",       "M3 4h8v16H3zM13 4h8v16h-8z"},
    {"continuous",   "M6 3h12v7H6zM6 14h12v7H6z"},
    // Three strokes of increasing weight: the thickness control. Drawn as
    // filled bars rather than strokes so the weight difference survives at
    // 20px, where a 1px vs 2px stroke is indistinguishable.
    {"stroke-weight", "M4 6h16v1.2H4zM4 11h16v2.4H4zM4 17h16v4H4z"},
    // A half-filled droplet: opacity. The fill is the left half, so the
    // glyph reads as "partly transparent" rather than as a plain drop.
    {"opacity",      "M12 3c3.5 4.2 5.5 7 5.5 9.5A5.5 5.5 0 0 1 12 18a5.5 5.5 "
                     "0 0 1-5.5-5.5C6.5 10 8.5 7.2 12 3zM12 3v15"},
    // Lowercase/uppercase A pair: the font-family control.
    {"type",         "M4 19l5-13 5 13M5.6 15h6.8M16 19l3-8 3 8M16.8 17h4.4"},
    // A glyph between size markers: text size.
    {"text-size",    "M3 7V5h10v2M8 5v14M15 12v-1.5h6V12M18 10.5V19"},
    {"grid",         "M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM13 13h7v7h-7z"},
    {"columns",      "M4 4h16v16H4zM12 4v16"},
    {"sidebar",      "M4 4h16v16H4zM9 4v16"},
    {"panel-right",  "M4 4h16v16H4zM15 4v16"},
    {"thumbnails",   "M4 4h5v6H4zM4 14h5v6H4zM12 5h8M12 9h8M12 15h8M12 19h8"},
    {"rotate-cw",    "M20 7v5h-5 M19.4 12a7.5 7.5 0 10-2 6"},
    {"rotate-ccw",   "M4 7v5h5 M4.6 12a7.5 7.5 0 112 6"},
    {"maximise-view", "M14 4h6v6M20 4l-7 7M10 20H4v-6M4 20l7-7"},
    {"minimise-view", "M20 10h-6V4M14 10l7-7M4 14h6v6M10 14l-7 7"},

    // --- editing tools -----------------------------------------------------
    {"cursor",       "M5 3l14 7-6 2-2 6z"},
    {"hand",         "M8 12V6a1.5 1.5 0 013 0v5V4.5a1.5 1.5 0 013 0V11V6.5a1.5 1.5 0 013 0V14a6 6 0 01-6 6h-1a5 5 0 01-5-5v-4a1.5 1.5 0 013 0z"},
    {"highlighter",  "M4 20h6l9-9a2.5 2.5 0 00-3.5-3.5l-9 9zM13 8l3 3"},
    {"pen",          "M4 20l1-4L16 5a2.1 2.1 0 013 3L8 19zM14 7l3 3"},
    {"eraser",       "M8 20H5l-2-2 10-10a2 2 0 013 0l4 4a2 2 0 010 3l-7 7zM11 8l5 5"},
    {"text",         "M5 6V4h14v2M12 4v16M9 20h6"},
    {"note",         "M5 4h14v10l-5 6H5zM14 20v-6h5"},
    {"stamp",        "M6 20h12M8 17h8v-2H8zM10 15V9a2 2 0 114 0v6"},
    {"signature",    "M3 17c4 0 4-9 7-9s2 9 5 9 3-4 6-4"},
    {"shapes",       "M4 13h7v7H4zM12 4l5 7h-10z"},
    {"link",         "M10 14a4 4 0 005.7 0l3-3a4 4 0 00-5.7-5.7l-1 1M14 10a4 4 0 00-5.7 0l-3 3a4 4 0 005.7 5.7l1-1"},
    {"image",        "M4 5h16v14H4zM4 16l4-4 4 4 3-3 5 5 @c 9 9 1.5"},

    // --- state / misc ------------------------------------------------------
    {"sun",          "@c 12 12 4 M12 3v2M12 19v2M3 12h2M19 12h2M5.6 5.6l1.4 1.4M17 17l1.4 1.4M18.4 5.6L17 7M7 17l-1.4 1.4"},
    {"moon",         "M20 14.5A8.5 8.5 0 019.5 4a8.5 8.5 0 1010.5 10.5z"},
    {"check",        "M5 13l4 4 10-10"},
    {"plus",         "M12 5v14M5 12h14"},
    {"minus",        "M5 12h14"},
    {"star",         "M12 4l2.4 5.1 5.6.7-4.1 3.9 1.1 5.5-5-2.8-5 2.8 1.1-5.5L4 9.8l5.6-.7z"},
    {"bookmark",     "M6 4h12v16l-6-4-6 4z"},
    {"pin",          "M12 13v8M8 4h8l-1 6 3 3H6l3-3z"},
    {"lock",         "M6 11h12v9H6zM9 11V8a3 3 0 016 0v3"},
    {"eye",          "M2 12s4-6 10-6 10 6 10 6-4 6-10 6-10-6-10-6z @c 12 12 2.6"},
    {"home",         "M3 10.6L12 3.2L21 10.6M5.4 9.4V20.4H18.6V9.4M9.6 20.4V14.2H14.4V20.4"},
    {"settings",     "@c 12 12 3 M12 3v2M12 19v2M4.5 7.5l1.7 1M17.8 15.5l1.7 1M4.5 16.5l1.7-1M17.8 8.5l1.7-1"},
    {"clock",        "@c 12 12 8 M12 7v5l3 2"},
    {"undo",         "M4 8v5h5 M4.8 13a7.5 7.5 0 112 6"},
    {"redo",         "M20 8v5h-5 M19.2 13a7.5 7.5 0 10-2 6"},
    {"filter",       "M3 5h18l-7 8v6l-4-2v-4z"},
    {"layers",       "M12 3l9 5-9 5-9-5zM3 13l9 5 9-5M3 17l9 5 9-5"},
    {"outline",      "M4 6h2M9 6h11M4 12h2M9 12h11M4 18h2M9 18h11"},
    {"comment",      "M4 5h16v11H9l-5 4z"},
    {"replace",      "M4 7h9M4 7l3-3M4 7l3 3M20 17h-9M20 17l-3-3M20 17l-3 3"},
    {"scissors",     "@c 6 6 2.5 @c 6 18 2.5 M8.1 7.9L20 18M8.1 16.1L20 6"},
};

/// Expand the "@c cx cy r" shorthand and parse the rest as SVG.
std::vector<PathVerb> compile(std::string_view d) {
    std::vector<PathVerb> out;
    std::size_t i = 0;
    std::string pending;
    while (i < d.size()) {
        if (d[i] == '@' && i + 1 < d.size() && d[i + 1] == 'c') {
            if (!pending.empty()) {
                auto sub = parseSvgPath(pending);
                out.insert(out.end(), sub.begin(), sub.end());
                pending.clear();
            }
            Cursor cur{d, i + 2};
            float cx = 0, cy = 0, r = 0;
            if (cur.number(cx) && cur.number(cy) && cur.number(r)) {
                appendCircle(out, cx, cy, r);
            }
            i = cur.i;
        } else {
            pending.push_back(d[i]);
            ++i;
        }
    }
    if (!pending.empty()) {
        auto sub = parseSvgPath(pending);
        out.insert(out.end(), sub.begin(), sub.end());
    }
    return out;
}

}  // namespace

IconRegistry::IconRegistry() {
    for (const Def& def : kIcons) cache_.emplace(def.name, compile(def.d));
}

IconRegistry& IconRegistry::instance() {
    static IconRegistry r;
    return r;
}

void IconRegistry::define(std::string name, std::string_view svgPath) {
    cache_[std::move(name)] = compile(svgPath);
}

std::span<const PathVerb> IconRegistry::get(std::string_view name) {
    auto it = cache_.find(std::string(name));
    if (it == cache_.end()) {
        static std::set<std::string> warned;
        if (warned.insert(std::string(name)).second) {
            std::fprintf(stderr, "[folio] unknown icon '%.*s'\n",
                         static_cast<int>(name.size()), name.data());
        }
        return {};
    }
    return it->second;
}

std::vector<std::string> IconRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(cache_.size());
    for (const auto& [k, v] : cache_) out.push_back(k);
    return out;
}

void drawIcon(folio::render::IPaintTarget& t, std::string_view name,
              const folio::render::Rect& box, const folio::render::Color& colour,
              float strokeWidth) {
    auto verbs = IconRegistry::instance().get(name);
    if (verbs.empty()) return;

    const float s = std::min(box.w, box.h) / 24.f;
    t.save();
    t.translate(box.x + (box.w - 24.f * s) * 0.5f, box.y + (box.h - 24.f * s) * 0.5f);
    t.scale(s, s);
    // The stroke is specified in device pixels, so undo the icon scale.
    t.strokePath(verbs, colour, strokeWidth / s, folio::render::LineCap::Round,
                 folio::render::LineJoin::Round);
    t.restore();
}

}  // namespace folio::ui
