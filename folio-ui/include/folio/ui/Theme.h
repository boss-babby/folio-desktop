// folio-ui/include/folio/ui/Theme.h
//
// Light/dark theme state and the 760 ms cross-fade between them.
//
// WHY INTERPOLATE THE PALETTE RATHER THAN CROSS-FADE TWO FRAMES
// -------------------------------------------------------------
// The obvious implementation of a theme transition is to render the old
// theme to a texture, render the new one, and cross-fade. That costs a
// full-window offscreen layer and doubles paint cost for 760 ms - on a
// 1440x900 window with CPU raster that is the difference between a smooth
// transition and a visible stall.
//
// Interpolating the fifteen palette colours instead costs fifteen lerps per
// frame and paints once. It is not pixel-identical to a cross-fade where a
// widget *changes shape* between themes, but nothing in the prototype does;
// only colours change. The one thing that genuinely swaps - the sun/moon
// icon - is handled by cross-fading just those two icons.

#pragma once

#include "folio/ui/Animation.h"
#include "folio/ui/Tokens.h"

namespace folio::ui {

enum class ColorScheme : std::uint8_t { Light, Dark };

/// Page rendering theme, independent of the chrome theme: a user can read
/// sepia pages inside a dark UI. This mirrors folio-core's ColorScheme used
/// by the raster cache, and is deliberately a separate axis.
enum class PageTheme : std::uint8_t { Light, Sepia, Dark };

class Theme {
public:
    /// Begin a transition. Calling with the current scheme is a no-op, so UI
    /// code can call it unconditionally.
    /// Begin a transition. `originX/Y` are window coordinates for the centre
    /// of the circular reveal - normally the centre of the button that was
    /// pressed, so the new theme visibly comes out from under the user's
    /// cursor. Pass the default to reveal from the top-right corner.
    ///
    /// The transition is a *reveal*, not a cross-fade. The prototype wipes
    /// the new theme in behind an expanding circle (the View Transitions
    /// idiom), and a palette lerp cannot reproduce that: lerping takes every
    /// colour through a muddy mid-grey simultaneously, which is what made
    /// the old implementation look broken rather than deliberate.
    void setScheme(ColorScheme s, double now, float originX = -1.f,
                   float originY = -1.f) {
        if (s == target_) return;
        // Snapshot the palette actually on screen, so reversing mid-reveal
        // starts from what the user can see rather than from a stale value.
        outgoing_ = effective(now);
        originX_ = originX;
        originY_ = originY;
        target_ = s;
        from_ = outgoing_;
        fade_.reset(0.f);
        fade_.animateTo(1.f, tokens::dur::themeTransition, now,
                        Curve::OutCubic);
    }

    /// The palette the *outgoing* frame is painted in during a reveal.
    [[nodiscard]] tokens::Palette outgoing() const noexcept { return outgoing_; }
    /// The palette the incoming (clipped) frame is painted in.
    [[nodiscard]] tokens::Palette incoming() const noexcept {
        return (target_ == ColorScheme::Dark) ? tokens::kDark : tokens::kLight;
    }
    [[nodiscard]] float revealOriginX() const noexcept { return originX_; }
    [[nodiscard]] float revealOriginY() const noexcept { return originY_; }

    void setSchemeImmediate(ColorScheme s) {
        target_ = s;
        from_ = (s == ColorScheme::Dark) ? tokens::kDark : tokens::kLight;
        fade_.reset(1.f);
    }

    void toggle(double now) {
        setScheme(target_ == ColorScheme::Light ? ColorScheme::Dark : ColorScheme::Light,
                  now);
    }

    [[nodiscard]] ColorScheme scheme() const noexcept { return target_; }
    [[nodiscard]] bool animating(double now) const { return fade_.active(now); }

    /// The palette to paint with this frame.
    [[nodiscard]] tokens::Palette effective(double now) const {
        const tokens::Palette& to =
            (target_ == ColorScheme::Dark) ? tokens::kDark : tokens::kLight;
        const float t = fade_.value(now);
        if (t >= 0.999f) return to;
        return lerpPalette(from_, to, t);
    }

    /// 0 while fully in the previous theme, 1 when the new one has landed.
    /// Components use it to cross-fade the two theme icons.
    [[nodiscard]] float progress(double now) const { return fade_.value(now); }

    // --- page theme (separate axis) ----------------------------------------
    [[nodiscard]] PageTheme pageTheme() const noexcept { return page_; }
    void setPageTheme(PageTheme p) noexcept { page_ = p; }

    [[nodiscard]] tokens::PageThemeTokens pageTokens() const noexcept {
        switch (page_) {
            case PageTheme::Sepia: return tokens::pageTheme_sepia;
            case PageTheme::Dark:  return tokens::pageTheme_dark;
            case PageTheme::Light: break;
        }
        return tokens::pageTheme_light;
    }

private:
    ColorScheme target_ = ColorScheme::Light;
    PageTheme page_ = PageTheme::Light;
    tokens::Palette from_ = tokens::kLight;
    tokens::Palette outgoing_ = tokens::kLight;
    float originX_ = -1.f, originY_ = -1.f;
    AnimatedF fade_{1.f};
};

}  // namespace folio::ui
