// folio-render/skia/SkiaPaintTarget.h
//
// IPaintTarget on top of SkCanvas.
//
// Works unchanged against a CPU raster surface, a Ganesh GL/Vulkan surface or
// a Graphite surface - SkCanvas is the same object in all three cases. That
// is what makes the headless golden tests meaningful: they exercise the exact
// drawing code the GPU path runs, with no "test mode" branch anywhere.

#pragma once

#include "folio/render/IPaintTarget.h"

#include "include/core/SkCanvas.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace folio::render::skia {

class SkiaPaintTarget final : public IPaintTarget {
public:
    explicit SkiaPaintTarget(SkCanvas* canvas) : canvas_(canvas) {}

    void save() override;
    void restore() override;
    void translate(float dx, float dy) override;
    void scale(float sx, float sy) override;
    void rotateDegrees(float deg) override;
    void clipRect(const Rect& r, bool antiAlias) override;
    void clipRoundedRect(const Rect& r, const Corners& c, bool antiAlias) override;
    void saveLayerAlpha(float alpha) override;

    void clear(const Color& c) override;
    void fillRect(const Rect& r, const Color& c) override;
    void fillRoundedRect(const Rect& r, const Corners& radii, const Color& c) override;
    void strokeRoundedRect(const Rect& r, const Corners& radii, const Color& c,
                           float width) override;
    void drawShadow(const Rect& r, const Corners& radii, const Shadow& s) override;
    void fillCircle(const Point& centre, float radius, const Color& c) override;
    void drawLine(const Point& a, const Point& b, const Color& c, float width) override;
    void fillRectVerticalGradient(const Rect& r, const Color& top,
                                  const Color& bottom) override;
    void setBlendMode(BlendMode m) override;

    void strokePath(std::span<const PathVerb> verbs, const Color& c, float width,
                    LineCap cap, LineJoin join) override;
    void fillPath(std::span<const PathVerb> verbs, const Color& c) override;

    void drawText(std::string_view utf8, const Point& origin, const TextStyle& style,
                  TextBaseline baseline) override;
    void drawTextInBox(std::string_view utf8, const Rect& box, const TextStyle& style,
                       TextAlign align) override;
    [[nodiscard]] float measureText(std::string_view utf8, const TextStyle& style) override;
    [[nodiscard]] FontMetrics fontMetrics(const TextStyle& style) override;

    [[nodiscard]] int width() const override;
    [[nodiscard]] int height() const override;
    [[nodiscard]] std::string_view backendName() const override { return "skia"; }

    [[nodiscard]] SkCanvas* canvas() const noexcept { return canvas_; }

    // --- offscreen raster cache (IPaintTarget) ------------------------------
    bool drawCached(std::uint64_t key, const Rect& dst, int pxW, int pxH,
                    const std::function<void(IPaintTarget&)>& paint) override;
    // NOTE: keep the default arguments off the override; they live on the
    // interface declaration only.
    void drawImageBGRA(const void* pixels, int width, int height,
                       std::size_t stride, const Rect& dst,
                       std::uint64_t cacheKey = 0,
                       ImageFit fit = ImageFit::Final,
                       ImageTint tint = ImageTint::None) override;
    void invalidateImage(std::uint64_t cacheKey) override;

    /// Bytes currently held by cached page images, and how many.
    static std::size_t imageCacheBytes() noexcept;
    static std::size_t imageCacheCount() noexcept;
    void invalidateRasterCache() override;
    [[nodiscard]] RasterCacheStats rasterCacheStats() const override;

    /// Discard entries not touched for `maxAgeFrames` generations. Called
    /// when the app goes idle so a document left open overnight does not
    /// keep hundreds of megabytes of page bitmaps resident.
    static void trimRasterCache(std::uint64_t maxAgeFrames);
    static void setRasterCacheBudget(std::size_t bytes);

private:
    SkCanvas* canvas_ = nullptr;
    SkBlendMode blend_ = SkBlendMode::kSrcOver;
};

/// CSS blur radius -> Gaussian sigma.
///
/// A CSS `box-shadow` blur of N pixels is specified as a Gaussian with
/// standard deviation N/2. Skia's MakeBlur takes sigma directly. Getting this
/// conversion wrong is the most common reason a native port of a web design
/// looks "heavier" than the original, and it is invisible in code review
/// because both versions say 28.
[[nodiscard]] constexpr float cssBlurToSigma(float cssBlur) noexcept {
    return cssBlur * 0.5f;
}

}  // namespace folio::render::skia
