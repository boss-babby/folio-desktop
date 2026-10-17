// folio-engine/pdfium/PdfiumRasterizer.cpp  --  work packages B2 / B3 / B4
//
// The only file in Folio that calls FPDF_*.

#include "PdfiumRasterizer.h"

#include "PdfiumLibrary.h"
#include "PdfiumPause.h"

#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_progressive.h>

#include <algorithm>
#include <cassert>

namespace folio::engine {

using core::Matrix;
using core::TileGrid;
using core::TileRequest;
using core::TileSpec;

namespace {

/// PDFium render flags.
///
///   FPDF_ANNOT          draw annotations already committed to the document.
///                       Folio's in-flight ink overlay is drawn by the GPU
///                       layer instead (plan §9), so this never includes the
///                       stroke currently under the user's finger.
///
///   FPDF_LCD_TEXT       deliberately ABSENT. Subpixel antialiasing depends
///                       on the physical RGB stripe order of the display. Turn
///                       it on and the same PDF produces different bytes on
///                       two machines, which destroys the cross-platform
///                       golden-image gate (plan §16.1) for a sharpness gain
///                       nobody notices on a HiDPI screen. On a 1x display it
///                       is a real improvement, so it is a per-display runtime
///                       decision - never a compile-time default.
///
///   FPDF_NO_NATIVETEXT  ON. Forces PDFium's own text rasteriser rather than
///                       CoreGraphics or GDI. This is the single flag that
///                       makes macOS and Windows produce identical pixels.
constexpr int kRenderFlags = FPDF_ANNOT | FPDF_NO_NATIVETEXT;

FS_MATRIX toFsMatrix(const Matrix& m) noexcept {
    return FS_MATRIX{m.a, m.b, m.c, m.d, m.e, m.f};
}

/// Page background. Rendering onto transparent black and compositing later
/// would be more flexible, but PDFium's antialiasing blends against whatever
/// is already in the bitmap - so text rendered on transparency and then
/// composited over white has visibly different edge pixels from text rendered
/// straight onto white. Fill here, match the goldens.
constexpr unsigned long kPaperWhite = 0xFFFFFFFFul;

}  // namespace

// ---------------------------------------------------------------------------

PdfiumRasterizer::LoadedDoc::~LoadedDoc() {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
    if (doc) {
        FPDF_CloseDocument(doc);
    }
}

PdfiumRasterizer::PdfiumRasterizer() {
    ensurePdfiumInitialised();
    threadId_ = std::this_thread::get_id();
}

PdfiumRasterizer::~PdfiumRasterizer() {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
    dropAllPages();
    current_.reset();
    retired_.clear();
}

// ---------------------------------------------------------------------------
// Document swap (B2). The old FPDF_DOCUMENT keeps serving tiles until the new
// one is open, so an edit never produces a blank frame.
// ---------------------------------------------------------------------------

bool PdfiumRasterizer::setSnapshot(core::SnapshotPtr snapshot) {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
    assertOnPdfiumThread();
    if (!snapshot) {
        return false;
    }
    if (current_ && current_->access &&
        current_->access->generation() >= snapshot->generation()) {
        return true;                      // already at or ahead of this state
    }

    auto next = std::make_unique<LoadedDoc>();
    next->access = std::make_unique<SnapshotFileAccess>(snapshot);
    next->doc = FPDF_LoadCustomDocument(next->access->handle(),
                                        password_.empty() ? nullptr : password_.c_str());

    if (!next->doc) {
        lastError_ = FPDF_GetLastError();
        ++stats_.loadFailures;
        return false;
    }
    next->pageCount = static_cast<std::size_t>(FPDF_GetPageCount(next->doc));

    // Invalidate only what actually changed. Pages untouched by the edit keep
    // every cached tile across the save - this is what makes annotating a
    // 500-page document feel instant instead of re-rendering the world.
    if (current_ && current_->access) {
        for (core::PageId p : snapshot->dirtyPagesSince(current_->access->generation())) {
            dropPage(p);
            opaqueCache_.erase(p);
        }
    } else {
        dropAllPages();
    }

    // Page handles belong to the old FPDF_DOCUMENT; they cannot outlive it.
    dropAllPages();

    if (current_) {
        retired_.push_back(std::move(current_));
    }
    current_ = std::move(next);
    ++stats_.documentsLoaded;

    // Retired documents are freed here rather than at swap time because
    // in-flight renders may still hold FPDF_PAGE handles into them. One
    // generation of lag is enough: by the time a second swap happens, the
    // first document's work has completed or been cancelled.
    if (retired_.size() > 1) {
        retired_.erase(retired_.begin(), retired_.end() - 1);
    }
    return true;
}

bool PdfiumRasterizer::openFile(const std::string& path) {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
    assertOnPdfiumThread();
    auto next = std::make_unique<LoadedDoc>();
    next->mapped = std::make_unique<FileMappedAccess>(path);
    if (!next->mapped->valid()) return false;
    next->doc = FPDF_LoadCustomDocument(next->mapped->handle(),
                                        password_.empty() ? nullptr : password_.c_str());
    if (!next->doc) {
        lastError_ = FPDF_GetLastError();
        ++stats_.loadFailures;
        return false;
    }
    next->pageCount = static_cast<std::size_t>(FPDF_GetPageCount(next->doc));
    dropAllPages();
    opaqueCache_.clear();
    retired_.clear();
    current_ = std::move(next);
    ++stats_.documentsLoaded;
    return true;
}

std::size_t PdfiumRasterizer::pageCount() const noexcept {
    return current_ ? current_->pageCount : 0;
}

core::Generation PdfiumRasterizer::generation() const noexcept {
    return current_ && current_->access ? current_->access->generation() : 0;
}

// ---------------------------------------------------------------------------
// Page handles
// ---------------------------------------------------------------------------

FPDF_PAGE PdfiumRasterizer::acquirePage(core::PageId id) {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
    assertOnPdfiumThread();
    if (!current_ || !current_->doc) {
        return nullptr;
    }
    if (id >= current_->pageCount) {
        return nullptr;
    }
    if (const auto it = pageCache_.find(id); it != pageCache_.end()) {
        return it->second;
    }
    // A prior operation never leaves another page parsed and resident.
    dropAllPages();
    FPDF_PAGE page = FPDF_LoadPage(current_->doc, static_cast<int>(id));
    if (page) {
        pageCache_.emplace(id, page);
    }
    return page;
}

void PdfiumRasterizer::dropPage(core::PageId id) {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
    if (const auto it = pageCache_.find(id); it != pageCache_.end()) {
        FPDF_ClosePage(it->second);
        pageCache_.erase(it);
    }
}

void PdfiumRasterizer::dropAllPages() {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
    for (auto& [id, page] : pageCache_) {
        FPDF_ClosePage(page);
    }
    pageCache_.clear();
}

namespace {

/// FPDFBookmark_GetTitle hands back UTF-16LE including the terminator.
std::string bookmarkTitle(FPDF_BOOKMARK bm) {
    const unsigned long bytes = FPDFBookmark_GetTitle(bm, nullptr, 0);
    if (bytes <= 2) return {};
    std::vector<unsigned short> buf(bytes / 2 + 1, 0);
    FPDFBookmark_GetTitle(bm, buf.data(), bytes);

    // Narrow to UTF-8. Surrogate pairs are folded to U+FFFD rather than
    // decoded: outline titles are headings, and a lone astral glyph in one
    // is not worth a full converter here.
    std::string out;
    for (unsigned short u : buf) {
        if (u == 0) break;
        if (u < 0x80) {
            out += static_cast<char>(u);
        } else if (u < 0x800) {
            out += static_cast<char>(0xC0 | (u >> 6));
            out += static_cast<char>(0x80 | (u & 0x3F));
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            out += "\xEF\xBF\xBD";
        } else {
            out += static_cast<char>(0xE0 | (u >> 12));
            out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (u & 0x3F));
        }
    }
    return out;
}

void walkBookmarks(FPDF_DOCUMENT doc, FPDF_BOOKMARK parent, int depth,
                   std::vector<PdfiumRasterizer::Bookmark>& out) {
    // Depth and total are both capped: /Outlines is a linked structure in
    // an untrusted file and can be made cyclic or absurdly deep.
    if (depth > 8 || out.size() > 2000) return;
    for (FPDF_BOOKMARK bm = FPDFBookmark_GetFirstChild(doc, parent); bm;
         bm = FPDFBookmark_GetNextSibling(doc, bm)) {
        PdfiumRasterizer::Bookmark e;
        e.title = bookmarkTitle(bm);
        e.depth = depth;
        if (FPDF_DEST dest = FPDFBookmark_GetDest(doc, bm))
            e.page = FPDFDest_GetDestPageIndex(doc, dest);
        if (!e.title.empty()) out.push_back(std::move(e));
        walkBookmarks(doc, bm, depth + 1, out);
        if (out.size() > 2000) return;
    }
}

}  // namespace

std::vector<PdfiumRasterizer::Bookmark> PdfiumRasterizer::outline() const {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
    std::vector<Bookmark> out;
    if (!current_ || !current_->doc) return out;
    walkBookmarks(current_->doc, nullptr, 0, out);
    return out;
}

core::PageInfo PdfiumRasterizer::pageInfo(core::PageId id) const {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
    assertOnPdfiumThread();
    core::PageInfo info{};
    if (!current_ || !current_->doc) {
        return info;
    }

    FS_SIZEF size{};
    if (FPDF_GetPageSizeByIndexF(current_->doc, static_cast<int>(id), &size)) {
        info.widthPt = size.width;
        info.heightPt = size.height;
    }
    auto* self = const_cast<PdfiumRasterizer*>(this);
    if (FPDF_PAGE p = self->acquirePage(id)) {
        info.rotation = FPDFPage_GetRotation(p) * 90;
        info.hasTransparency = FPDFPage_HasTransparency(p) != 0;
    }
    return info;
}

bool PdfiumRasterizer::pageIsOpaque(core::PageId id) {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
    if (const auto it = opaqueCache_.find(id); it != opaqueCache_.end()) {
        return it->second;
    }
    FPDF_PAGE p = acquirePage(id);
    const bool opaque = p ? (FPDFPage_HasTransparency(p) == 0) : true;
    opaqueCache_.emplace(id, opaque);
    return opaque;
}

// ---------------------------------------------------------------------------
// Rendering (B3) with progressive slicing (B4)
// ---------------------------------------------------------------------------

std::unique_ptr<core::RasterTile> PdfiumRasterizer::renderInto(
    core::PageId pageId, const core::TileKey& key,
    std::int32_t w, std::int32_t h, const Matrix& matrix,
    bool slice, core::Priority priority, const TileRequest* cancelCheck) {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());

    assertOnPdfiumThread();
    if (w <= 0 || h <= 0) {
        return nullptr;
    }

    FPDF_PAGE page = acquirePage(pageId);
    if (!page) {
        return nullptr;
    }

    auto tile = core::RasterTile::make(key, w, h, generation());
    tile->opaque = pageIsOpaque(pageId);

    // FPDFBitmap_CreateEx renders DIRECTLY into our buffer - PDFium allocates
    // nothing and there is no bitmap to copy out of afterwards. The tile that
    // comes back from this function is the exact allocation PDFium wrote into.
    const int stride = static_cast<int>(tile->stride);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        w, h, FPDFBitmap_BGRA, tile->pixels.data(), stride);
    if (!bitmap) {
        return nullptr;
    }

    FPDFBitmap_FillRect(bitmap, 0, 0, w, h, kPaperWhite);

    const FS_MATRIX fs = toFsMatrix(matrix);
    const FS_RECTF clip{0.f, 0.f, static_cast<float>(w), static_cast<float>(h)};

    if (!slice) {
        // Visible work: render in one shot. Slicing a small visible tile adds
        // latency and buys nothing, because nothing is more urgent than it.
        FPDF_RenderPageBitmapWithMatrix(bitmap, page, &fs, &clip, kRenderFlags);
        FPDFBitmap_Destroy(bitmap);
        ++stats_.tilesRendered;
        return tile;
    }

    // Background work: slice it so a visible tile can preempt.
    //
    // NOTE, AND THIS IS A REAL LIMITATION: the progressive API
    // (FPDF_RenderPageBitmap_Start) takes an integer start_x/y/size_x/size_y
    // and a rotate enum - it has NO matrix overload. Fractional zoom and
    // arbitrary tile origins therefore cannot be expressed through it. So
    // sliced rendering is used only for whole-page work at integer scale
    // (thumbnails, proxies, prefetch), which is exactly the work that is slow
    // enough to need slicing. Visible tiles go through the matrix path above.
    PdfiumPause pause([this] { return hooks_.hasUrgentWork && hooks_.hasUrgentWork(); });
    pause.beginSlice();

    int status = FPDF_RenderPageBitmap_Start(bitmap, page, 0, 0, w, h,
                                             /*rotate=*/0, kRenderFlags, &pause);
    while (status == FPDF_RENDER_TOBECONTINUED) {
        if (cancelCheck && hooks_.isCancelled && hooks_.isCancelled(*cancelCheck)) {
            FPDF_RenderPage_Close(page);
            FPDFBitmap_Destroy(bitmap);
            ++stats_.tilesCancelled;
            return nullptr;
        }
        if (hooks_.pumpHigherPriority) {
            hooks_.pumpHigherPriority(priority);
        }
        pause.beginSlice();
        status = FPDF_RenderPage_Continue(page, &pause);
    }
    FPDF_RenderPage_Close(page);
    FPDFBitmap_Destroy(bitmap);

    stats_.slices += pause.sliceCount();
    stats_.worstPauseLatencyUs = std::max(
        stats_.worstPauseLatencyUs,
        static_cast<std::uint64_t>(pause.worstLatency().count()));

    if (status != FPDF_RENDER_DONE) {
        ++stats_.tilesCancelled;
        return nullptr;
    }
    ++stats_.tilesRendered;
    return tile;
}

std::unique_ptr<core::RasterTile> PdfiumRasterizer::renderTile(
    const TileRequest& req, const TileSpec& spec) {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
    assertOnPdfiumThread();

    // By the time this dequeues, the document may have moved on. Rendering
    // stale content would put the wrong pixels on screen, so drop it - the
    // scheduler has already queued a replacement against the current state.
    if (!current_ || req.snapshotGen != generation()) {
        ++stats_.tilesStale;
        return nullptr;
    }
    if (hooks_.isCancelled && hooks_.isCancelled(req)) {
        ++stats_.tilesCancelled;
        return nullptr;
    }

    core::TileKey key{};
    key.pageId   = req.page;
    key.tileX    = spec.tx;
    key.tileY    = spec.ty;
    key.scaleQ   = core::quantiseScale(req.scale);
    key.rotation = static_cast<std::uint8_t>(((req.rotation / 90) % 4 + 4) % 4);
    key.scheme   = req.scheme;

    return renderInto(req.page, key, spec.rect.w, spec.rect.h, spec.matrix,
                      /*slice=*/false, req.priority, &req);
}

std::unique_ptr<core::RasterTile> PdfiumRasterizer::renderFullPage(
    core::PageId pageId, float scale, int rotationDegrees, core::ColorScheme scheme) {
    std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
    assertOnPdfiumThread();

    const core::PageInfo info = pageInfo(pageId);
    if (info.widthPt <= 0.f || info.heightPt <= 0.f) {
        return nullptr;
    }

    const auto quarter = static_cast<std::uint8_t>(((rotationDegrees / 90) % 4 + 4) % 4);
    const TileGrid grid(info.widthPt, info.heightPt, scale, quarter,
                        /*tileSize=*/1 << 20);   // one tile covers the page

    core::TileKey key{};
    key.pageId   = pageId;
    key.scaleQ   = core::quantiseScale(scale);
    key.rotation = quarter;
    key.scheme   = scheme;

    return renderInto(pageId, key, grid.deviceWidth(), grid.deviceHeight(),
                      grid.fullPageMatrix(), /*slice=*/false,
                      core::Priority::VisibleTile, nullptr);
}

void PdfiumRasterizer::assertOnPdfiumThread() const {
    assert(std::this_thread::get_id() == threadId_ &&
           "PDFium APIs are not thread-safe - every call must come from the "
           "document's bound PDFium thread");
}

}  // namespace folio::engine
