#include "folio/ui/Animation.h"

namespace folio::ui {
namespace {

/// x(s) for the CSS bezier with implicit endpoints (0,0) and (1,1).
constexpr float bezX(float s, float x1, float x2) noexcept {
    const float u = 1.f - s;
    return 3.f * u * u * s * x1 + 3.f * u * s * s * x2 + s * s * s;
}
constexpr float bezY(float s, float y1, float y2) noexcept {
    const float u = 1.f - s;
    return 3.f * u * u * s * y1 + 3.f * u * s * s * y2 + s * s * s;
}
/// dx/ds, needed for Newton-Raphson.
constexpr float bezDX(float s, float x1, float x2) noexcept {
    const float u = 1.f - s;
    return 3.f * u * u * x1 + 6.f * u * s * (x2 - x1) + 3.f * s * s * (1.f - x2);
}

}  // namespace

float cubicBezier(float x1, float y1, float x2, float y2, float t) noexcept {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    // The identity curve - linear() and a surprising number of hand-written
    // token values - needs no solving.
    if (x1 == y1 && x2 == y2) return t;

    float s = t;  // first guess: s == x is exact for the linear curve and
                  // close for every curve CSS authors actually write.
    for (int i = 0; i < 6; ++i) {
        const float err = bezX(s, x1, x2) - t;
        if (err > -1e-6f && err < 1e-6f) return bezY(s, y1, y2);
        const float d = bezDX(s, x1, x2);
        if (d > -1e-6f && d < 1e-6f) break;  // flat: Newton would diverge
        s -= err / d;
    }
    // Bisection fallback. Only reached for pathological control points, but
    // it guarantees termination and monotonicity.
    float lo = 0.f, hi = 1.f;
    s = t;
    for (int i = 0; i < 24; ++i) {
        const float x = bezX(s, x1, x2);
        if (x < t) lo = s; else hi = s;
        s = (lo + hi) * 0.5f;
    }
    return bezY(s, y1, y2);
}

render::Color lerpColor(const render::Color& a, const render::Color& b,
                        float t) noexcept {
    const float u = clamp01(t);
    const auto mix = [u](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(
            static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * u
            + 0.5f);
    };
    return render::Color{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

tokens::Palette lerpPalette(const tokens::Palette& a, const tokens::Palette& b,
                            float t) noexcept {
    tokens::Palette p;
    p.bg            = lerpColor(a.bg, b.bg, t);
    p.card          = lerpColor(a.card, b.card, t);
    p.text          = lerpColor(a.text, b.text, t);
    p.muted         = lerpColor(a.muted, b.muted, t);
    p.border        = lerpColor(a.border, b.border, t);
    p.divider       = lerpColor(a.divider, b.divider, t);
    p.accent        = lerpColor(a.accent, b.accent, t);
    p.accentSoft    = lerpColor(a.accentSoft, b.accentSoft, t);
    p.accentText    = lerpColor(a.accentText, b.accentText, t);
    p.surfaceSunken = lerpColor(a.surfaceSunken, b.surfaceSunken, t);
    p.hover         = lerpColor(a.hover, b.hover, t);
    p.danger        = lerpColor(a.danger, b.danger, t);
    p.shadow        = lerpColor(a.shadow, b.shadow, t);
    p.ring          = lerpColor(a.ring, b.ring, t);
    p.pageShadow    = lerpColor(a.pageShadow, b.pageShadow, t);
    // Past the midpoint the theme *is* the destination, for anything that
    // branches on it (icon choice, shadow strategy).
    p.isDark = (t < 0.5f) ? a.isDark : b.isDark;
    return p;
}

}  // namespace folio::ui
