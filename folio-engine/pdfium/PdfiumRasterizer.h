// folio-engine/pdfium/PdfiumRasterizer.h  --  work packages B2 / B3 / B4
//
// The only class in Folio that owns an FPDF_DOCUMENT.
//
// THREADING CONTRACT
// ------------------
// PDFium's own header says it plainly: "None of the PDFium APIs are
// thread-safe. They expect to be called from a single thread." Violating it
// does not throw and does not assert inside PDFium - it corrupts the heap and
// crashes somewhere unrelated, minutes later. So:
//
//   * One PdfiumRasterizer per open document.
//   * Every method must be called from that rasterizer's bound thread.
//   * bindToCurrentThread() records which thread that is; every entry point
//     asserts on it in debug builds.
//
// Concurrency across documents is real (two documents render in parallel);
// concurrency within one is not, and the design does not pretend otherwise.
// The progressive slicing in PdfiumPause.h is what makes one thread per
// document sufficient: a two-second page render becomes ~500 resumable
// fragments, so a newly visible tile preempts within one 4 ms slice instead
// of waiting for the page.

#pragma once

#include <utility>
#include <fpdfview.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "folio/core/engine/DocumentSnapshot.h"
#include "folio/core/engine/IPdfRasterizer.h"
#include "folio/core/render/RasterTile.h"
#include "folio/core/render/TileGrid.h"

#include "SnapshotFileAccess.h"

namespace folio::engine {

namespace core = folio::core;

/// Everything the rasterizer needs from the scheduler, as three callables.
///
/// Deliberately not a pointer to TileScheduler. The engine does not depend on
/// the scheduler, which means the whole rasterizer can be driven directly by
/// a test with no queue, no threads and no timing - which is exactly how the
/// pixel-exactness tests in tests/engine are written.
struct RenderHooks {
    /// Polled from inside PDFium between render steps. Must be cheap and must
    /// not allocate.
    std::function<bool()> hasUrgentWork;

    /// Called between slices of a background render, so higher-priority work
    /// can run on this same thread before we resume.
    std::function<void(core::Priority)> pumpHigherPriority;

    /// True if this specific request has been cancelled since it was queued.
    std::function<bool(const core::TileRequest&)> isCancelled;
};

class PdfiumRasterizer {
public:
    struct Stats {
        std::uint64_t tilesRendered = 0;
        std::uint64_t tilesCancelled = 0;
        std::uint64_t tilesStale = 0;
        std::uint64_t slices = 0;
        std::uint64_t worstPauseLatencyUs = 0;
        std::uint64_t documentsLoaded = 0;
        std::uint64_t loadFailures = 0;
    };

    PdfiumRasterizer();
    ~PdfiumRasterizer();

    PdfiumRasterizer(const PdfiumRasterizer&) = delete;
    PdfiumRasterizer& operator=(const PdfiumRasterizer&) = delete;

    /// Declare the calling thread to be this document's PDFium thread.
    void bindToCurrentThread() noexcept { threadId_ = std::this_thread::get_id(); }

    void setHooks(RenderHooks hooks) { hooks_ = std::move(hooks); }
    void setPassword(std::string password) { password_ = std::move(password); }

    // --- document ----------------------------------------------------------

    /// Point at a new snapshot. Returns false and keeps serving the previous
    /// generation if the new one fails to parse - a corrupt incremental
    /// update must never blank the user's document.
    bool setSnapshot(core::SnapshotPtr snapshot);

    [[nodiscard]] std::size_t pageCount() const noexcept;
    [[nodiscard]] core::Generation generation() const noexcept;
    [[nodiscard]] core::PageInfo pageInfo(core::PageId page) const;
    [[nodiscard]] unsigned long lastError() const noexcept { return lastError_; }

    /// One flattened entry of the document's /Outlines tree.
    struct Bookmark {
        std::string title;
        int page = -1;
        int depth = 0;
    };
    /// Walk /Outlines depth-first. Must be called on this document's PDFium
    /// thread, like everything else here.
    [[nodiscard]] std::vector<Bookmark> outline() const;

    // --- rendering ---------------------------------------------------------

    /// Render one tile. Returns the tile, or nullptr if the request was stale,
    /// cancelled, or the page could not be loaded.
    ///
    /// Synchronous by design: the caller is already on the PDFium thread and
    /// has already decided this is the most important work available. Async
    /// is the scheduler's job, one layer up.
    [[nodiscard]] std::unique_ptr<core::RasterTile> renderTile(
        const core::TileRequest& request, const core::TileSpec& spec);

    /// Whole page into one bitmap. Used for thumbnails, proxies, and the B3
    /// test that asserts a tiled render reassembles to exactly this.
    [[nodiscard]] std::unique_ptr<core::RasterTile> renderFullPage(
        core::PageId page, float scale, int rotationDegrees,
        core::ColorScheme scheme = core::ColorScheme::Light);

    /// Borrow the loaded FPDF_PAGE so a caller can run a non-rendering
    /// PDFium API against it - text extraction, link lists, form fields.
    /// Must be called on the bound thread; the handle stays owned by the
    /// page cache here and must not be closed by the caller.
    [[nodiscard]] FPDF_PAGE borrowPage(core::PageId page) {
        assertOnPdfiumThread();
        return acquirePage(page);
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = Stats{}; }

private:
    struct LoadedDoc {
        std::unique_ptr<SnapshotFileAccess> access;
        FPDF_DOCUMENT doc = nullptr;
        std::size_t   pageCount = 0;
        ~LoadedDoc();
    };

    /// LRU of open FPDF_PAGE handles. Opening a page parses its content
    /// stream; at 6 tiles per page that is 6x the work if not cached. 16 is
    /// three screens' worth either side of the viewport.
    static constexpr std::size_t kMaxOpenPages = 16;

    FPDF_PAGE acquirePage(core::PageId page);
    void      dropPage(core::PageId page);
    void      dropAllPages();
    bool      pageIsOpaque(core::PageId page);

    /// Shared body of renderTile and renderFullPage.
    std::unique_ptr<core::RasterTile> renderInto(
        core::PageId page, const core::TileKey& key, std::int32_t w, std::int32_t h,
        const core::Matrix& matrix, bool slice, core::Priority priority,
        const core::TileRequest* cancelCheck);

    void assertOnPdfiumThread() const;

    std::unique_ptr<LoadedDoc> current_;
    std::vector<std::unique_ptr<LoadedDoc>> retired_;

    std::map<core::PageId, FPDF_PAGE>   pageCache_;
    std::unordered_map<core::PageId, bool> opaqueCache_;

    RenderHooks     hooks_{};
    std::string     password_;
    unsigned long   lastError_ = 0;
    std::thread::id threadId_{};
    Stats           stats_{};
};

}  // namespace folio::engine
