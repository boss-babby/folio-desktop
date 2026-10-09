// folio-render/skia/SkiaTileCompositor.cpp

#include "SkiaTileCompositor.h"

#include "PageThemeShader.h"

#include "include/core/SkColorFilter.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"

#include <memory>

namespace folio::render::skia {
namespace {

/// Keeps the tile alive for exactly as long as Skia holds the pixels.
struct TileKeepAlive {
    core::TileHandle tile;
};

void releaseTile(const void*, void* ctx) {
    delete static_cast<TileKeepAlive*>(ctx);
}

/// Magnifying a cached tile from a lower zoom bucket: bilinear with a mipmap
/// is wrong (we are scaling UP), cubic is the right trade. Mitchell (1/3,1/3)
/// is the usual choice for document content - it sharpens without the halo
/// that Catmull-Rom puts on text edges.
const SkSamplingOptions kUpscale{SkCubicResampler::Mitchell()};

/// Exact-scale tiles land 1:1 on the destination, so nearest is both correct
/// and free. Using linear here would soften every glyph for no reason.
const SkSamplingOptions kExact{SkFilterMode::kNearest, SkMipmapMode::kNone};

}  // namespace

sk_sp<SkImage> imageForTile(const core::TileHandle& tile) {
    if (!tile || tile->pixels.empty()) {
        return nullptr;
    }
    // BGRA8888 premultiplied is PDFium's native output and Skia's native
    // input on little-endian. No swizzle, no conversion. See RasterTile.h.
    const SkImageInfo info = SkImageInfo::Make(
        tile->width, tile->height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    const SkPixmap pixmap(info, tile->pixels.data(), tile->stride);

    auto* keep = new TileKeepAlive{tile};
    sk_sp<SkImage> image = SkImages::RasterFromPixmap(pixmap, &releaseTile, keep);
    if (!image) {
        delete keep;
    }
    return image;
}

void SkiaTileCompositor::setTheme(const PageTheme& theme) {
    theme_ = theme;
    themeFilter_ = makePageThemeFilter(theme);
}

void SkiaTileCompositor::drawPagePlaceholder(SkCanvas& canvas, const SkRect& dest,
                                             float cornerRadius) const {
    const SkRRect rrect = SkRRect::MakeRectXY(dest, cornerRadius, cornerRadius);

    // elevation.page = "0 10px 40px {pageShadow}" from design-tokens.json.
    // CSS blur radius maps to a Gaussian sigma of radius/2, which is the
    // conversion that makes the native build match the HTML prototype
    // instead of looking "nearly right".
    SkPaint shadow;
    shadow.setAntiAlias(true);
    shadow.setColor(SkColorSetARGB(40, 0, 0, 0));
    shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 40.f / 2.f));
    canvas.drawRRect(rrect.makeOffset(0, 10), shadow);

    SkPaint paper;
    paper.setAntiAlias(true);
    paper.setColor(SkColorSetARGB(
        255, static_cast<std::uint8_t>(theme_.paper.r * 255.f + 0.5f),
        static_cast<std::uint8_t>(theme_.paper.g * 255.f + 0.5f),
        static_cast<std::uint8_t>(theme_.paper.b * 255.f + 0.5f)));
    canvas.drawRRect(rrect, paper);
}

core::TileHandle SkiaTileCompositor::lookupTile(core::RasterCache& cache,
                                                const core::TileKey& key,
                                                bool& exactOut) const {
    if (auto exact = cache.find(key)) {
        exactOut = true;
        return exact;
    }
    exactOut = false;
    return cache.findFallback(key);
}

SkiaTileCompositor::Stats SkiaTileCompositor::drawPage(
    SkCanvas& canvas, const PageDraw& draw, const core::TileGrid& grid,
    core::RasterCache& cache) {

    Stats stats;
    stats.pagesDrawn = 1;

    SkAutoCanvasRestore restore(&canvas, true);
    canvas.clipRect(draw.dest, /*doAntiAlias=*/true);

    // Page device space -> canvas space. The grid works in device pixels at
    // `scale`; the destination rect may be a different size because the
    // viewport animates zoom continuously while tiles only exist at
    // quantised buckets. That difference is the whole reason for the
    // quantisation: the GPU absorbs it as a scale on an already-rendered
    // tile, instead of PDFium re-rendering on every frame of a pinch.
    const float sx = draw.dest.width() / static_cast<float>(grid.deviceWidth());
    const float sy = draw.dest.height() / static_cast<float>(grid.deviceHeight());
    canvas.translate(draw.dest.left(), draw.dest.top());
    canvas.scale(sx, sy);

    SkPaint paint;
    paint.setAntiAlias(false);        // tiles are axis-aligned rectangles
    paint.setColorFilter(themeFilter_);
    paint.setBlendMode(SkBlendMode::kSrcOver);

    for (std::uint16_t ty = 0; ty < grid.rows(); ++ty) {
        for (std::uint16_t tx = 0; tx < grid.columns(); ++tx) {
            core::TileKey key{};
            key.pageId = draw.page;
            key.tileX = tx;
            key.tileY = ty;
            key.scaleQ = core::quantiseScale(draw.scale);
            key.rotation = draw.rotation;
            key.scheme = core::ColorScheme::Light;   // see note below

            // NOTE: the cache key always says Light. The theme is applied by
            // the shader above, so there is exactly ONE cached rendering of
            // each tile regardless of theme. Keying by scheme as well would
            // triple the cache footprint to store three copies of identical
            // pixels. ColorScheme survives in TileKey only for the rare case
            // of a document that must be re-rendered per theme (tagged PDFs
            // with colour-dependent OCGs); nothing uses it yet.

            bool exact = false;
            const core::TileHandle tile = lookupTile(cache, key, exact);
            if (!tile) {
                ++stats.tilesMissing;
                continue;
            }

            sk_sp<SkImage> image = imageForTile(tile);
            if (!image) {
                ++stats.tilesMissing;
                continue;
            }

            const core::DeviceRect r = grid.tileRect(tx, ty);
            const SkRect dst = SkRect::MakeXYWH(
                static_cast<float>(r.x), static_cast<float>(r.y),
                static_cast<float>(r.w), static_cast<float>(r.h));

            if (exact && tile->width == r.w && tile->height == r.h) {
                canvas.drawImageRect(image, dst, kExact, &paint);
                ++stats.tilesExact;
            } else {
                // Fallback: the tile came from another zoom bucket, or it was
                // rendered with bleed and is larger than its destination.
                // Source rect selects the interior.
                const float bx = static_cast<float>(tile->width - r.w) * 0.5f;
                const float by = static_cast<float>(tile->height - r.h) * 0.5f;
                const SkRect src =
                    (bx > 0.f || by > 0.f)
                        ? SkRect::MakeXYWH(bx, by, static_cast<float>(r.w),
                                           static_cast<float>(r.h))
                        : SkRect::MakeWH(static_cast<float>(tile->width),
                                         static_cast<float>(tile->height));
                canvas.drawImageRect(image, src, dst, kUpscale, &paint,
                                     SkCanvas::kStrict_SrcRectConstraint);
                ++stats.tilesFallback;
            }
            ++stats.tilesDrawn;
        }
    }

    last_ = stats;
    return stats;
}

}  // namespace folio::render::skia
