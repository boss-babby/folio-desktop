#include "folio/core/render/TileGrid.h"

#include <algorithm>
#include <cmath>

namespace folio::core {

namespace {
[[nodiscard]] std::int32_t ceilDiv(std::int32_t a, std::int32_t b) noexcept {
    return (a + b - 1) / b;
}
}  // namespace

TileGrid::TileGrid(float widthPt, float heightPt, float scale,
                   std::uint8_t rotationQuarter, std::int32_t tileSize) noexcept
    : wPt_(widthPt), hPt_(heightPt), scale_(scale),
      rot_(static_cast<std::uint8_t>(rotationQuarter & 3u)), tileSize_(tileSize) {

    const auto pw = static_cast<std::int32_t>(std::lround(wPt_ * scale_));
    const auto ph = static_cast<std::int32_t>(std::lround(hPt_ * scale_));

    // 90 and 270 swap the page's device footprint.
    const bool swap = (rot_ == 1 || rot_ == 3);
    devW_ = swap ? ph : pw;
    devH_ = swap ? pw : ph;

    cols_ = static_cast<std::uint16_t>(std::max(1, ceilDiv(devW_, tileSize_)));
    rows_ = static_cast<std::uint16_t>(std::max(1, ceilDiv(devH_, tileSize_)));
}

DeviceRect TileGrid::tileRect(std::uint16_t tx, std::uint16_t ty) const noexcept {
    const std::int32_t x = static_cast<std::int32_t>(tx) * tileSize_;
    const std::int32_t y = static_cast<std::int32_t>(ty) * tileSize_;
    // Edge tiles are clipped, never padded - padding would waste both render
    // time and texture memory on the right and bottom edges of every page.
    return DeviceRect{x, y,
                      std::min(tileSize_, devW_ - x),
                      std::min(tileSize_, devH_ - y)};
}

Matrix TileGrid::matrixWithOrigin(std::int32_t ox, std::int32_t oy) const noexcept {
    const float s = scale_;
    const float W = wPt_ * s;  // unrotated device width
    const float H = hPt_ * s;  // unrotated device height
    const auto fx = static_cast<float>(ox);
    const auto fy = static_cast<float>(oy);

    // Each case maps page points into the rotated device space, then shifts
    // by the tile origin so the tile renders at (0,0) of its own bitmap.
    switch (rot_) {
        case 1:  // 90 CW:   (x,y) -> (H - y, x)
            return Matrix{0.f,  s,  -s, 0.f, H - fx,     -fy};
        case 2:  // 180:     (x,y) -> (W - x, H - y)
            return Matrix{ -s, 0.f, 0.f, -s,  W - fx, H - fy};
        case 3:  // 270 CW:  (x,y) -> (y, W - x)
            return Matrix{0.f, -s,   s, 0.f,    -fx,  W - fy};
        case 0:
        default: // identity
            return Matrix{  s, 0.f, 0.f,  s,    -fx,     -fy};
    }
}

Matrix TileGrid::tileMatrix(std::uint16_t tx, std::uint16_t ty) const noexcept {
    return matrixWithOrigin(static_cast<std::int32_t>(tx) * tileSize_,
                            static_cast<std::int32_t>(ty) * tileSize_);
}

TileSpec TileGrid::tile(std::uint16_t tx, std::uint16_t ty) const noexcept {
    return TileSpec{tx, ty, tileRect(tx, ty), tileMatrix(tx, ty)};
}

std::vector<TileSpec> TileGrid::allTiles() const {
    std::vector<TileSpec> out;
    out.reserve(tileCount());
    for (std::uint16_t ty = 0; ty < rows_; ++ty)
        for (std::uint16_t tx = 0; tx < cols_; ++tx)
            out.push_back(tile(tx, ty));
    return out;
}

std::vector<TileSpec> TileGrid::tilesForViewport(const DeviceRect& vp) const {
    std::vector<TileSpec> out;
    if (vp.empty()) return out;

    const std::int32_t x0 = std::max(0, vp.x / tileSize_);
    const std::int32_t y0 = std::max(0, vp.y / tileSize_);
    const std::int32_t x1 = std::min<std::int32_t>(cols_ - 1, (vp.right()  - 1) / tileSize_);
    const std::int32_t y1 = std::min<std::int32_t>(rows_ - 1, (vp.bottom() - 1) / tileSize_);
    if (x1 < x0 || y1 < y0) return out;

    out.reserve(static_cast<std::size_t>((x1 - x0 + 1) * (y1 - y0 + 1)));
    for (std::int32_t ty = y0; ty <= y1; ++ty)
        for (std::int32_t tx = x0; tx <= x1; ++tx)
            out.push_back(tile(static_cast<std::uint16_t>(tx),
                               static_cast<std::uint16_t>(ty)));

    // Centre-out ordering: the eye lands in the middle of the viewport, so
    // that is what should sharpen first.
    const float cx = static_cast<float>(vp.x) + static_cast<float>(vp.w) * 0.5f;
    const float cy = static_cast<float>(vp.y) + static_cast<float>(vp.h) * 0.5f;
    std::stable_sort(out.begin(), out.end(),
                     [cx, cy](const TileSpec& l, const TileSpec& r) {
                         const float lx = static_cast<float>(l.rect.x) + static_cast<float>(l.rect.w) * 0.5f - cx;
                         const float ly = static_cast<float>(l.rect.y) + static_cast<float>(l.rect.h) * 0.5f - cy;
                         const float rx = static_cast<float>(r.rect.x) + static_cast<float>(r.rect.w) * 0.5f - cx;
                         const float ry = static_cast<float>(r.rect.y) + static_cast<float>(r.rect.h) * 0.5f - cy;
                         return (lx * lx + ly * ly) < (rx * rx + ry * ry);
                     });
    return out;
}

// ---------------------------------------------------------------------------
// Bleed. See the long comment in TileGrid.h for the measurement that forced
// this to exist.
// ---------------------------------------------------------------------------

std::int32_t TileGrid::recommendedBleed() const noexcept {
    // A glyph's ink can extend roughly one em above and below its origin, and
    // a little to either side. 96 pt is a generous upper bound for body and
    // display text in a document; anything larger is a title that will be
    // caught by the clamp and is big enough that the seam lands mid-stroke
    // where it is least visible.
    //
    // Rounded up to a multiple of 4 so tile bitmaps stay 16-byte aligned per
    // row, which matters for the SIMD paths in both PDFium and the compositor.
    constexpr float kMaxGlyphPt = 96.f;
    const auto raw = static_cast<std::int32_t>(kMaxGlyphPt * scale_ * 0.25f + 0.5f);
    const std::int32_t clamped = (raw < 8) ? 8 : ((raw > 64) ? 64 : raw);
    return (clamped + 3) & ~3;
}

TileGrid::BledTile TileGrid::bledTile(std::uint16_t tx, std::uint16_t ty,
                                      std::int32_t bleed) const noexcept {
    if (bleed < 0) {
        bleed = 0;
    }
    const DeviceRect inner = tileRect(tx, ty);

    BledTile out;
    out.dest = inner;

    // Expand without clamping to the page. Rendering outside the page is
    // harmless - it comes back as paper white - and clamping would make the
    // margin asymmetric at the edges, which is exactly where a seam would
    // then reappear.
    out.render = DeviceRect{inner.x - bleed, inner.y - bleed,
                            inner.w + 2 * bleed, inner.h + 2 * bleed};
    out.matrix = matrixWithOrigin(out.render.x, out.render.y);
    out.keep = DeviceRect{bleed, bleed, inner.w, inner.h};
    return out;
}

}  // namespace folio::core
