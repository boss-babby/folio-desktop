// folio-ui/include/folio/ui/Draw.h
//
// Composite drawing helpers shared by every component.
//
// These exist so that "a card" means exactly one thing across 26 components.
// The prototype's CSS achieves that with a `.card` class; without an
// equivalent here, each component would re-implement the shadow stack and
// they would drift apart within a week.

#pragma once

#include "folio/render/IPaintTarget.h"
#include "folio/ui/Tokens.h"

#include <string_view>

namespace folio::ui {

using folio::render::Color;
using folio::render::Corners;
using folio::render::IPaintTarget;
using folio::render::Point;
using folio::render::Rect;
using folio::render::Shadow;

/// Paint a token elevation.
///
/// CSS box-shadow has two very different shapes that both appear in
/// design-tokens.json:
///   * a real blurred drop shadow  -> "0 6px 28px {shadow}"
///   * a hairline ring             -> "0 0 0 1px {ring}"
/// The second is a zero-blur, zero-offset, positive-spread shadow, which is
/// just an outline. Feeding it to a Gaussian blur draws nothing visible, so
/// it is detected and stroked instead.
/// `interiorOpaque` promises that the caller fills `r` opaquely straight
/// afterwards, letting the backend skip the hidden middle.
void drawElevation(IPaintTarget& t, const Rect& r, const Corners& radii,
                   const tokens::Elevation& e, float alpha = 1.f,
                   bool interiorOpaque = false);

/// Filled surface + its elevation, the `.card` of the prototype.
void drawCard(IPaintTarget& t, const Rect& r, const Corners& radii,
              const Color& fill, const tokens::Elevation& e, float alpha = 1.f);

/// A 1-device-pixel line. Takes dpr so it stays exactly one pixel on a
/// Retina display rather than becoming a blurry two-pixel smear.
void hairline(IPaintTarget& t, const Point& a, const Point& b, const Color& c,
              float dpr);

/// Horizontal / vertical divider inset from the ends, as the prototype's
/// `.v` separator does.
void dividerV(IPaintTarget& t, float x, float y0, float y1, const Color& c, float dpr);

/// Interpolate a colour's alpha without touching its RGB. Used constantly by
/// hover states, which fade a tint in rather than cross-fading two fills.
[[nodiscard]] inline Color fade(const Color& c, float a01) noexcept {
    return Color{c.r, c.g, c.b, static_cast<std::uint8_t>(
                                    static_cast<float>(c.a) * a01 + 0.5f)};
}

/// Composite `over` onto `base` in straight sRGB. Needed where a translucent
/// hover tint sits on an opaque surface and we want the resulting opaque
/// colour (cheaper than a real layer, and avoids seams at rounded corners).
[[nodiscard]] Color over(const Color& base, const Color& layer) noexcept;

/// A standard focus ring, drawn outside the control's bounds.
void focusRing(IPaintTarget& t, const Rect& r, const Corners& radii,
               const Color& accent, float alpha = 1.f);

/// Text style built from a token type scale.
[[nodiscard]] folio::render::TextStyle styleFor(const tokens::TypeScale& ts,
                                                const Color& c) noexcept;

/// Uppercase an ASCII string, for `text-transform: uppercase` section
/// headers. Deliberately ASCII-only: a locale-correct uppercase needs ICU
/// and would change string length, which the layout does not expect. The
/// strings this is applied to are UI section headers that go through
/// translation as already-uppercased text.
[[nodiscard]] std::string upperAscii(std::string_view s);

}  // namespace folio::ui
