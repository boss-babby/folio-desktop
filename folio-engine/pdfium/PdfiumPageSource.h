// folio-engine/pdfium/PdfiumPageSource.h
//
// An IPageSource backed by a real PDF, rendered off-thread by PdfiumWorker.
//
// WHERE THIS SITS
// ---------------
// PageStage knows only IPageSource: thumbnails use drawPage, while the
// main canvas supplies its actual visible rectangle to drawVisiblePage.
// Placeholder sources delegate to drawPage; this source renders only the
// intersecting 512-device-pixel PDFium tiles on its document worker.
//
// DRAWING SOMETHING WHILE THE ENGINE WORKS
// ----------------------------------------
// Rasterisation is asynchronous. Thumbnails served by drawPage() degrade as:
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
// Thumbnails are keyed by (page, pixel width). Canvas tiles add tile X/Y to
// the key; old-scale on-screen tiles bridge continuous zoom while fresh ones
// arrive. An inactive tab keeps only its open document, never its pixels.

#pragma once

#include "components/PageStage.h"
#include "pdfium/PdfiumWorker.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <tuple>
#include <functional>

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
    /// The real bookmark tree, loaded in the background after first paint.
    /// Empty for the many PDFs without one; the panel must not invent headings.
    [[nodiscard]] const std::vector<OutlineEntry>& outline() const noexcept {
        return outline_;
    }
    [[nodiscard]] unsigned outlineRevision() const noexcept { return lastOutlineRevision_; }

    [[nodiscard]] bool isOpen() const noexcept { return pageCount_ > 0; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    // --- IPageSource --------------------------------------------------------
    [[nodiscard]] int pageCount() const override { return pageCount_; }
    [[nodiscard]] folio::ui::PageSizePt pageSize(int index) const override;
    void drawPage(folio::render::IPaintTarget& t, int index,
                  const folio::render::Rect& dest, float scale,
                  folio::ui::PageTheme theme) override;
    void drawVisiblePage(folio::render::IPaintTarget& t, int index,
                         const folio::render::Rect& dest,
                         const folio::render::Rect& visible,
                         float scale, folio::ui::PageTheme theme) override;
    bool drawCachedRegion(folio::render::IPaintTarget& t, int index,
                          const folio::render::Rect& dest,
                          const folio::render::Rect& visible, float scale,
                          folio::ui::PageTheme theme) override;
    [[nodiscard]] std::string describe() const override;
    bool tick() override;
    [[nodiscard]] bool pending() const override;
    [[nodiscard]] unsigned geometryRevision() const override {
        return worker_->geometryRevision();
    }
    void setFocusPage(int page) override;
    void setVisibleReadingPages(int first, int last) override;
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
    /// Release search-only text once its hits have been indexed by the UI.
    void releaseText(int page);

    /// Null until the document worker indexes that page on demand.
    const std::vector<PdfAnnotationInfo>* annotationsForPage(int page);
    void releaseAnnotations(int page) { annotations_.erase(page); requestedAnnotations_.erase(page); }
    void suppressSourceAnnotations(PdfiumWorker::Suppressed indices);

    struct CacheStats {
        std::size_t pages = 0;
        std::size_t bytes = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t proxies = 0;  // frames served by a wrong-size bitmap
    };
    bool requestExport(std::vector<AnnotSpec> marks, std::string output);
    /// Returned once after the worker has saved (or failed).
    [[nodiscard]] std::optional<PdfiumWorker::Result> takeExportResult();

    [[nodiscard]] CacheStats cacheStats() const noexcept { return stats_; }
    [[nodiscard]] bool hasCachedPageProxy(int page) const noexcept {
        auto it = cache_.lower_bound(key(page, 0));
        return it != cache_.end() && static_cast<int>(it->first >> 32) == page;
    }

    /// Drop rendered pages that are not near `page`. Called when the view
    /// moves; keeps resident memory bounded by what is actually on screen.
    void trimTo(int page, int radius);
    /// One paint frame's on-screen tile set. Unused tile jobs are cancelled;
    /// no timer is left running to do speculative background rendering.
    void beginVisibleFrame();
    void endVisibleFrame() override;
    void beginCanvasFrame() override { visibleFrameComplete_ = true; }
    [[nodiscard]] bool visibleFrameComplete() const noexcept override {
        return visibleFrameComplete_;
    }
    void setVisibleThumbnailRange(int first, int last) override;
    void setVisibleGridRange(int first, int last, int bufferFirst,
                             int bufferLast, int pixelW, int pixelH) override;
    /// One-shot idle pruning: retain only images touched by the last frame.
    void trimIdle();
    /// Inactive tab: keep at most one bounded screenshot-sized preview.
    void parkVisualCaches(int page, bool retainNearby = false);
    /// Minimise/close: drop *all* pixels/text, keep the document.
    void suspendVisualCaches();
    void setWakeCallback(std::function<void()> wake) { worker_->setWakeCallback(std::move(wake)); }
    [[nodiscard]] bool busy() const { return worker_->busy(); }

private:
    std::unordered_map<int, std::vector<PdfAnnotationInfo>> annotations_;
    std::unordered_set<int> requestedAnnotations_;
    std::optional<PdfiumWorker::Result> exportResult_;
    struct Rendered {
        std::unique_ptr<folio::core::RasterTile> tile;
        int           pxW = 0, pxH = 0;
        std::uint64_t lastUsed = 0;
        std::uint64_t lastFrame = 0;
    };
    using TileId = std::tuple<int,int,int,int>; // page, device width, tx, ty
    struct VisibleTile {
        std::unique_ptr<folio::core::RasterTile> pixels;
        folio::core::DeviceRect rect{};
        std::uint64_t lastUsed = 0;
        std::uint64_t lastFrame = 0;
    };
    [[nodiscard]] std::uint64_t tileImageKey(const TileId& k) const {
        auto [page, width, x, y] = k;
        std::uint64_t h = imageKey(page, width) ^ 0xD2B74407B1CE6E93ull;
        h ^= static_cast<std::uint64_t>(x + 1) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::uint64_t>(y + 1) * 0xC2B2AE3D27D4EB4Full;
        return h;
    }
    void evictTilesTo(std::size_t budget);

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
    int         gridFirst_ = -1, gridLast_ = -1;
    int         gridBufferFirst_ = -1, gridBufferLast_ = -1;
    int         gridPixelW_ = 0, gridPixelH_ = 0;
    [[nodiscard]] bool gridBuffered(int page) const noexcept {
        return gridBufferFirst_ >= 0 && page >= gridBufferFirst_ &&
               page <= gridBufferLast_;
    }
    void queueGridBuffer();
    std::vector<OutlineEntry> outline_;
    unsigned lastOutlineRevision_ = 0;
    unsigned lastGeometryRevision_ = 0;
    std::string path_;
    std::string error_;

    std::map<std::uint64_t, Rendered>                   cache_;
    std::map<TileId, VisibleTile>                       tileCache_;
    std::unordered_set<std::uint64_t>                    requestedTiles_;
    std::unordered_set<std::uint64_t>                    nextRequestedTiles_;
    std::uint64_t                                       frame_ = 0;
    bool                                                visibleFrameComplete_ = true;
    std::size_t                                         tileBytes_ = 0;
    static std::uint64_t tileRequestKey(const TileId& k) {
        auto [page, width, x, y] = k;
        return (static_cast<std::uint64_t>(static_cast<std::uint16_t>(page)) << 48) |
               (static_cast<std::uint64_t>(static_cast<std::uint16_t>(width)) << 32) |
               (static_cast<std::uint64_t>(static_cast<std::uint16_t>(y)) << 16) |
                static_cast<std::uint16_t>(x);
    }
    std::unordered_set<std::uint64_t>                    requested_;  // page + width
    std::unordered_map<int, int>                        failedWidth_; // retry lower res
    std::unordered_map<int, int>                        previewWidth_; // low-res visible proxy
    std::unordered_set<int>                             failedPreview_;
    // One page either side, plus one ahead in the current scroll direction:
    // <=240x320 each, after the foreground has produced its first pixels.
    std::unordered_set<int>                             adjacentPreviewQueued_;
    int                                                 scrollDirection_ = 1;
    int                                                 focusPage_ = -1;
    int                                                 readingFirst_ = -1, readingLast_ = -1;
    [[nodiscard]] bool visibleReadingPage(int p) const noexcept {
        return readingFirst_ >= 0 && p >= readingFirst_ && p <= readingLast_;
    }
    int                                                 thumbnailFirst_ = -1;
    int                                                 thumbnailLast_ = -1;
    [[nodiscard]] bool visibleThumbnail(int page, int width) const noexcept {
        return width <= 256 && page >= thumbnailFirst_ && page <= thumbnailLast_;
    }
    std::unordered_map<int, std::shared_ptr<const PageText>> text_;
    std::unordered_map<int, bool>                       textRequested_;
    std::unordered_map<int, std::vector<folio::ui::PageGlyph>> uiGlyphs_;
    std::uint64_t                                       clock_ = 0;
    std::size_t                                         bytes_ = 0;
    CacheStats                                          stats_{};
};

}  // namespace folio::engine
