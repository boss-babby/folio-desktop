// folio-engine/pdfium/PdfiumWorker.h
//
// One thread per document: first-page open, on-demand geometry, raster and text.
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
// other thread touches PDFium. A path open publishes the first page's
// geometry and page count immediately; geometry for nearby pages is fetched
// lazily on that same worker. The UI only sees plain cached data.
//
// COALESCING
// ----------
// Canvas tile jobs are keyed by page, device width and grid coordinates;
// each visible frame discards queued work from the old viewport. Whole-page
// thumbnail jobs are independently keyed by page and width. Visible tiles
// outrank thumbnails, text and background metadata. An in-flight synchronous
// PDFium render cannot be interrupted; cancelled tab results are discarded.

#pragma once

#include "pdfium/PdfiumRasterizer.h"
#include "folio/core/render/TileGrid.h"
#include "AnnotationSpec.h"

#include "folio/core/engine/DocumentSnapshot.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <functional>
#include <unordered_set>
#include <optional>
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

/// Metadata for a source-PDF annotation. It is NOT a Folio overlay: PDFium
/// renders the original once, and export leaves its object untouched.
struct PdfAnnotationInfo {
    int index = -1;
    int subtype = 0;
    std::string contents;
    float l = 0.f, t = 0.f, r = 0.f, b = 0.f; // display-normalised rect
    std::uint8_t red = 255, green = 214, blue = 10, alpha = 255;
    bool editable = false;
    std::uint64_t fingerprint = 0;
};

class PdfiumWorker {
public:
    struct PageMeta {
        double widthPt = 612.0;
        double heightPt = 792.0;
        int    rotation = 0;
    };

    enum class Kind { Render, Tile, Text, Annotations, Export, Meta, Outline };

    struct Result {
        Kind kind = Kind::Render;
        int  page = 0;
        // Render
        std::unique_ptr<folio::core::RasterTile> tile;
        int   pxW = 0, pxH = 0;
        int   requestedWidth = 0; // stable source key, even for failed renders
        float scale = 1.0f;
        folio::core::TileSpec spec{};
        // Text
        std::shared_ptr<const PageText> text;
        std::vector<PdfAnnotationInfo> annotations;
        std::string exportPath;
        std::string error;
    };

    PdfiumWorker();
    ~PdfiumWorker();

    PdfiumWorker(const PdfiumWorker&) = delete;
    PdfiumWorker& operator=(const PdfiumWorker&) = delete;

    /// Load a document. Blocks until the worker has parsed it and reported
    /// page geometry back - opening is the one operation a user expects to
    /// wait for, and laying out the page rail needs the sizes up front.
    bool open(std::vector<std::byte> bytes, std::string* error);
    bool openFile(std::string path, std::string* error);

    [[nodiscard]] int pageCount() const noexcept { return static_cast<int>(meta_.size()); }
    [[nodiscard]] PageMeta pageMeta(int index) const;
    [[nodiscard]] bool pageMetaLoaded(int index) const;
    /// The /Outlines tree, read on the worker thread at load time and
    /// cached. Empty when the document has no bookmarks.
    [[nodiscard]] std::vector<PdfiumRasterizer::Bookmark> outline() const;
    [[nodiscard]] unsigned geometryRevision() const noexcept {
        return geometryRevision_.load(std::memory_order_acquire);
    }
    [[nodiscard]] unsigned outlineRevision() const noexcept {
        return outlineRevision_.load(std::memory_order_acquire);
    }
    /// Fetch the geometry of a visible page, not the whole document tree.
    void requestMeta(int page);

    /// Ask for a page raster. Supersedes any queued request for the same page.
    void requestRender(int page, float scale, int rotationDeg, int requestedWidth,
                       bool visiblePreview = false);

    /// Queue one visible 512-device-pixel tile; queued tiles outrank whole
    /// thumbnails. `width` identifies the precise raster scale.
    void requestTile(int page, float scale, int width,
                     const folio::core::TileSpec& spec, int rank);
    void dropQueuedTiles();
    /// Keep only the current frame's tile jobs. Repeated frames must not tear
    /// down and reinsert the same queue while a PDF is still rendering.
    void retainQueuedTiles(const std::unordered_set<std::uint64_t>& wanted);
    /// Cancel queued rendering and text requests when a tab goes inactive.
    void suspendVisualWork();

    /// Ask for a page's text layer. Ignored if already requested.
    void requestText(int page);
    void requestAnnotations(int page);
    // In-memory view suppression. The source PDF remains untouched; Save As
    // removes only matching originals and writes the edited Folio overlays.
    using Suppressed = std::map<int, std::vector<std::pair<int, int>>>;
    /// Returns true only when the suppressed imported marks actually change.
    bool setSuppressed(Suppressed indices);

    /// Queue a private copy on the sole PDFium thread. One export at a time.
    bool requestExport(std::string source, std::string target,
                       std::vector<AnnotSpec> marks);

    /// Pages near `page` are rendered first when several are outstanding.
    void setFocusPage(int page) noexcept { focus_.store(page, std::memory_order_relaxed); }
    /// Metadata and bounded previews for every on-screen reading page must
    /// outrank sharp tiles of the focused page. Grid has its own range.
    void setVisibleCanvasRange(int first, int last);
    /// The Pages panel can show pages far from the canvas focus. Only its
    /// visible thumbnails are eligible to remain queued/rendered.
    void setThumbnailRange(int first, int last);
    void setGridRange(int first, int last, int bufferFirst, int bufferLast);

    /// Hand over everything finished since the last call. UI thread only.
    [[nodiscard]] std::vector<Result> drain();

    /// True while anything is queued or in flight - the UI keeps asking for
    /// frames while this is true so a landing raster is actually shown.
    [[nodiscard]] bool busy() const;
    /// Completed results require a repaint; in-flight jobs do not. The
    /// shell polls completion without spinning its GPU while PDFium works.
    [[nodiscard]] bool hasResults() const;
    void setWakeCallback(std::function<void()> wake);

    /// Forget queued work for pages outside [page-radius, page+radius].
    void dropQueuedOutside(int page, int radius);

private:
    struct Job {
        Kind  kind = Kind::Render;
        int   page = 0;
        float scale = 1.0f;
        int   rotation = 0;
        int   requestedWidth = 0;
        folio::core::TileSpec spec{};
        int rank = 0;
        std::uint64_t visualEpoch = 0;
        std::string source, target;
        std::vector<AnnotSpec> marks;
        bool visiblePreview = false;
    };

    void threadMain();
    bool takeNextJob(Job& out);

    std::thread             thread_;
    mutable std::mutex      m_;
    std::condition_variable cv_;
    bool                    quit_ = false;
    std::function<void()>   wake_;
    std::uint64_t           visualEpoch_ = 0;
    Suppressed              suppressed_;
    std::uint64_t           suppressionRevision_ = 0; // guarded by m_

    // --- protected by m_ ---
    std::vector<std::byte>      pendingLoad_;
    std::string                 pendingPath_;
    bool                        loadRequested_ = false;
    bool                        loadDone_ = false;
    bool                        loadOk_ = false;
    std::string                 loadError_;
    // Main canvas and thumbnail need different widths for the same page.
    // Keying the queue by page alone made them continually cancel each other.
    std::map<std::uint64_t, Job> renderQueue_; // (page, requested pixel width)
    std::map<std::tuple<int,int,int,int>, Job> tileQueue_; // page,width,tx,ty
    int                         tilesSinceThumbnail_ = 0; // worker thread only
    int                         tilesSinceNeighbour_ = 0; // worker thread only
    std::optional<std::tuple<int,int,int,int>> activeTile_; // guarded by m_
    std::uint64_t               activeTileEpoch_ = 0;
    int                         thumbnailFirst_ = -1; // protected by m_
    int                         thumbnailLast_ = -1;
    int                         canvasFirst_ = -1, canvasLast_ = -1; // visible reading pages
    int                         gridFirst_ = -1, gridLast_ = -1;
    int                         gridBufferFirst_ = -1, gridBufferLast_ = -1;
    std::map<int, Job>          textQueue_;
    std::map<int, Job>          annotationsQueue_;
    std::map<int, Job>          metaQueue_;
    bool                        outlinePending_ = false;
    bool                        firstRenderDone_ = false;
    std::vector<std::uint8_t>   metaLoaded_;
    std::set<int>               resolvedMeta_; // nearest-geometry lookup, O(log pages)
    std::unique_ptr<Job>        exportJob_;
    std::vector<Result>         done_;
    bool                        exporting_ = false;
    int                         inFlight_ = 0;
    bool                        foregroundInFlight_ = false;
    std::atomic<unsigned>       geometryRevision_{0};
    std::atomic<unsigned>       outlineRevision_{0};

    std::vector<PageMeta>       meta_;     // provisional on path open; updated on demand
    std::vector<PdfiumRasterizer::Bookmark> outline_; // deferred until after first raster
    std::atomic<int>            focus_{0};
    folio::core::SnapshotPtr    snapshot_;
};

}  // namespace folio::engine
