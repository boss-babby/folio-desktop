// folio-render/skia/SkiaTileCompositor.h  --  work package F2 (Skia backend)
//
// Draws the pages. Everything else in the UI is chrome; this is the part the
// user is actually looking at.
//
// Three properties it has to have, in priority order:
//
//   1. NEVER BLANK. If the exact tile is missing, draw a lower-resolution
//      substitute from the cache and let it sharpen. A soft page for 80 ms
//      beats a white rectangle for 80 ms by a wide margin.
//
//   2. ZERO-COPY. The SkImage wraps the tile's own pixels and holds a
//      shared_ptr to keep them alive. PDFium rendered into that allocation;
//      nothing copies it on the way to the screen.
//
//   3. THEME AS A SHADER, NOT A RE-RENDER. Light/sepia/dark is a colour
//      filter on the paint. Switching themes is one uniform change and a
//      repaint, not a cache flush.

#pragma once

#include "folio/core/render/RasterCache.h"
#include "folio/core/render/TileGrid.h"
#include "folio/render/PageTheme.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkRect.h"

#include <cstdint>

namespace folio::render::skia {

namespace core = folio::core;

/// Wrap a cached tile as an SkImage without copying its pixels.
///
/// The returned image holds a strong reference to the tile, so it is safe for
/// the cache to evict the entry while a frame is still in flight - the exact
/// race the RasterCache handle design exists to make harmless.
[[nodiscard]] sk_sp<SkImage> imageForTile(const core::TileHandle& tile);

class SkiaTileCompositor {
public:
    struct Stats {
        int tilesDrawn = 0;
        int tilesExact = 0;
        int tilesFallback = 0;   // wrong zoom bucket, drawn scaled
        int tilesMissing = 0;    // nothing at all available
        int pagesDrawn = 0;
    };

    struct PageDraw {
        core::PageId    page = 0;
        SkRect          dest{};        // where the page goes, in canvas space
        float           scale = 1.f;   // device pixels per page point
        std::uint8_t    rotation = 0;
        core::Generation generation = 0;
    };

    void setTheme(const PageTheme& theme);
    [[nodiscard]] const PageTheme& theme() const noexcept { return theme_; }

    /// Paper, shadow and rounded corners, with no content. Drawn first so
    /// that a page whose tiles have not arrived still looks like a page.
    void drawPagePlaceholder(SkCanvas& canvas, const SkRect& dest, float cornerRadius = 4.f) const;

    /// Composite one page's tiles. Returns what it managed to draw, which the
    /// viewport uses to decide whether to keep the proxy visible.
    Stats drawPage(SkCanvas& canvas, const PageDraw& draw,
                   const core::TileGrid& grid, core::RasterCache& cache);

    [[nodiscard]] const Stats& lastFrame() const noexcept { return last_; }

private:
    [[nodiscard]] core::TileHandle lookupTile(core::RasterCache& cache,
                                              const core::TileKey& key,
                                              bool& exactOut) const;

    PageTheme            theme_ = PageTheme::light();
    sk_sp<SkColorFilter> themeFilter_;
    Stats                last_{};
};

}  // namespace folio::render::skia
