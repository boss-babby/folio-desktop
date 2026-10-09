// folio/core/render/TileGrid.h  --  work packages C1 / B3
//
// Page <-> device coordinate math and tile enumeration.
//
// This is the piece most likely to contain a silent off-by-one that shows up
// as a one-pixel seam between tiles at certain zoom levels - visible, annoying
// and very hard to trace back. Hence the exhaustive acceptance tests.
//
// The matrices here feed FPDF_RenderPageBitmapWithMatrix directly. PDFium has
// no rotate parameter on that call, so rotation is baked into the matrix.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "folio/core/render/TileKey.h"

namespace folio::core {

/// PDFium's FS_MATRIX. Applies:
///     x' = a*x + c*y + e
///     y' = b*x + d*y + f
struct Matrix {
    float a = 1.f, b = 0.f, c = 0.f, d = 1.f, e = 0.f, f = 0.f;

    [[nodiscard]] constexpr std::pair<float, float> apply(float x, float y) const noexcept {
        return {a * x + c * y + e, b * x + d * y + f};
    }
};

struct DeviceRect {
    std::int32_t x = 0, y = 0, w = 0, h = 0;

    [[nodiscard]] constexpr std::int32_t right()  const noexcept { return x + w; }
    [[nodiscard]] constexpr std::int32_t bottom() const noexcept { return y + h; }
    [[nodiscard]] constexpr bool empty() const noexcept { return w <= 0 || h <= 0; }
    friend bool operator==(const DeviceRect&, const DeviceRect&) = default;
};

/// One tile to render: where it lives, and the exact matrix PDFium needs.
struct TileSpec {
    std::uint16_t tx = 0, ty = 0;
    DeviceRect    rect{};    // position in the page's device space
    Matrix        matrix{};  // page points -> this tile's local bitmap space
};

/// Describes one page at one zoom and rotation.
class TileGrid {
public:
    /// `rotationQuarter` is 0/1/2/3 == 0/90/180/270 degrees clockwise and is
    /// the SUM of the PDF's own /Rotate and any user rotation.
    TileGrid(float widthPt, float heightPt, float scale,
             std::uint8_t rotationQuarter, std::int32_t tileSize = kTileSize) noexcept;

    /// Full page size in device pixels, after scale and rotation.
    [[nodiscard]] std::int32_t deviceWidth()  const noexcept { return devW_; }
    [[nodiscard]] std::int32_t deviceHeight() const noexcept { return devH_; }

    [[nodiscard]] std::uint16_t columns() const noexcept { return cols_; }
    [[nodiscard]] std::uint16_t rows()    const noexcept { return rows_; }
    [[nodiscard]] std::size_t   tileCount() const noexcept {
        return static_cast<std::size_t>(cols_) * rows_;
    }

    /// Tile rect clipped to the page - edge tiles are smaller than tileSize.
    [[nodiscard]] DeviceRect tileRect(std::uint16_t tx, std::uint16_t ty) const noexcept;

    /// Matrix mapping page points into tile-local bitmap coordinates, i.e.
    /// with the tile's device origin subtracted.
    [[nodiscard]] Matrix tileMatrix(std::uint16_t tx, std::uint16_t ty) const noexcept;

    [[nodiscard]] TileSpec tile(std::uint16_t tx, std::uint16_t ty) const noexcept;

    /// Tiles intersecting `viewport` (device space), in priority order:
    /// nearest the viewport centre first, so the user sees the middle sharpen
    /// before the edges.
    [[nodiscard]] std::vector<TileSpec> tilesForViewport(const DeviceRect& viewport) const;

    /// Whole page. Used for thumbnails and the always-resident proxy.
    [[nodiscard]] std::vector<TileSpec> allTiles() const;

    /// The matrix for rendering the whole page into one bitmap of
    /// deviceWidth() x deviceHeight(). The B3 acceptance test asserts that
    /// tiles reassemble to exactly this.
    [[nodiscard]] Matrix fullPageMatrix() const noexcept { return matrixWithOrigin(0, 0); }

    /// Matrix that puts device-space point (ox, oy) at the bitmap's (0, 0).
    /// Origins outside the page are legal and are what bledTile() uses.
    [[nodiscard]] Matrix matrixWithOrigin(std::int32_t ox, std::int32_t oy) const noexcept;

    // -----------------------------------------------------------------------
    // Bleed
    //
    // MEASURED, NOT THEORISED (tests/engine/test_pdfium_rasterizer.cpp):
    // rendering a page in 256 px tiles and stitching them does NOT reproduce
    // the single full-page render. On a 612x792 page with 36 pt text the
    // disagreement was 106 pixels, all of them in a band 9 px wide running
    // down the inside of one tile boundary, with deltas up to 135/255 - i.e.
    // plainly visible, not antialiasing noise.
    //
    // Cause: a glyph whose origin lies outside the tile is rasterised
    // differently from the same glyph drawn with its origin inside the
    // bitmap. PDFium hands FreeType a different origin and the hinting and
    // edge antialiasing come out different. It is not a clipping bug and it
    // cannot be fixed by adjusting the clip rect.
    //
    // Fix: render a margin of extra pixels on every side, then copy out only
    // the interior. Any glyph overhanging the seam then has its origin inside
    // the rendered bitmap, so it rasterises identically to the full-page case.
    //
    // The margin must exceed the largest glyph overhang at the current zoom,
    // which is why it scales with `scale` rather than being a constant.
    // -----------------------------------------------------------------------

    struct BledTile {
        DeviceRect render{};   ///< rasterise this (may extend outside the page)
        Matrix     matrix{};   ///< page points -> render-local bitmap space
        DeviceRect keep{};     ///< sub-rect of `render` to copy out, render-local
        DeviceRect dest{};     ///< where `keep` belongs in page device space
    };

    /// Recommended bleed in device pixels for this grid's zoom. Covers a
    /// 96 pt glyph's overhang at the current scale, clamped to something
    /// sane - beyond ~64 px the extra rasterising costs more than the seam.
    [[nodiscard]] std::int32_t recommendedBleed() const noexcept;

    [[nodiscard]] BledTile bledTile(std::uint16_t tx, std::uint16_t ty,
                                    std::int32_t bleed) const noexcept;

private:

    float        wPt_, hPt_, scale_;
    std::uint8_t rot_;
    std::int32_t tileSize_;
    std::int32_t devW_, devH_;
    std::uint16_t cols_, rows_;
};

}  // namespace folio::core
