// folio-render/skia/SkiaPaintTarget.cpp

#include "SkiaPaintTarget.h"
#include "SkiaTextSystem.h"
#include "ShadowCache.h"
#include <mutex>
#include <cmath>
#include <cstdlib>
#include <map>
#include "include/core/SkSurface.h"
#include "include/core/SkSamplingOptions.h"

#include <string>

#include "include/core/SkColor.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkImage.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPath.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRRect.h"
#include "include/effects/SkGradientShader.h"

namespace folio::render::skia {
namespace {

SkColor toSk(const Color& c) {
    return SkColorSetARGB(c.a, c.r, c.g, c.b);
}

SkRect toSk(const Rect& r) {
    return SkRect::MakeXYWH(r.x, r.y, r.w, r.h);
}

SkRRect toSk(const Rect& r, const Corners& c) {
    if (c.uniform()) {
        return SkRRect::MakeRectXY(toSk(r), c.tl, c.tl);
    }
    SkRRect rr;
    const SkVector radii[4] = {{c.tl, c.tl}, {c.tr, c.tr}, {c.br, c.br}, {c.bl, c.bl}};
    rr.setRectRadii(toSk(r), radii);
    return rr;
}

SkBlendMode toSk(BlendMode m) {
    switch (m) {
        case BlendMode::Multiply: return SkBlendMode::kMultiply;
        case BlendMode::Screen:   return SkBlendMode::kScreen;
        case BlendMode::Plus:     return SkBlendMode::kPlus;
        case BlendMode::SrcOver:
        default:                  return SkBlendMode::kSrcOver;
    }
}

}  // namespace

void SkiaPaintTarget::save() { canvas_->save(); }
void SkiaPaintTarget::restore() { canvas_->restore(); }
void SkiaPaintTarget::translate(float dx, float dy) { canvas_->translate(dx, dy); }
void SkiaPaintTarget::scale(float sx, float sy) { canvas_->scale(sx, sy); }
void SkiaPaintTarget::rotateDegrees(float deg) { canvas_->rotate(deg); }

void SkiaPaintTarget::clipRect(const Rect& r, bool antiAlias) {
    canvas_->clipRect(toSk(r), antiAlias);
}

void SkiaPaintTarget::clipRoundedRect(const Rect& r, const Corners& c, bool antiAlias) {
    canvas_->clipRRect(toSk(r, c), antiAlias);
}

void SkiaPaintTarget::saveLayerAlpha(float alpha) {
    canvas_->saveLayerAlphaf(nullptr, alpha);
}

void SkiaPaintTarget::clear(const Color& c) { canvas_->clear(toSk(c)); }

void SkiaPaintTarget::fillRect(const Rect& r, const Color& c) {
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(toSk(c));
    p.setBlendMode(blend_);
    canvas_->drawRect(toSk(r), p);
}

void SkiaPaintTarget::fillRoundedRect(const Rect& r, const Corners& radii, const Color& c) {
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(toSk(c));
    p.setBlendMode(blend_);
    canvas_->drawRRect(toSk(r, radii), p);
}

void SkiaPaintTarget::strokeRoundedRect(const Rect& r, const Corners& radii,
                                        const Color& c, float width) {
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(width);
    p.setColor(toSk(c));
    p.setBlendMode(blend_);
    // CSS draws a 1px border on the INSIDE of the box; Skia centres the
    // stroke on the path. Inset by half the width so a 1px ring lands on the
    // same pixels as the prototype's `box-shadow: 0 0 0 1px`.
    canvas_->drawRRect(toSk(r.inset(width * 0.5f), radii), p);
}

void SkiaPaintTarget::drawShadow(const Rect& r, const Corners& radii, const Shadow& s) {
    if (s.color.a == 0) return;

    const Rect spread{r.x - s.spread + s.dx, r.y - s.spread + s.dy,
                      r.w + 2 * s.spread, r.h + 2 * s.spread};
    const float sigma = cssBlurToSigma(s.blur);

    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(toSk(s.color));
    p.setBlendMode(blend_);

    // No blur: an ordinary filled shape, no cache needed.
    if (sigma <= 0.f) {
        canvas_->drawRRect(toSk(spread, radii), p);
        return;
    }

    // Nine-patch fast path: see ShadowCache.h. Turns a per-frame Gaussian
    // blur into a textured blit.
    //
    // The blit happens in DEVICE space, with the tile built at device
    // resolution. That matters for two reasons:
    //
    //  * the nine-patch corner blocks then map 1:1 onto device pixels, so
    //    nearest-neighbour sampling is exact rather than merely close, and
    //    nearest measured 6 ms/frame cheaper than linear on this screen;
    //  * a shadow at dpr 2 is blurred at twice the sigma into twice the
    //    tile, instead of magnifying a dpr-1 tile and going soft.
    //
    // Drawing in local space with a scaled canvas would get both wrong.
    // FOLIO_SHADOW_DIRECT=1 forces the reference path, for A/B checks.
    static const bool kDirect = std::getenv("FOLIO_SHADOW_DIRECT") != nullptr;
    const SkMatrix m = canvas_->getTotalMatrix();
    const bool axisAligned =
        m.getSkewX() == 0.f && m.getSkewY() == 0.f && m.getScaleX() > 0.f &&
        std::abs(m.getScaleX() - m.getScaleY()) < 1e-4f;

    if (radii.uniform() && axisAligned && !kDirect) {
        const float sc = m.getScaleX();
        // CSS spread grows the corner radius along with the box.
        const float effR = (radii.tl + s.spread) * sc;
        SkRect dev = m.mapRect(toSk(spread));

        if (const auto* tile =
                ShadowCache::instance().get(effR, sigma * sc, dev.width(),
                                            dev.height())) {
            // Skip the interior when the caller is about to cover it with
            // an opaque fill: on the page shadow that is ~790k pixels of
            // blended output discarded by the very next draw call, and
            // measured at 13.7 ms/frame. Inset by a pixel so the shape's
            // own antialiased edge still has shadow beneath it.
            //
            // Clip first, while the canvas still holds the caller's
            // matrix; clips are retained in device space across the reset.
            canvas_->save();
            if (s.interiorOpaque)
                canvas_->clipRRect(toSk(r.inset(1.f), radii), SkClipOp::kDifference,
                                   true);
            canvas_->resetMatrix();

            // Snap to whole device pixels so the 1:1 corner mapping is
            // exact. A sub-pixel shift of a soft shadow is invisible; a
            // resampled corner is not.
            const float pad = static_cast<float>(tile->pad);
            const SkRect dst =
                SkRect::MakeLTRB(std::round(dev.fLeft) - pad, std::round(dev.fTop) - pad,
                                 std::round(dev.fRight) + pad,
                                 std::round(dev.fBottom) + pad);
            const SkIRect centre = SkIRect::MakeLTRB(tile->cornerX, tile->cornerY,
                                                     tile->cornerX + 1,
                                                     tile->cornerY + 1);

            // The tile is black at varying alpha. kSrcIn swaps in the
            // shadow colour while keeping the alpha profile, which is what
            // lets one tile serve every colour and every step of a fade.
            // Do not replace this with setColor: tinting an image needs a
            // colour filter, and an alpha-only tile relying on paint colour
            // drew nothing at all.
            SkPaint tintP;
            tintP.setBlendMode(blend_);
            tintP.setColorFilter(
                SkColorFilters::Blend(toSk(s.color), SkBlendMode::kSrcIn));
            canvas_->drawImageNine(tile->image.get(), centre, dst,
                                   SkFilterMode::kNearest, &tintP);

            canvas_->restore();
            return;
        }
    }

    // Fallback: asymmetric corners, or a shape too small to stretch.
    p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, sigma));
    canvas_->drawRRect(toSk(spread, radii), p);
}

void SkiaPaintTarget::fillCircle(const Point& centre, float radius, const Color& c) {
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(toSk(c));
    p.setBlendMode(blend_);
    canvas_->drawCircle(centre.x, centre.y, radius, p);
}

void SkiaPaintTarget::drawLine(const Point& a, const Point& b, const Color& c, float width) {
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(toSk(c));
    p.setStrokeWidth(width);
    p.setStrokeCap(SkPaint::kRound_Cap);
    p.setBlendMode(blend_);
    canvas_->drawLine(a.x, a.y, b.x, b.y, p);
}

void SkiaPaintTarget::fillRectVerticalGradient(const Rect& r, const Color& top,
                                               const Color& bottom) {
    if (r.empty()) return;

    // A two-stop vertical gradient evaluated by the shader costs about
    // 8 ns/pixel on a CPU rasteriser - measured at 7.0 ms for the reader's
    // backdrop alone, against 0.8 ns/pixel for an opaque solid fill. Over a
    // 1100x790 backdrop that is most of a 60 FPS frame budget spent on a
    // ramp that only varies along one axis.
    //
    // So the ramp is built once into a 1 x 256 image and stretched. The
    // horizontal stretch of a one-texel-wide image is a fast path (every
    // output pixel in a row samples the same texel), and the vertical
    // filtering is linear interpolation between adjacent texels, which is
    // exactly what the gradient was computing anyway.
    //
    // 256 steps is enough that banding stays below the quantisation of an
    // 8-bit channel for any ramp shorter than 256 * 255 pixels.
    struct RampKey {
        std::uint32_t top, bottom;
        auto operator<=>(const RampKey&) const = default;
    };
    static std::map<RampKey, sk_sp<SkImage>> ramps;
    static std::mutex rampMu;

    const SkColor c0 = toSk(top), c1 = toSk(bottom);
    const RampKey key{static_cast<std::uint32_t>(c0), static_cast<std::uint32_t>(c1)};

    sk_sp<SkImage> ramp;
    {
        std::lock_guard<std::mutex> lk(rampMu);
        if (auto it = ramps.find(key); it != ramps.end()) {
            ramp = it->second;
        } else {
            constexpr int kSteps = 256;
            const SkImageInfo info =
                SkImageInfo::Make(1, kSteps, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
            if (sk_sp<SkSurface> surf = SkSurfaces::Raster(info)) {
                const SkPoint pts[2] = {{0.f, 0.f}, {0.f, static_cast<float>(kSteps)}};
                const SkColor colors[2] = {c0, c1};
                SkPaint gp;
                gp.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2,
                                                          SkTileMode::kClamp));
                surf->getCanvas()->drawPaint(gp);
                ramp = surf->makeImageSnapshot();
                // Unbounded growth is not a risk: the palette has fifteen
                // colours and the UI uses two ramps, but cap it anyway so a
                // future animated gradient cannot leak.
                if (ramps.size() < 64) ramps.emplace(key, ramp);
            }
        }
    }

    SkPaint p;
    p.setBlendMode(blend_);
    if (!ramp) {
        // Fallback to the direct shader if the ramp could not be built.
        const SkPoint pts[2] = {{r.x, r.y}, {r.x, r.bottom()}};
        const SkColor colors[2] = {c0, c1};
        p.setAntiAlias(true);
        p.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2,
                                                 SkTileMode::kClamp));
        canvas_->drawRect(toSk(r), p);
        return;
    }
    canvas_->drawImageRect(ramp.get(), toSk(r),
                           SkSamplingOptions(SkFilterMode::kLinear), &p);
}

void SkiaPaintTarget::setBlendMode(BlendMode m) { blend_ = toSk(m); }

int SkiaPaintTarget::width() const { return canvas_->imageInfo().width(); }
int SkiaPaintTarget::height() const { return canvas_->imageInfo().height(); }

}  // namespace folio::render::skia

// ---------------------------------------------------------------------------
// Text
//
// All four entry points funnel into SkiaTextSystem so that measurement and
// drawing can never disagree about which SkFont they are talking about.
// ---------------------------------------------------------------------------

namespace folio::render::skia {

void SkiaPaintTarget::drawText(std::string_view utf8, const Point& origin,
                               const TextStyle& style, TextBaseline baseline) {
    auto& ts = SkiaTextSystem::instance();
    float y = origin.y;
    if (baseline != TextBaseline::Alphabetic) {
        const FontMetrics m = ts.metrics(style);
        // Top:    shift down by the ascent so the em box starts at origin.y.
        // Middle: centre the ascent..descent band on origin.y. Using the band
        //         rather than the full line height is what makes a 13 px label
        //         look optically centred in a 34 px control.
        y += (baseline == TextBaseline::Top) ? -m.ascent
                                             : (-m.ascent - m.lineHeight() * 0.5f
                                                + m.lineGap * 0.5f);
    }
    ts.draw(canvas_, utf8, origin.x, y, style);
}

void SkiaPaintTarget::drawTextInBox(std::string_view utf8, const Rect& box,
                                    const TextStyle& style, TextAlign align) {
    if (box.empty()) return;
    auto& ts = SkiaTextSystem::instance();
    const std::string shown = ts.elide(utf8, style, box.w);
    if (shown.empty()) return;

    const float w = ts.measure(shown, style);
    float x = box.x;
    if (align == TextAlign::Centre) x = box.x + (box.w - w) * 0.5f;
    else if (align == TextAlign::Right) x = box.right() - w;

    const FontMetrics m = ts.metrics(style);
    const float y = box.y + (box.h - (-m.ascent + m.descent)) * 0.5f - m.ascent;
    ts.draw(canvas_, shown, x, y, style);
}

float SkiaPaintTarget::measureText(std::string_view utf8, const TextStyle& style) {
    return SkiaTextSystem::instance().measure(utf8, style);
}

FontMetrics SkiaPaintTarget::fontMetrics(const TextStyle& style) {
    return SkiaTextSystem::instance().metrics(style);
}

}  // namespace folio::render::skia

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

namespace folio::render::skia {
namespace {

SkPath buildPath(std::span<const PathVerb> verbs) {
    SkPath p;
    for (const PathVerb& v : verbs) {
        switch (v.op) {
            case PathVerb::Op::MoveTo:  p.moveTo(v.x0, v.y0); break;
            case PathVerb::Op::LineTo:  p.lineTo(v.x0, v.y0); break;
            case PathVerb::Op::QuadTo:  p.quadTo(v.x0, v.y0, v.x1, v.y1); break;
            case PathVerb::Op::CubicTo: p.cubicTo(v.x0, v.y0, v.x1, v.y1, v.x2, v.y2); break;
            case PathVerb::Op::Close:   p.close(); break;
        }
    }
    return p;
}

SkPaint::Cap toSk(LineCap c) {
    switch (c) {
        case LineCap::Butt:   return SkPaint::kButt_Cap;
        case LineCap::Square: return SkPaint::kSquare_Cap;
        case LineCap::Round:  break;
    }
    return SkPaint::kRound_Cap;
}

SkPaint::Join toSk(LineJoin j) {
    switch (j) {
        case LineJoin::Miter: return SkPaint::kMiter_Join;
        case LineJoin::Bevel: return SkPaint::kBevel_Join;
        case LineJoin::Round: break;
    }
    return SkPaint::kRound_Join;
}

}  // namespace

void SkiaPaintTarget::strokePath(std::span<const PathVerb> verbs, const Color& c,
                                 float width, LineCap cap, LineJoin join) {
    if (verbs.empty()) return;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(width);
    paint.setStrokeCap(toSk(cap));
    paint.setStrokeJoin(toSk(join));
    paint.setColor(SkColorSetARGB(c.a, c.r, c.g, c.b));
    paint.setBlendMode(blend_);
    canvas_->drawPath(buildPath(verbs), paint);
}

void SkiaPaintTarget::fillPath(std::span<const PathVerb> verbs, const Color& c) {
    if (verbs.empty()) return;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kFill_Style);
    paint.setColor(SkColorSetARGB(c.a, c.r, c.g, c.b));
    paint.setBlendMode(blend_);
    canvas_->drawPath(buildPath(verbs), paint);
}


// ============================================================================
// Offscreen raster cache
// ============================================================================
//
// The reader's hot loop used to be "re-run every vector draw call for every
// visible page, every frame". A page of body text is on the order of a
// thousand glyph and rect operations; at 60 Hz with four pages on screen that
// is a quarter of a million draw calls a second to produce pixels that did
// not change. This replaces that with one textured blit per page.
//
// The cache is keyed on content + scale bucket + theme, holds device-resolution
// SkImages, and is bounded by a byte budget with strict LRU eviction. It is
// process-wide (static) rather than per-target because a target is constructed
// per frame around the current SkCanvas; the pixels outlive it.

namespace {

struct RasterEntry {
    sk_sp<SkImage> image;
    std::size_t bytes = 0;
    std::uint64_t lastUse = 0;
};

std::mutex gRasterMu;
std::map<std::uint64_t, RasterEntry> gRaster;
std::uint64_t gRasterClock = 0;
std::size_t gRasterBytes = 0;
// Keep the Skia-owned caches separate from PDFium's budget, but account for
// both: they may contain copies of the same displayed page.
#if defined(__ANDROID__)
std::size_t gRasterBudget = 16u * 1024u * 1024u;
#else
std::size_t gRasterBudget = 32u * 1024u * 1024u;
#endif
std::size_t gRasterHits = 0, gRasterMisses = 0, gRasterEvictions = 0;

// Evict least-recently-used until we fit. Called with gRasterMu held.
void evictLocked(std::size_t need) {
    while (gRasterBytes + need > gRasterBudget && !gRaster.empty()) {
        auto victim = gRaster.begin();
        for (auto it = gRaster.begin(); it != gRaster.end(); ++it)
            if (it->second.lastUse < victim->second.lastUse) victim = it;
        gRasterBytes -= victim->second.bytes;
        gRaster.erase(victim);
        ++gRasterEvictions;
    }
}

}  // namespace

bool SkiaPaintTarget::drawCached(std::uint64_t key, const Rect& dst, int pxW, int pxH,
                                 const std::function<void(IPaintTarget&)>& paint) {
    if (pxW <= 0 || pxH <= 0 || dst.empty()) return false;

    // A/B switch so the cache's contribution can be measured, not asserted.
    static const bool kDisabled = std::getenv("FOLIO_NO_RASTER_CACHE") != nullptr;
    if (kDisabled) {
        save();
        clipRect(dst, false);
        translate(dst.x, dst.y);
        scale(dst.w / static_cast<float>(pxW), dst.h / static_cast<float>(pxH));
        paint(*this);
        restore();
        return false;
    }

    // A single page larger than a quarter of the budget would thrash the
    // cache on its own; draw it straight through instead.
    const std::size_t need = static_cast<std::size_t>(pxW) *
                             static_cast<std::size_t>(pxH) * 4u;
    if (need * 4u > gRasterBudget) {
        save();
        clipRect(dst, false);
        translate(dst.x, dst.y);
        scale(dst.w / static_cast<float>(pxW), dst.h / static_cast<float>(pxH));
        paint(*this);
        restore();
        return false;
    }

    sk_sp<SkImage> img;
    bool hit = false;
    {
        std::lock_guard<std::mutex> lk(gRasterMu);
        auto it = gRaster.find(key);
        if (it != gRaster.end() && it->second.image &&
            it->second.image->width() == pxW && it->second.image->height() == pxH) {
            it->second.lastUse = ++gRasterClock;
            img = it->second.image;
            hit = true;
            ++gRasterHits;
        }
    }

    if (!img) {
        // Miss: rasterise into an offscreen surface whose coordinate space is
        // the bitmap itself, so the caller draws in device pixels and text is
        // hinted at the resolution it will actually be shown at.
        auto surf = SkSurfaces::Raster(
            SkImageInfo::Make(pxW, pxH, kBGRA_8888_SkColorType, kPremul_SkAlphaType));
        if (!surf) return false;
        surf->getCanvas()->clear(SK_ColorTRANSPARENT);
        {
            SkiaPaintTarget sub(surf->getCanvas());
            paint(sub);
        }
        img = surf->makeImageSnapshot();
        if (!img) return false;

        std::lock_guard<std::mutex> lk(gRasterMu);
        ++gRasterMisses;
        evictLocked(need);
        auto& e = gRaster[key];
        gRasterBytes -= e.bytes;  // replacing an entry of a different size
        e.image = img;
        e.bytes = need;
        e.lastUse = ++gRasterClock;
        gRasterBytes += need;
    }

    SkPaint p;
    p.setAntiAlias(true);
    // Exact 1:1 wants no resampling at all; a stretched cache entry (mid-zoom)
    // gets a cubic so it softens rather than aliases.
    const float deviceScale = canvas_->getTotalMatrix().getScaleX();
    const bool exact = std::abs(dst.w * deviceScale - static_cast<float>(pxW)) < 0.6f;
    const SkSamplingOptions sampling =
        exact ? SkSamplingOptions(SkFilterMode::kNearest)
              : SkSamplingOptions(SkCubicResampler::Mitchell());
    canvas_->drawImageRect(img, SkRect::MakeXYWH(dst.x, dst.y, dst.w, dst.h),
                           sampling, &p);
    return hit;
}

namespace {

/// Cached page images, keyed by the caller's promise-key.
///
/// Wrapping a pixmap in an SkImage costs a full copy of the bitmap. Doing
/// that per frame is multiple megabytes of memcpy at 60 Hz for one page -
/// it dominated frame time before this cache existed. Doing it once per
/// unique (page, size) makes the steady-state blit a map lookup.
struct ImageEntry {
    sk_sp<SkImage> image;
    std::size_t    bytes = 0;
    std::uint64_t  lastUsed = 0;
};
std::map<std::uint64_t, ImageEntry> g_images;
std::uint64_t g_imageClock = 0;
std::size_t   g_imageBytes = 0;
#if defined(__ANDROID__)
constexpr std::size_t kImageBudget = 24u * 1024u * 1024u;
#else
constexpr std::size_t kImageBudget = 40u * 1024u * 1024u;
#endif

void evictImages() {
    while (g_imageBytes > kImageBudget && g_images.size() > 1) {
        auto oldest = g_images.begin();
        for (auto it = std::next(g_images.begin()); it != g_images.end(); ++it)
            if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
        g_imageBytes -= oldest->second.bytes;
        g_images.erase(oldest);
    }
}

}  // namespace

std::size_t SkiaPaintTarget::imageCacheBytes() noexcept { return g_imageBytes; }
std::size_t SkiaPaintTarget::imageCacheCount() noexcept { return g_images.size(); }

void SkiaPaintTarget::invalidateImage(std::uint64_t cacheKey) {
    if (auto it = g_images.find(cacheKey); it != g_images.end()) {
        g_imageBytes -= it->second.bytes;
        g_images.erase(it);
    }
}

namespace {

// Page-theme colour filters. Built once and reused: an SkColorFilter is
// immutable and cheap to share, but constructing one per blit would allocate
// on every frame of a scroll.
sk_sp<SkColorFilter> tintFilter(folio::render::IPaintTarget::ImageTint t) {
    using Tint = folio::render::IPaintTarget::ImageTint;
    static const sk_sp<SkColorFilter> kSepia = [] {
        // Multiply by the sepia paper colour (0xF6 0xEC 0xD8). White paper
        // warms to exactly that; black ink stays black; a red stamp stays
        // red, just a shade warmer. This is the same transform a physical
        // cream stock applies, which is why it reads as "paper" and not as
        // "orange filter".
        float m[20] = {
            0xF6 / 255.f, 0, 0, 0, 0,
            0, 0xEC / 255.f, 0, 0, 0,
            0, 0, 0xD8 / 255.f, 0, 0,
            0, 0, 0, 1, 0,
        };
        return SkColorFilters::Matrix(m);
    }();
    static const sk_sp<SkColorFilter> kInvert = [] {
        // Duotone: luminance drives a ramp from ink (light) to paper (dark).
        // out = ink + L * (paper - ink), with L the Rec.709 luminance.
        // Straight inversion (1 - c) turns every photograph into a negative;
        // this keeps images legible and text crisply light-on-dark.
        constexpr float pr = 0x15 / 255.f, pg = 0x17 / 255.f, pb = 0x1C / 255.f;
        constexpr float ir = 0xE8 / 255.f, ig = 0xE8 / 255.f, ib = 0xEC / 255.f;
        constexpr float lr = 0.2126f, lg = 0.7152f, lb = 0.0722f;
        const float dr = pr - ir, dg = pg - ig, db = pb - ib;
        float m[20] = {
            lr * dr, lg * dr, lb * dr, 0, ir,
            lr * dg, lg * dg, lb * dg, 0, ig,
            lr * db, lg * db, lb * db, 0, ib,
            0,       0,       0,       1, 0,
        };
        return SkColorFilters::Matrix(m);
    }();
    switch (t) {
        case Tint::Sepia:  return kSepia;
        case Tint::Invert: return kInvert;
        case Tint::None:   break;
    }
    return nullptr;
}

}  // namespace

void SkiaPaintTarget::drawImageBGRA(const void* pixels, int width, int height,
                                    std::size_t stride, const Rect& dst,
                                    std::uint64_t cacheKey, ImageFit fit,
                                    ImageTint tint) {
    if (!pixels || width <= 0 || height <= 0) return;

    // PDFium's FPDFBitmap_BGRA is byte-for-byte kBGRA_8888_SkColorType, and
    // FPDFBitmap_FillRect leaves it premultiplied. No conversion, no swizzle.
    const SkImageInfo info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                                               kPremul_SkAlphaType);
    SkPixmap pm(info, pixels, stride);

    sk_sp<SkImage> img;
    if (cacheKey != 0) {
        auto it = g_images.find(cacheKey);
        if (it != g_images.end() && it->second.image &&
            it->second.image->width() == width &&
            it->second.image->height() == height) {
            it->second.lastUsed = ++g_imageClock;
            img = it->second.image;
        } else {
            img = SkImages::RasterFromPixmapCopy(pm);
            if (!img) return;
            if (it != g_images.end()) g_imageBytes -= it->second.bytes;
            const std::size_t sz = static_cast<std::size_t>(height) * stride;
            g_images[cacheKey] = ImageEntry{img, sz, ++g_imageClock};
            g_imageBytes += sz;
            evictImages();
        }
    } else {
        // No lifetime promise from the caller, so copy every time.
        img = SkImages::RasterFromPixmapCopy(pm);
    }
    if (!img) return;

    SkPaint paint;
    paint.setAntiAlias(false);
    paint.setColorFilter(tintFilter(tint));
    const SkRect d = SkRect::MakeXYWH(dst.x, dst.y, dst.w, dst.h);

    // Sampling choice dominates frame time here - a cubic resample of a
    // multi-megapixel page measured 25ms/frame against under 1ms for
    // nearest, so this ladder is a performance decision as much as a
    // quality one.
    //
    //   within a pixel and a half : nearest. Sub-pixel rounding between the
    //       layout rectangle and the integer bitmap is not worth a resample,
    //       and a tight tolerance here was the cliff that made zooming cost
    //       3.6x what it should.
    //   mild rescale : bilinear. This is the mid-gesture case where the
    //       stage holds its raster scale and the bitmap is deliberately the
    //       wrong size for a frame or two. It is about to be replaced; it
    //       only has to look unobjectionable, not perfect.
    //   mild rescale is the NORMAL settled state, not an exception: raster
    //       scale is quantised to kScaleQuantum steps, so the bitmap is
    //       almost never exactly the destination size. Sending that case to
    //       a cubic resampler cost 6ms on every idle frame.
    //   2x or more, and final : Mitchell. That only happens when a page hit
    //       the long-edge cap and is being genuinely magnified - a steady
    //       state the user sits and reads, so the cost is justified.
    const float dw = std::max(1.0f, dst.w), dh = std::max(1.0f, dst.h);
    const bool exact = std::abs(static_cast<float>(width) - dw) <= 1.5f &&
                       std::abs(static_cast<float>(height) - dh) <= 1.5f;
    const float ratio = dw / static_cast<float>(width);
    SkSamplingOptions sampling;
    if (exact) {
        sampling = SkSamplingOptions(SkFilterMode::kNearest);
    } else if (fit == ImageFit::Final && ratio >= 2.0f) {
        sampling = SkSamplingOptions(SkCubicResampler::Mitchell());
    } else {
        sampling = SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone);
    }
    canvas_->drawImageRect(img, d, sampling, &paint);
}

void SkiaPaintTarget::invalidateRasterCache() {
    {
        std::lock_guard<std::mutex> lk(gRasterMu);
        gRaster.clear();
        gRasterBytes = 0;
    }
    // Also release Skia's copied PDF images when the document is closed or
    // replaced; otherwise this global cache retains tens of MiB after the
    // PDFium page source and its mapped input have been destroyed.
    g_images.clear();
    g_imageBytes = 0;
}

void SkiaPaintTarget::trimRasterCache(std::uint64_t maxAgeFrames) {
    std::lock_guard<std::mutex> lk(gRasterMu);
    for (auto it = gRaster.begin(); it != gRaster.end();) {
        if (gRasterClock - it->second.lastUse > maxAgeFrames) {
            gRasterBytes -= it->second.bytes;
            it = gRaster.erase(it);
            ++gRasterEvictions;
        } else {
            ++it;
        }
    }
}

void SkiaPaintTarget::setRasterCacheBudget(std::size_t bytes) {
    std::lock_guard<std::mutex> lk(gRasterMu);
    gRasterBudget = bytes;
    evictLocked(0);
}

IPaintTarget::RasterCacheStats SkiaPaintTarget::rasterCacheStats() const {
    std::lock_guard<std::mutex> lk(gRasterMu);
    return {gRaster.size(), gRasterBytes, gRasterBudget,
            gRasterHits, gRasterMisses, gRasterEvictions};
}


}  // namespace folio::render::skia
