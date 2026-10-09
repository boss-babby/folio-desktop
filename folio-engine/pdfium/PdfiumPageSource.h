// folio-engine/pdfium/PdfiumPageSource.h
//
// An IPageSource backed by a real PDF, rendered off-thread by PdfiumWorker.
//
// WHERE THIS SITS
// ---------------
// PageStage knows only IPageSource: page count, page size, "draw page N into
// this rectangle". PlaceholderPageSource satisfies that with synthetic
// shapes. This satisfies it with PDFium. That boundary is why the UI needed
// no changes to show a real document - the stage's layout, zoom anchoring,
// raster cache and annotation overlay all work in page coordinates and do
// not care what fills them.
//
// DRAWING SOMETHING WHILE THE ENGINE WORKS
// ----------------------------------------
// Rasterisation is asynchronous, so drawPage() frequently has no bitmap at
// the size it was asked for. Rather than leave a hole it degrades in order:
//
//   1. exact-size raster               - the normal case, a plain blit
//   2. any other raster for that page  - blitted scaled, so a zoom gesture
//                                        shows the old pixels stretched for
//                                        a frame or two instead of flashing
//   3. nothing                         - paper-coloured rectangle
//
// and in cases 2 and 3 it posts a request for the right size. This is what
// makes zooming feel continuous: the gesture never waits on the engine, it
// just sharpens a moment later.
//
// CACHE KEYING
// ------------
// Keyed on page *and* pixel width, not page alone. The thumbnail rail and
// the main stage draw the same pages at wildly different sizes in the same
// frame; a page-only key would have them evict each other every frame, which
// is the most expensive possible behaviour. Eviction is LRU against a byte
// budget.

#pragma once

#include "components/PageStage.h"
#include "pdfium/PdfiumWorker.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace folio::engine {

class PdfiumPageSource final : public folio::ui::IPageSource {
public:
    PdfiumPageSource();
    ~PdfiumPageSource() override;

    /// Load a file. Blocks until the document is parsed and page geometry is
    /// known - the one operation a user expects to wait for. Returns false
    /// and leaves the source empty on failure; `error()` then says why.
    bool open(const std::string& path);

    /// One entry of the document's /Outlines tree, flattened depth-first.
    /// `depth` is 0 for a top-level heading. `page` is -1 when the
    /// destination is not a simple page (a URI action, say).
    struct OutlineEntry {
        std::string title;
        int page = -1;
        int depth = 0;
    };
    /// The real bookmark tree, read once on open. Empty for the many PDFs
    /// that ship without one - which the panel has to say out loud rather
    /// than inventing headings to fill the space.
    [[nodiscard]] const std::vector<OutlineEntry>& outline() const noexcept {
        return outline_;
    }

    [[nodiscard]] bool isOpen() const noexcept { return pageCount_ > 0; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    // --- IPageSource --------------------------------------------------------
    [[nodiscard]] int pageCount() const override { return pageCount_; }
    [[nodiscard]] folio::ui::PageSizePt pageSize(int index) const override;
    void drawPage(folio::render::IPaintTarget& t, int index,
                  const folio::render::Rect& dest, float scale,
                  folio::ui::PageTheme theme) override;
    [[nodiscard]] std::string describe() const override;
    bool tick() override;
    [[nodiscard]] bool pending() const override;
    void setFocusPage(int page) override;
    [[nodiscard]] bool cachesOwnRasters() const override { return true; }

    // --- text layer ---------------------------------------------------------
    /// The page's glyphs, or nullptr if not extracted yet. Asking for a page
    /// that has not been extracted queues the work and returns nullptr; the
    /// caller should try again on a later frame.
    [[nodiscard]] std::shared_ptr<const PageText> pageText(int index);

    /// IPageSource's glyph accessor. Converts the engine's glyph list into
    /// the UI's once per page and caches it; the two structs are kept
    /// separate so folio-ui-core never has to see an engine header.
    const std::vector<folio::ui::PageGlyph>* glyphs(int page) override;

    /// How many pages have a text layer resident. Used by search to know
    /// whether it has seen the whole document yet.
    [[nodiscard]] int textPagesResident() const noexcept {
        return static_cast<int>(text_.size());
    }

    /// Queue text extraction for every page that lacks it. Search needs the
    /// whole document, not just what has been looked at.
    void requestAllText();

    struct CacheStats {
        std::size_t pages = 0;
        std::size_t bytes = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t proxies = 0;  // frames served by a wrong-size bitmap
    };
    [[nodiscard]] CacheStats cacheStats() const noexcept { return stats_; }

    /// Drop rendered pages that are not near `page`. Called when the view
    /// moves; keeps resident memory bounded by what is actually on screen.
    void trimTo(int page, int radius);

private:
    struct Rendered {
        std::unique_ptr<folio::core::RasterTile> tile;
        int           pxW = 0, pxH = 0;
        std::uint64_t lastUsed = 0;
    };

    static std::uint64_t key(int page, int pxW) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(page)) << 32) |
               static_cast<std::uint32_t>(pxW);
    }
    /// The key handed to the paint target. Salted per opened document so a
    /// reopened file cannot collide with the previous one's cached images.
    [[nodiscard]] std::uint64_t imageKey(int page, int pxW) const {
        return key(page, pxW) ^ (salt_ * 0x9E3779B97F4A7C15ull);
    }
    /// Set when a tile leaves our cache, so the paint target can drop its
    /// matching image on the next frame. Collected rather than acted on
    /// immediately because eviction can happen without a target to hand.
    std::vector<std::uint64_t> staleKeys_;
    std::uint64_t              salt_ = 0;
    void evictTo(std::size_t budget);

    std::unique_ptr<PdfiumWorker> worker_;
    int         pageCount_ = 0;
    std::vector<OutlineEntry> outline_;
    std::string path_;
    std::string error_;

    std::map<std::uint64_t, Rendered>                   cache_;
    std::unordered_map<int, int>                        requested_;  // page -> pxW
    std::unordered_map<int, std::shared_ptr<const PageText>> text_;
    std::unordered_map<int, bool>                       textRequested_;
    std::unordered_map<int, std::vector<folio::ui::PageGlyph>> uiGlyphs_;
    std::uint64_t                                       clock_ = 0;
    std::size_t                                         bytes_ = 0;
    CacheStats                                          stats_{};
};

}  // namespace folio::engine
