// folio-render/skia/PageThemeShader.cpp

#include "PageThemeShader.h"

#include "include/core/SkData.h"
#include "include/core/SkString.h"
#include "include/core/SkM44.h"
#include "include/effects/SkRuntimeEffect.h"

#include <array>
#include <mutex>
#include <string>

namespace folio::render::skia {
namespace {

// ---------------------------------------------------------------------------
// THE SHADER
//
// Naive dark mode is `rgb = 1 - rgb`. It is wrong, and it is wrong in a way
// that is obvious the moment a document contains a photograph: faces turn
// blue. What people actually want from a dark PDF is the LUMINANCE inverted
// and the HUE left alone.
//
// So:
//   1. Compute perceptual luminance (Rec. 709 weights).
//   2. Map it onto the theme's paper->ink ramp. Luminance 1 (paper white)
//      becomes `paper`; luminance 0 (black ink) becomes `ink`. In dark mode
//      paper is near-black and ink is near-white, so this inverts - but along
//      the theme's own ramp, so the result is the designed colour pair rather
//      than whatever 1-x happens to give.
//   3. Add the pixel's own chroma back in, scaled by how saturated it was and
//      by huePreserve. Greyscale text has zero chroma and lands exactly on
//      the ink colour. A red stamp stays red.
//   4. Dim strongly saturated regions (photographs) instead of remapping
//      them, because a remapped photograph looks like a photocopy.
//
// Written against premultiplied input, which is what the tile cache stores,
// so the first thing it does is unpremultiply. Getting that wrong shows up as
// dark fringing on antialiased glyph edges - visible, and easy to misdiagnose
// as a font problem.
// ---------------------------------------------------------------------------
constexpr char kPageThemeSkSL[] = R"SKSL(
uniform half3 paper;
uniform half3 ink;
uniform half  remap;
uniform half  huePreserve;
uniform half  saturatedDim;

const half3 kLumaWeights = half3(0.2126, 0.7152, 0.0722);

half4 main(half4 inColor) {
    half a = inColor.a;
    if (a <= 0.0) {
        return inColor;
    }

    // Premultiplied in, so recover straight colour first.
    half3 rgb = inColor.rgb / a;

    half luma = dot(rgb, kLumaWeights);
    half mx   = max(rgb.r, max(rgb.g, rgb.b));
    half mn   = min(rgb.r, min(rgb.g, rgb.b));
    half sat  = mx - mn;

    // 1-2. Luminance onto the theme ramp. luma 1 -> paper, luma 0 -> ink.
    half3 ramped = mix(ink, paper, luma);

    // 3. Put the original chroma back, proportional to how colourful the
    //    source was. Greyscale -> exactly on the ramp.
    half3 chroma = rgb - half3(luma);
    ramped += chroma * huePreserve;

    // 4. Strongly saturated pixels are artwork; dim rather than remap them,
    //    and blend back toward the original so photographs survive.
    half artwork = smoothstep(0.25, 0.65, sat);
    half3 dimmed = rgb * saturatedDim;
    ramped = mix(ramped, dimmed, artwork);

    half3 outRgb = mix(rgb, ramped, remap);
    outRgb = clamp(outRgb, 0.0, 1.0);

    return half4(outRgb * a, a);
}
)SKSL";

struct Cached {
    PageTheme             theme{};
    sk_sp<SkColorFilter>  filter;
    bool                  valid = false;
};

std::mutex                g_mutex;
sk_sp<SkRuntimeEffect>    g_effect;
std::string               g_error;
std::array<Cached, 3>     g_cache;

bool sameTheme(const PageTheme& a, const PageTheme& b) {
    auto eq = [](const Rgb& x, const Rgb& y) {
        return x.r == y.r && x.g == y.g && x.b == y.b;
    };
    return eq(a.paper, b.paper) && eq(a.ink, b.ink) && a.remap == b.remap &&
           a.huePreserve == b.huePreserve && a.saturatedDim == b.saturatedDim;
}

/// Caller holds g_mutex.
SkRuntimeEffect* effectLocked() {
    if (!g_effect && g_error.empty()) {
        auto [effect, error] =
            SkRuntimeEffect::MakeForColorFilter(SkString(kPageThemeSkSL));
        if (!effect) {
            g_error = error.isEmpty() ? "unknown SkSL compile failure" : error.c_str();
        }
        g_effect = std::move(effect);
    }
    return g_effect.get();
}

}  // namespace

const char* pageThemeSkSL() { return kPageThemeSkSL; }

bool validatePageThemeShader(const char** errorOut) {
    const std::lock_guard lock(g_mutex);
    const bool ok = effectLocked() != nullptr;
    if (errorOut) {
        *errorOut = ok ? "" : g_error.c_str();
    }
    return ok;
}

sk_sp<SkColorFilter> makePageThemeFilter(const PageTheme& theme) {
    if (theme.isIdentity()) {
        // Light mode. Returning null is not a failure - it tells the
        // compositor to bind the tile with no colour filter at all, which
        // removes a full-screen fragment pass from the common case.
        return nullptr;
    }

    const std::lock_guard lock(g_mutex);

    for (const auto& c : g_cache) {
        if (c.valid && sameTheme(c.theme, theme)) {
            return c.filter;
        }
    }

    SkRuntimeEffect* effect = effectLocked();
    if (!effect) {
        return nullptr;
    }

    SkRuntimeColorFilterBuilder builder(sk_ref_sp(effect));
    builder.uniform("paper") = SkV3{theme.paper.r, theme.paper.g, theme.paper.b};
    builder.uniform("ink") = SkV3{theme.ink.r, theme.ink.g, theme.ink.b};
    builder.uniform("remap") = theme.remap;
    builder.uniform("huePreserve") = theme.huePreserve;
    builder.uniform("saturatedDim") = theme.saturatedDim;

    sk_sp<SkColorFilter> filter = builder.makeColorFilter();

    for (auto& c : g_cache) {
        if (!c.valid) {
            c.theme = theme;
            c.filter = filter;
            c.valid = true;
            break;
        }
    }
    return filter;
}

}  // namespace folio::render::skia
