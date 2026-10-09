// folio-render/skia/SkiaTextSystem.h
//
// Font loading, caching and run drawing for the Skia backend.
//
// WHY A BUNDLED FONT
// ------------------
// The product requirement is "the same UI design everywhere". System fonts
// make that impossible: the same label is 11% wider in Helvetica Neue than in
// Segoe UI, which is enough to wrap a button caption on one platform and not
// another. design-tokens.json therefore names DM Sans (SIL OFL 1.1, so
// bundling is free of obligation beyond shipping the licence) and we load it
// from our own asset directory into an *empty* font manager. The system font
// set is never consulted for chrome, so a Windows build and a macOS build lay
// text out to the same subpixel.
//
// Fallback to the system font manager exists only so a developer who has not
// run tools/fetch-deps.sh still gets a usable window instead of a crash; it
// logs loudly, and the golden-image tests fail if it is taken.

#pragma once

#include "folio/render/IPaintTarget.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace folio::render::skia {

/// Process-wide font cache. Creating an SkFont is cheap but creating a
/// typeface with a variation position is not, and the UI asks for the same
/// half-dozen (size, weight) pairs thousands of times per frame.
class SkiaTextSystem {
public:
    /// Returns the singleton. First call must have been preceded by
    /// `initialise()`, otherwise the system-font fallback is used.
    static SkiaTextSystem& instance();

    /// Load DM Sans from `fontDir`. Returns false and arms the fallback if
    /// the file is absent or unparseable.
    bool initialise(const std::string& fontDir);

    [[nodiscard]] bool usingBundledFont() const noexcept { return bundled_; }
    [[nodiscard]] const std::string& familyName() const noexcept { return familyName_; }

    /// An SkFont for this style, cached. Subpixel + hinting settings are
    /// fixed here so that every call site is consistent.
    [[nodiscard]] const SkFont& font(float sizePx, FontWeight weight,
                                     FontFamily family = FontFamily::Sans);

    [[nodiscard]] float measure(std::string_view utf8, const TextStyle& st);
    [[nodiscard]] FontMetrics metrics(const TextStyle& st);

    /// Draw a single run. `trackingPx` is applied after every glyph, matching
    /// CSS `letter-spacing` (which, unlike typographic tracking, also spaces
    /// after the final character).
    void draw(SkCanvas* canvas, std::string_view utf8, float x, float baselineY,
              const TextStyle& st);

    /// Truncate `utf8` to fit `maxWidth`, appending a horizontal ellipsis.
    /// Cuts only on UTF-8 code point boundaries.
    [[nodiscard]] std::string elide(std::string_view utf8, const TextStyle& st,
                                    float maxWidth);

private:
    SkiaTextSystem() = default;

    struct Key {
        std::uint32_t sizeQ;   // size in 1/16 px, so 13.5 and 13.5 hash alike
        std::uint16_t weight;
        std::uint8_t  family;
        auto operator<=>(const Key&) const = default;
    };

    sk_sp<SkTypeface> typefaceFor(FontWeight w, FontFamily f = FontFamily::Sans);
    sk_sp<SkTypeface> loadFamily(FontFamily f);

    sk_sp<SkFontMgr> mgr_;
    sk_sp<SkData> fontData_;
    std::map<std::uint32_t, sk_sp<SkTypeface>> typefaces_;
    std::map<std::uint8_t, sk_sp<SkTypeface>> families_;
    std::map<Key, SkFont> fonts_;
    std::string familyName_ = "(none)";
    bool bundled_ = false;
    bool initialised_ = false;
};

}  // namespace folio::render::skia
