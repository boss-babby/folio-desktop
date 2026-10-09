// folio-engine/pdfium/PdfiumWorker.h
//
// One thread, one document, two kinds of work.
//
// WHY A THREAD AT ALL
// -------------------
// Rasterising a page is real work - tens of milliseconds for a dense page at
// high zoom, and unbounded for a scanned one. Doing it inside drawPage() ties
// frame time to document complexity: the UI stalls for exactly as long as
// PDFium takes, and there is nothing the compositor can do about it. Moving
// it off the UI thread turns a stall into a latency: the frame still ships on
// time, just showing a slightly stale raster until the fresh one lands.
//
// WHY ONE THREAD AND NOT A POOL
// -----------------------------
// PDFium's contract is single-threaded per document. FPDF_DOCUMENT and every
// FPDF_PAGE derived from it must be touched from one thread; the library will
// corrupt its own page cache otherwise, usually not immediately. A pool would
// need one document instance per thread - a real option for huge documents,
// but it multiplies parse cost and resident memory, and the win only shows up
// when several pages are wanted at once. One thread, kept busy, is the right
// starting point.
//
// So: this class owns the thread, and the thread owns the rasteriser. No
// other thread touches PDFium. Page geometry is pulled across once at open
// time and cached as plain data, so the UI can lay out pages without ever
// calling into the engine.
//
// COALESCING
// ----------
// During a zoom gesture the stage asks for the same page at a dozen different
// scales in a few hundred milliseconds. Only the last one matters. Pending
// work is therefore keyed by page, and a newer request for a page replaces
// the older one rather than queueing behind it. Work already in flight is not
// interrupted - PDFium's pause callback could do that, and does for tiles,
// but for whole pages letting it finish is usually cheaper than restarting.

#pragma once

#include "pdfium/PdfiumRasterizer.h"

#include "folio/core/engine/DocumentSnapshot.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace folio::engine {

/// One laid-out glyph. Boxes are 0..1 page-normalised with a top-left origin,
/// which is the coordinate space the UI already uses for annotations, so a
/// selection rectangle and a highlight are directly comparable.
struct TextGlyph {
    char32_t code = 0;
    float    l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;
};

/// A page's text layer: the glyphs in reading order as PDFium reports them,
/// plus a parallel case-folded string so search is a plain substring scan.
struct PageText {
    std::vector<TextGlyph> glyphs;
    std::u32string         folded;  // same length as glyphs, lowercased
    [[nodiscard]] bool empty() const noexcept { return glyphs.empty(); }
};

class PdfiumWorker {
public:
    struct PageMeta {
        double widthPt = 612.0;
        double heightPt = 792.0;
        int    rotation = 0;
    };

    enum class Kind { Render, Text };

    struct Result {
        Kind kind = Kind::Render;
        int  page = 0;
        // Render
        std::unique_ptr<folio::core::RasterTile> tile;
        int   pxW = 0, pxH = 0;
        float scale = 1.0f;
        // Text
        std::shared_ptr<const PageText> text;
    };

    PdfiumWorker();
    ~PdfiumWorker();

    PdfiumWorker(const PdfiumWorker&) = delete;
    PdfiumWorker& operator=(const PdfiumWorker&) = delete;

    /// Load a document. Blocks until the worker has parsed it and reported
    /// page geometry back - opening is the one operation a user expects to
    /// wait for, and laying out the page rail needs the sizes up front.
    bool open(std::vector<std::byte> bytes, std::string* error);

    [[nodiscard]] int pageCount() const noexcept { return static_cast<int>(meta_.size()); }
    [[nodiscard]] PageMeta pageMeta(int index) const;
    /// The /Outlines tree, read on the worker thread at load time and
    /// cached. Empty when the document has no bookmarks.
    [[nodiscard]] std::vector<PdfiumRasterizer::Bookmark> outline() const;

    /// Ask for a page raster. Supersedes any queued request for the same page.
    void requestRender(int page, float scale, int rotationDeg);

    /// Ask for a page's text layer. Ignored if already requested.
    void requestText(int page);

    /// Pages near `page` are rendered first when several are outstanding.
    void setFocusPage(int page) noexcept { focus_.store(page, std::memory_order_relaxed); }

    /// Hand over everything finished since the last call. UI thread only.
    [[nodiscard]] std::vector<Result> drain();

    /// True while anything is queued or in flight - the UI keeps asking for
    /// frames while this is true so a landing raster is actually shown.
    [[nodiscard]] bool busy() const;

    /// Forget queued work for pages outside [page-radius, page+radius].
    void dropQueuedOutside(int page, int radius);

private:
    struct Job {
        Kind  kind = Kind::Render;
        int   page = 0;
        float scale = 1.0f;
        int   rotation = 0;
    };

    void threadMain();
    bool takeNextJob(Job& out);

    std::thread             thread_;
    mutable std::mutex      m_;
    std::condition_variable cv_;
    bool                    quit_ = false;

    // --- protected by m_ ---
    std::vector<std::byte>      pendingLoad_;
    bool                        loadRequested_ = false;
    bool                        loadDone_ = false;
    bool                        loadOk_ = false;
    std::string                 loadError_;
    std::map<int, Job>          renderQueue_;   // one per page, newest wins
    std::map<int, Job>          textQueue_;
    std::vector<Result>         done_;
    int                         inFlight_ = 0;

    std::vector<PageMeta>       meta_;          // written once at open
    std::vector<PdfiumRasterizer::Bookmark> outline_;   // ditto
    std::atomic<int>            focus_{0};
    folio::core::SnapshotPtr    snapshot_;
};

}  // namespace folio::engine
