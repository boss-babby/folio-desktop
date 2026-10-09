#include "folio/ui/Draw.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <string>

namespace folio::ui {

/// Diagnostic switches, read once. Shadows and blurs are the usual
/// suspects when a CPU-rastered UI misses frame budget, and being able to
/// subtract them from a running build is worth five lines.
namespace {
bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}
const bool kNoShadows = envFlag("FOLIO_NO_SHADOWS");
}  // namespace

void drawElevation(IPaintTarget& t, const Rect& r, const Corners& radii,
                   const tokens::Elevation& e, float alpha, bool interiorOpaque) {
    if (alpha <= 0.003f || kNoShadows) return;
    for (int i = 0; i < e.count; ++i) {
        Shadow s = e.layers[static_cast<std::size_t>(i)];
        s.color = fade(s.color, alpha);
        s.interiorOpaque = interiorOpaque;
        if (s.color.a == 0) continue;

        const bool isRing = s.blur == 0.f && s.dx == 0.f && s.dy == 0.f && s.spread > 0.f;
        if (isRing) {
            // Outset by half the stroke so the ring sits outside the fill,
            // matching CSS's spread semantics.
            const Rect outer = r.inset(-s.spread * 0.5f);
            const Corners rr{radii.tl + s.spread * 0.5f, radii.tr + s.spread * 0.5f,
                             radii.br + s.spread * 0.5f, radii.bl + s.spread * 0.5f};
            t.strokeRoundedRect(outer, rr, s.color, s.spread);
        } else {
            t.drawShadow(r, radii, s);
        }
    }
}

void drawCard(IPaintTarget& t, const Rect& r, const Corners& radii, const Color& fill,
              const tokens::Elevation& e, float alpha) {
    // The fill below is opaque whenever the card itself is, which lets the
    // backend skip the shadow's interior.
    const bool opaqueFill = alpha >= 0.997f && fill.a == 255;
    drawElevation(t, r, radii, e, alpha, opaqueFill);
    t.fillRoundedRect(r, radii, alpha >= 0.997f ? fill : fade(fill, alpha));
}

void hairline(IPaintTarget& t, const Point& a, const Point& b, const Color& c,
              float dpr) {
    const float w = (dpr > 0.f) ? 1.f / dpr : 1.f;
    // Snap to the half-pixel grid; an unsnapped 1 px line straddles two
    // device pixels and renders at half opacity across both.
    const float snap = [&] {
        if (a.x == b.x) return std::floor(a.x * dpr) / dpr + w * 0.5f;
        return 0.f;
    }();
    if (a.x == b.x) {
        t.drawLine(Point{snap, a.y}, Point{snap, b.y}, c, w);
    } else if (a.y == b.y) {
        const float sy = std::floor(a.y * dpr) / dpr + w * 0.5f;
        t.drawLine(Point{a.x, sy}, Point{b.x, sy}, c, w);
    } else {
        t.drawLine(a, b, c, w);
    }
}

void dividerV(IPaintTarget& t, float x, float y0, float y1, const Color& c, float dpr) {
    hairline(t, Point{x, y0}, Point{x, y1}, c, dpr);
}

Color over(const Color& base, const Color& layer) noexcept {
    const float a = static_cast<float>(layer.a) / 255.f;
    const auto mix = [a](std::uint8_t b, std::uint8_t l) {
        return static_cast<std::uint8_t>(static_cast<float>(b) * (1.f - a) +
                                         static_cast<float>(l) * a + 0.5f);
    };
    return Color{mix(base.r, layer.r), mix(base.g, layer.g), mix(base.b, layer.b),
                 base.a};
}

void focusRing(IPaintTarget& t, const Rect& r, const Corners& radii,
               const Color& accent, float alpha) {
    if (alpha <= 0.003f) return;
    const float gap = 2.f, w = 2.f;
    const Rect outer = r.inset(-(gap + w * 0.5f));
    const float d = gap + w * 0.5f;
    const Corners rr{radii.tl + d, radii.tr + d, radii.br + d, radii.bl + d};
    t.strokeRoundedRect(outer, rr, fade(accent, alpha), w);
}

folio::render::TextStyle styleFor(const tokens::TypeScale& ts, const Color& c) noexcept {
    folio::render::TextStyle s;
    s.size = ts.size;
    s.weight = static_cast<folio::render::FontWeight>(ts.weight);
    s.letterSpacingEm = ts.letterSpacingEm;
    s.color = c;
    return s;
}

std::string upperAscii(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return out;
}

}  // namespace folio::ui
