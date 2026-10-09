// folio/render/PageTheme.h
//
// Light / sepia / dark page rendering, expressed as data rather than as three
// code paths.
//
// THE RULE THIS FILE EXISTS TO ENFORCE: switching page theme must not
// re-rasterise anything.
//
// PDFium renders a page once, in document colours, into the tile cache. The
// theme is applied afterwards, per frame, by a fragment shader. Re-rendering
// instead would mean a cache flush and a visible stall every time someone
// toggles dark mode - and it would multiply the cache key space by three,
// which on a phone is the difference between caching three pages and caching
// one. (Architecture plan §8.4, §9.)
//
// The values below come from design-tokens.json -> pageTheme. They are
// duplicated here as a generated header in the real build (tools/token-gen,
// WP A3); this hand-written version is the shape that generator targets.

#pragma once

#include <cstdint>

namespace folio::render {

struct Rgb {
    float r = 0.f, g = 0.f, b = 0.f;

    [[nodiscard]] static constexpr Rgb fromHex(std::uint32_t hex) noexcept {
        return Rgb{static_cast<float>((hex >> 16) & 0xFF) / 255.f,
                   static_cast<float>((hex >> 8) & 0xFF) / 255.f,
                   static_cast<float>(hex & 0xFF) / 255.f};
    }
};

enum class PageThemeId : std::uint8_t { Light = 0, Sepia = 1, Dark = 2 };

/// Shader uniforms for one theme.
struct PageTheme {
    Rgb   paper{1.f, 1.f, 1.f};
    Rgb   ink{0.f, 0.f, 0.f};

    /// 0 = leave document colours alone (light).
    /// 1 = fully remap luminance onto the paper/ink ramp (sepia, dark).
    float remap = 0.f;

    /// How much of a pixel's own hue survives the remap. Pure greyscale text
    /// should land exactly on the ink colour; a photograph should stay
    /// recognisably a photograph. Saturation decides which is which.
    float huePreserve = 0.85f;

    /// Dark mode only: images and saturated artwork are dimmed rather than
    /// inverted, because inverting a photograph produces a negative and
    /// everybody notices.
    float saturatedDim = 1.f;

    [[nodiscard]] static constexpr PageTheme light() noexcept {
        PageTheme t;
        t.paper = Rgb::fromHex(0xFEFEFE);
        t.ink = Rgb::fromHex(0x2A2F46);
        t.remap = 0.f;              // identity: the document already looks right
        t.huePreserve = 1.f;
        t.saturatedDim = 1.f;
        return t;
    }

    [[nodiscard]] static constexpr PageTheme sepia() noexcept {
        PageTheme t;
        t.paper = Rgb::fromHex(0xF1E4C8);
        t.ink = Rgb::fromHex(0x4A3A22);
        t.remap = 1.f;
        t.huePreserve = 0.55f;      // warm everything, but keep images readable
        t.saturatedDim = 0.92f;
        return t;
    }

    [[nodiscard]] static constexpr PageTheme dark() noexcept {
        PageTheme t;
        t.paper = Rgb::fromHex(0x22252E);
        t.ink = Rgb::fromHex(0xE1E3EA);
        t.remap = 1.f;              // luminance inversion, not colour inversion
        t.huePreserve = 0.80f;
        t.saturatedDim = 0.78f;
        return t;
    }

    [[nodiscard]] static constexpr PageTheme forId(PageThemeId id) noexcept {
        switch (id) {
            case PageThemeId::Sepia: return sepia();
            case PageThemeId::Dark:  return dark();
            case PageThemeId::Light:
            default:                 return light();
        }
    }

    /// True when the theme is a no-op and the compositor can bind the tile
    /// directly with no shader at all. Worth checking: light mode is the
    /// common case and this removes a full-screen fragment pass from it.
    [[nodiscard]] constexpr bool isIdentity() const noexcept { return remap <= 0.f; }
};

}  // namespace folio::render
