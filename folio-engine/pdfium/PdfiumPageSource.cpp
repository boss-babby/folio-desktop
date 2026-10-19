// folio-engine/pdfium/PdfiumPageSource.cpp

#include "pdfium/PdfiumPageSource.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace folio::engine {

namespace core = folio::core;
using folio::render::IPaintTarget;
using folio::render::Rect;

namespace {
// PDFium pixels, SkImage copies and GPU textures are SEPARATE allocations.
// The previous 192 MiB source cache + 256 MiB image cache could each hold
// the same pages independently, well before PDFium's open-page cache and the
// display surface. Keep the phone's entire page pipeline bounded instead of
// budgeting a single layer in isolation.
#if defined(__ANDROID__)
constexpr int kMaxLongEdge = 2048; // at most ~16 MiB for one square BGRA page
constexpr std::size_t kByteBudget = 24u * 1024u * 1024u;
#else
constexpr int kMaxLongEdge = 4096;
constexpr std::size_t kByteBudget = 48u * 1024u * 1024u;
#endif
}  // namespace

PdfiumPageSource::PdfiumPageSource() : worker_(std::make_unique<PdfiumWorker>()) {}
PdfiumPageSource::~PdfiumPageSource() = default;

bool PdfiumPageSource::open(const std::string& path) {
    cache_.clear();
    tileCache_.clear();
    tileBytes_ = 0;
    requestedTiles_.clear();
    nextRequestedTiles_.clear();
    frame_ = 0;
    exportResult_.reset();
    requested_.clear();
    failedWidth_.clear();
    previewWidth_.clear();
    failedPreview_.clear();
    adjacentPreviewQueued_.clear();
    focusPage_ = -1;
    readingFirst_ = readingLast_ = -1;
    worker_->setVisibleCanvasRange(-1, -1);
    scrollDirection_ = 1;
    thumbnailFirst_ = thumbnailLast_ = -1;
    gridFirst_ = gridLast_ = gridBufferFirst_ = gridBufferLast_ = -1;
    gridPixelW_ = gridPixelH_ = 0;
    worker_->setThumbnailRange(-1, -1);
    text_.clear();
    textRequested_.clear();
    uiGlyphs_.clear();
    annotations_.clear();
    requestedAnnotations_.clear();
    bytes_ = 0;
    stats_ = CacheStats{};
    pageCount_ = 0;
    error_.clear();
    static std::atomic<std::uint64_t> s_salt{0};
    salt_ = ++s_salt;
    staleKeys_.clear();

    // An OS-backed mapping exposes PDFium's random-access file API without
    // privately allocating the entire PDF on every open. Large PDFs fault
    // only the ranges touched by parsing and the visible pages.
    std::string err;
    if (!worker_->openFile(path, &err)) {
        error_ = err.empty() ? "PDFium refused or could not map " + path : err;
        return false;
    }

    pageCount_ = worker_->pageCount();
    outline_.clear();
    lastOutlineRevision_ = worker_->outlineRevision();
    lastGeometryRevision_ = worker_->geometryRevision();
    for (const auto& b : worker_->outline())
        outline_.push_back(OutlineEntry{b.title, b.page, b.depth});
    path_ = path;
    return pageCount_ > 0;
}

folio::ui::PageSizePt PdfiumPageSource::pageSize(int index) const {
    if (index < 0 || index >= pageCount_) return {};
    const auto m = worker_->pageMeta(index);
    if (m.widthPt <= 0.0 || m.heightPt <= 0.0) return {};
    // No rotation swap here. FPDF_GetPageSizeByIndexF already reports the
    // *displayed* size - an A4 page with /Rotate 90 comes back as 842x595,
    // not 595x842. Swapping again turned one rotation into two, which went
    // unnoticed until a document in the corpus actually had /Rotate set.
    return {static_cast<float>(m.widthPt), static_cast<float>(m.heightPt)};
}

std::string PdfiumPageSource::describe() const {
    if (pageCount_ <= 0) return "no document";
    const auto slash = path_.find_last_of("/\\");
    return slash == std::string::npos ? path_ : path_.substr(slash + 1);
}

const std::vector<PdfAnnotationInfo>* PdfiumPageSource::annotationsForPage(int page) {
    if (page < 0 || page >= pageCount_) return nullptr;
    if (auto it = annotations_.find(page); it != annotations_.end())
        return &it->second;
    if (requestedAnnotations_.insert(page).second)
        worker_->requestAnnotations(page);
    return nullptr;
}

void PdfiumPageSource::suppressSourceAnnotations(PdfiumWorker::Suppressed indices) {
    // Tab activation reloads the same sidecar. A no-op suppression update
    // must not erase the parked preview and force a white-page re-render.
    if (!worker_->setSuppressed(std::move(indices))) return;
    suspendVisualCaches();
    annotations_.clear();
    // Old GPU tile keys were collected by suspendVisualCaches(). A fresh
    // worker revision will produce a correct PDF base under Folio overlays.
}

bool PdfiumPageSource::requestExport(std::vector<AnnotSpec> marks,
                                     std::string output) {
    if (!isOpen() || exportResult_) return false;
    return worker_->requestExport(path_, std::move(output), std::move(marks));
}

std::optional<PdfiumWorker::Result> PdfiumPageSource::takeExportResult() {
    auto result = std::move(exportResult_);
    exportResult_.reset();
    return result;
}

bool PdfiumPageSource::tick() {
    bool changed = false;
    if (lastGeometryRevision_ != worker_->geometryRevision()) {
        lastGeometryRevision_ = worker_->geometryRevision();
        changed = true;
    }
    const auto revision = worker_->outlineRevision();
    if (revision != lastOutlineRevision_) {
        outline_.clear();
        for (const auto& b : worker_->outline())
            outline_.push_back(OutlineEntry{b.title, b.page, b.depth});
        lastOutlineRevision_ = revision;
        changed = true;
    }
    auto results = worker_->drain();
    if (results.empty()) return changed;
    for (auto& r : results) {
        if (r.kind == PdfiumWorker::Kind::Export) {
            exportResult_ = std::move(r);
            changed = true;
            continue;
        }
        if (r.kind == PdfiumWorker::Kind::Annotations) {
            if (!requestedAnnotations_.count(r.page)) continue;
            requestedAnnotations_.erase(r.page);
            annotations_[r.page] = std::move(r.annotations);
            changed = true;
            continue;
        }
        if (r.kind == PdfiumWorker::Kind::Text) {
            if (!textRequested_.count(r.page)) continue; // cancelled tab
            text_[r.page] = std::move(r.text);
            changed = true;
            continue;
        }
        if (r.kind == PdfiumWorker::Kind::Tile) {
            const TileId id{r.page, r.requestedWidth, r.spec.tx, r.spec.ty};
            if (!requestedTiles_.count(tileRequestKey(id)) || !r.tile) continue;
            VisibleTile entry;
            entry.rect = r.spec.rect;
            entry.lastUsed = ++clock_;
            entry.pixels = std::move(r.tile);
            auto& slot = tileCache_[id];
            if (slot.pixels) {
                tileBytes_ -= slot.pixels->pixels.size();
                staleKeys_.push_back(tileImageKey(id));
            }
            tileBytes_ += entry.pixels->pixels.size();
            slot = std::move(entry);
            changed = true;
            continue;
        }
        // Results from a suspended tab have no live request marker.
        if (!requested_.count(key(r.page, r.requestedWidth))) continue;
        if (focusPage_ >= 0 && std::abs(r.page - focusPage_) > 2 &&
            !visibleReadingPage(r.page) &&
            !visibleThumbnail(r.page, r.requestedWidth) &&
            !gridBuffered(r.page)) {
            requested_.erase(key(r.page, r.requestedWidth));
            continue; // neither the canvas nor the Pages panel wants it
        }
        if (!r.tile) {
            const auto preview = previewWidth_.find(r.page);
            const bool wasPreview = preview != previewWidth_.end() &&
                                    preview->second == r.requestedWidth;
            if (wasPreview)
                failedPreview_.insert(r.page); // don't spin on an unrenderable preview
            // A failed high-resolution render must not leave a permanent
            // white page. Try again at half resolution on the next frame.
            const bool stillWanted =
                requested_.erase(key(r.page, r.requestedWidth)) != 0;
            if (stillWanted && r.requestedWidth > 256 && !wasPreview) {
                failedWidth_[r.page] = r.requestedWidth;
                changed = true;
            }
            continue;
        }
        const std::size_t sz = r.tile->pixels.size();
        Rendered entry;
        entry.pxW = r.pxW;
        entry.pxH = r.pxH;
        entry.lastUsed = ++clock_;
        entry.tile = std::move(r.tile);

        const auto k = key(r.page, r.requestedWidth);
        if (auto it = cache_.find(k); it != cache_.end()) {
            bytes_ -= it->second.tile ? it->second.tile->pixels.size() : 0;
            staleKeys_.push_back(imageKey(r.page, r.requestedWidth));
        }
        cache_[k] = std::move(entry);
        bytes_ += sz;
        changed = true;
    }
    evictTo(kByteBudget);
    evictTilesTo(kByteBudget);
    stats_.pages = cache_.size() + tileCache_.size();
    stats_.bytes = bytes_ + tileBytes_;
    return changed;
}

bool PdfiumPageSource::pending() const { return worker_->hasResults(); }

void PdfiumPageSource::setVisibleReadingPages(int first, int last) {
    if (first < 0 || last < first || last >= pageCount_) first = last = -1;
    if (first == readingFirst_ && last == readingLast_) return;
    readingFirst_ = first;
    readingLast_ = last;
    worker_->setVisibleCanvasRange(first, last);
    // Ask for the REAL geometry of all intersecting pages now. The worker
    // picks those jobs ahead of sharp tiles, so a partly visible next page
    // cannot wait behind a long raster queue with provisional page-1 size.
    for (int p = first; p >= 0 && p <= last; ++p)
        worker_->requestMeta(p);
}

void PdfiumPageSource::setFocusPage(int page) {
    if (page < 0 || page >= pageCount_ || page == focusPage_) return;
    if (focusPage_ >= 0)
        scrollDirection_ = page > focusPage_ ? 1 : -1;
    focusPage_ = page;
    for (auto it = adjacentPreviewQueued_.begin(); it != adjacentPreviewQueued_.end();)
        it = std::abs(*it - page) > 2 ? adjacentPreviewQueued_.erase(it)
                                         : std::next(it);
    worker_->setFocusPage(page);
    if (gridFirst_ < 0)
        for (int p = std::max(0, page - 3); p <= page + 4 && p < pageCount_; ++p)
            worker_->requestMeta(p);
    // Discard superseded work as well as its markers: otherwise returning
    // to a page can find an old marker for a job that was never rendered.
    worker_->dropQueuedOutside(page, 2);
    for (auto it = requested_.begin(); it != requested_.end();) {
        const int p = static_cast<int>(*it >> 32);
        const int width = static_cast<int>(*it & 0xFFFFFFFFu);
        it = std::abs(p - page) > 2 && !visibleReadingPage(p) &&
             !visibleThumbnail(p, width) && !gridBuffered(p)
            ? requested_.erase(it) : std::next(it);
    }
    if (gridFirst_ < 0) trimTo(page, 2);
    for (auto it = previewWidth_.begin(); it != previewWidth_.end();)
        it = std::abs(it->first - page) > 2 && !visibleReadingPage(it->first)
            ? previewWidth_.erase(it) : std::next(it);
    for (auto it = failedPreview_.begin(); it != failedPreview_.end();)
        it = std::abs(*it - page) > 2 ? failedPreview_.erase(it) : std::next(it);
}

void PdfiumPageSource::setVisibleGridRange(int first, int last,
                                           int bufferFirst, int bufferLast,
                                           int pixelW, int pixelH) {
    if (first == gridFirst_ && last == gridLast_ &&
        bufferFirst == gridBufferFirst_ && bufferLast == gridBufferLast_ &&
        pixelW == gridPixelW_ && pixelH == gridPixelH_) return;
    const bool wasGrid = gridFirst_ >= 0;
    gridFirst_ = first; gridLast_ = last;
    gridBufferFirst_ = bufferFirst; gridBufferLast_ = bufferLast;
    gridPixelW_ = pixelW; gridPixelH_ = pixelH;
    worker_->setGridRange(first, last, bufferFirst, bufferLast);
    if (first >= 0 && !wasGrid) {
        // The reading view's full-DPI tiles are not useful in the overview.
        // Release them immediately instead of holding the previous zoom's
        // page-sized GPU/CPU cache through a long grid-scrolling session.
        worker_->dropQueuedOutside(focusPage_, -1);
        for (const auto& [id, entry] : tileCache_)
            staleKeys_.push_back(tileImageKey(id));
        tileCache_.clear();
        requestedTiles_.clear();
        nextRequestedTiles_.clear();
        tileBytes_ = 0;
    }
    if (first < 0) {
        if (wasGrid && focusPage_ >= 0) trimTo(focusPage_, 2);
        return;
    }
    // Drop stale offscreen markers, not just queued worker jobs; otherwise
    // returning to a row sees a cancelled request and leaves its cell white.
    for (auto it = requested_.begin(); it != requested_.end();) {
        const int page = static_cast<int>(*it >> 32);
        const int width = static_cast<int>(*it & 0xFFFFFFFFu);
        it = !gridBuffered(page) && !visibleThumbnail(page, width)
            ? requested_.erase(it) : std::next(it);
    }
    for (auto it = cache_.begin(); it != cache_.end();) {
        const int page = static_cast<int>(it->first >> 32);
        const int width = static_cast<int>(it->first & 0xFFFFFFFFu);
        if (gridBuffered(page) || visibleThumbnail(page, width) ||
            std::abs(page - focusPage_) <= 1) { ++it; continue; }
        bytes_ -= it->second.tile->pixels.size();
        staleKeys_.push_back(imageKey(page, width));
        requested_.erase(it->first);
        it = cache_.erase(it);
    }
    queueGridBuffer();
}

void PdfiumPageSource::queueGridBuffer() {
    if (gridFirst_ < 0) return;
    for (int page = gridBufferFirst_; page <= gridBufferLast_; ++page) {
        if (!worker_->pageMetaLoaded(page)) {
            worker_->requestMeta(page);
            continue;
        }
        // On-screen cells request their own exact dimensions in drawPage().
        // Only the row above/below is speculative, at the same small cap.
        if (page >= gridFirst_ && page <= gridLast_) continue;
        const auto m = worker_->pageMeta(page);
        if (m.widthPt <= 0 || m.heightPt <= 0) continue;
        const float scale = std::min(gridPixelW_ / static_cast<float>(m.widthPt),
                                      gridPixelH_ / static_cast<float>(m.heightPt));
        const int width = std::max(1, static_cast<int>(std::lround(m.widthPt * scale)));
        if (requested_.insert(key(page, width)).second)
            worker_->requestRender(page, scale, 0, width);
    }
}

void PdfiumPageSource::setVisibleThumbnailRange(int first, int last) {
    if (first == thumbnailFirst_ && last == thumbnailLast_) return;
    thumbnailFirst_ = first;
    thumbnailLast_ = last;
    worker_->setThumbnailRange(first, last);
    // Scrolling the sidebar does not change canvas focus. Cancel markers for
    // thumbnails that have left the panel; keep the new range so arrivals
    // don't get thrown away simply because they are far from the PDF page.
    for (auto it = requested_.begin(); it != requested_.end();) {
        const int p = static_cast<int>(*it >> 32);
        const int width = static_cast<int>(*it & 0xFFFFFFFFu);
        it = std::abs(p - focusPage_) > 2 && width <= 256 &&
             !visibleThumbnail(p, width) && !gridBuffered(p)
            ? requested_.erase(it) : std::next(it);
    }
    if (focusPage_ >= 0 && gridFirst_ < 0) trimTo(focusPage_, 2);
}

std::shared_ptr<const PageText> PdfiumPageSource::pageText(int index) {
    if (index < 0 || index >= pageCount_) return nullptr;
    if (auto it = text_.find(index); it != text_.end()) return it->second;
    if (!textRequested_[index]) {
        textRequested_[index] = true;
        worker_->requestText(index);
    }
    return nullptr;
}

const std::vector<folio::ui::PageGlyph>* PdfiumPageSource::glyphs(int page) {
    if (auto it = uiGlyphs_.find(page); it != uiGlyphs_.end()) return &it->second;
    auto text = pageText(page);           // queues extraction on a miss
    if (!text) return nullptr;
    auto& out = uiGlyphs_[page];
    out.reserve(text->glyphs.size());
    for (const auto& g : text->glyphs)
        out.push_back(folio::ui::PageGlyph{g.code, g.l, g.t, g.r, g.b});
    return &out;
}

void PdfiumPageSource::requestAllText() {
    for (int i = 0; i < pageCount_; ++i) {
        if (text_.count(i) || textRequested_[i]) continue;
        textRequested_[i] = true;
        worker_->requestText(i);
    }
}

void PdfiumPageSource::releaseText(int page) {
    if (std::abs(page - focusPage_) <= 2) return;
    text_.erase(page);
    textRequested_.erase(page);
    uiGlyphs_.erase(page);
}

void PdfiumPageSource::drawPage(IPaintTarget& t, int index, const Rect& dest,
                                float scale, folio::ui::PageTheme theme) {
    if (index < 0 || index >= pageCount_ || dest.empty()) return;

    // Hand back any images whose pixels we have since dropped.
    if (!staleKeys_.empty()) {
        for (std::uint64_t k : staleKeys_) t.invalidateImage(k);
        staleKeys_.clear();
    }

    // The page theme never reaches the rasteriser - it is a filter on the
    // blit. See IPaintTarget::ImageTint.
    const auto tint = theme == folio::ui::PageTheme::Sepia
                          ? IPaintTarget::ImageTint::Sepia
                      : theme == folio::ui::PageTheme::Dark
                          ? IPaintTarget::ImageTint::Invert
                          : IPaintTarget::ImageTint::None;

    if (!worker_->pageMetaLoaded(index)) {
        if (gridFirst_ >= 0) visibleFrameComplete_ = false;
        worker_->requestMeta(index);
        return; // no distorted render while the real page geometry is pending
    }
    const auto m = worker_->pageMeta(index);
    // Likewise, FPDF_RenderPageBitmap applies the page's own /Rotate. The
    // rotation argument is reserved for an extra, user-requested view
    // rotation on top of it; passing the page's own value here rotated
    // everything twice.
    constexpr int kExtraRotation = 0;

    // Target pixel size, capped. The cap is on the raster, not the layout:
    // past it the blit upscales rather than the page shrinking.
    float eff = scale;
    const double longPt = std::max(m.widthPt, m.heightPt);
    if (longPt * eff > kMaxLongEdge)
        eff = static_cast<float>(kMaxLongEdge / std::max(1.0, longPt));

    if (auto fail = failedWidth_.find(index); fail != failedWidth_.end() &&
        m.widthPt * eff >= fail->second && fail->second > 256)
        eff = std::min(eff, static_cast<float>(fail->second * 0.5 / std::max(1.0, m.widthPt)));
    const int pxW = std::max(1, static_cast<int>(std::lround(m.widthPt * eff)));

    // 1. exact size - the normal path.
    if (auto it = cache_.find(key(index, pxW)); it != cache_.end() && it->second.tile) {
        it->second.lastUsed = ++clock_;
        it->second.lastFrame = frame_;
        ++stats_.hits;
        const auto& tile = *it->second.tile;
        t.drawImageBGRA(tile.pixels.data(), tile.width, tile.height, tile.stride,
                        dest, imageKey(index, pxW),
                        IPaintTarget::ImageFit::Final, tint);
        return;
    }

    if (gridFirst_ >= 0) visibleFrameComplete_ = false;
    ++stats_.misses;
    if (pxW >= 768) {
        for (auto it = requested_.begin(); it != requested_.end();)
            it = static_cast<int>(*it >> 32) == index &&
                 static_cast<int>(*it & 0xFFFFFFFFu) >= 768 &&
                 static_cast<int>(*it & 0xFFFFFFFFu) != pxW
                ? requested_.erase(it) : std::next(it);
    }
    if (requested_.insert(key(index, pxW)).second)
        worker_->requestRender(index, eff, kExtraRotation, pxW);

    // 2. any other raster for this page, stretched. Keeps a zoom gesture
    //    continuous instead of flashing blank between scales.
    const std::uint64_t lo = key(index, 0);
    const std::uint64_t hi = key(index, 0xFFFFFFFFu);
    auto best = cache_.end();
    int bestDelta = 0;
    for (auto it = cache_.lower_bound(lo); it != cache_.end() && it->first <= hi;
         ++it) {
        if (!it->second.tile) continue;
        const int d = std::abs(it->second.pxW - pxW);
        if (best == cache_.end() || d < bestDelta) { best = it; bestDelta = d; }
    }
    if (best != cache_.end()) {
        best->second.lastUsed = ++clock_;
        best->second.lastFrame = frame_;
        ++stats_.proxies;
        const auto& tile = *best->second.tile;
        t.drawImageBGRA(tile.pixels.data(), tile.width, tile.height, tile.stride,
                        dest, imageKey(index, static_cast<int>(best->first & 0xFFFFFFFFu)),
                        IPaintTarget::ImageFit::Transient, tint);
        return;
    }

    // 3. nothing yet - paper.
    t.fillRect(dest, folio::render::Color{0xFF, 0xFF, 0xFF, 0xFF});
}

void PdfiumPageSource::beginVisibleFrame() {
    // ReaderScreen begins its frame before PageStage::onPaint calls tick().
    // Install completions against the *previous* frame's wanted set first;
    // clearing markers earlier would discard every finished visible tile.
    tick();
    ++frame_;
    nextRequestedTiles_.clear();
    // Do not clear the queue here: scrolling animations can repaint at 45 Hz
    // while a visible tile is still waiting. Keep identical jobs intact and
    // cancel only those the completed frame no longer needs.
}

void PdfiumPageSource::endVisibleFrame() {
    // Stage.tick() may run between beginVisibleFrame and this point. Keep
    // the previous set alive until its last possible completion is drained.
    worker_->retainQueuedTiles(nextRequestedTiles_);
    requestedTiles_ = std::move(nextRequestedTiles_);
    nextRequestedTiles_.clear();
    if (gridFirst_ >= 0) {
        queueGridBuffer();
        return; // grid already owns every visible/adjacent page's priority
    }
    if (focusPage_ < 0) return;
    // Tab switches and first opens prioritize the actual visible page.
    // After its first pixels, queue only tiny proxies: one in each direction
    // and a second page ahead in the direction of scrolling. This bridges
    // a fast fling without retaining an offscreen high-DPI tile grid.
    const auto first = cache_.lower_bound(key(focusPage_, 0));
    bool ready = first != cache_.end() &&
                 static_cast<int>(first->first >> 32) == focusPage_;
    if (!ready)
        for (const auto& [id, tile] : tileCache_)
            if (std::get<0>(id) == focusPage_) { ready = true; break; }
    if (!ready) return;
    for (int offset : {scrollDirection_, -scrollDirection_, 2 * scrollDirection_}) {
        const int p = focusPage_ + offset;
        if (p < 0 || p >= pageCount_ || adjacentPreviewQueued_.count(p)) continue;
        auto cached = cache_.lower_bound(key(p, 0));
        if (cached != cache_.end() && static_cast<int>(cached->first >> 32) == p)
            continue;
        const auto meta = worker_->pageMeta(p);
        if (meta.widthPt <= 0 || meta.heightPt <= 0) continue;
        const float scale = std::min(240.f / static_cast<float>(meta.widthPt),
                                     320.f / static_cast<float>(meta.heightPt));
        const int width = std::max(1, static_cast<int>(
            std::lround(static_cast<float>(meta.widthPt) * scale)));
        adjacentPreviewQueued_.insert(p);
        if (requested_.insert(key(p, width)).second)
            // Some pages still advertise provisional geometry. Recalculate
            // the cap on the PDFium thread against this page's real size.
            worker_->requestRender(p, scale, 0, width, true);
    }
}

bool PdfiumPageSource::drawCachedRegion(IPaintTarget& t, int index,
                                         const Rect& dest, const Rect& visible,
                                         float scale, folio::ui::PageTheme theme) {
    if (index < 0 || index >= pageCount_ || dest.empty() || visible.empty() ||
        scale <= 0.f) return false;
    if (!worker_->pageMetaLoaded(index)) {
        worker_->requestMeta(index);
        return false;
    }
    const auto m = worker_->pageMeta(index);
    if (m.widthPt <= 0 || m.heightPt <= 0) return false;
    const core::TileGrid grid(static_cast<float>(m.widthPt),
                              static_cast<float>(m.heightPt), scale, 0);
    if (grid.deviceWidth() <= 0 || grid.deviceHeight() <= 0) return false;
    const int x0 = std::clamp(static_cast<int>(std::floor(
        (visible.x - dest.x) * grid.deviceWidth() / dest.w)), 0, grid.deviceWidth());
    const int y0 = std::clamp(static_cast<int>(std::floor(
        (visible.y - dest.y) * grid.deviceHeight() / dest.h)), 0, grid.deviceHeight());
    const int x1 = std::clamp(static_cast<int>(std::ceil(
        (visible.right() - dest.x) * grid.deviceWidth() / dest.w)), 0, grid.deviceWidth());
    const int y1 = std::clamp(static_cast<int>(std::ceil(
        (visible.bottom() - dest.y) * grid.deviceHeight() / dest.h)), 0, grid.deviceHeight());
    if (x1 <= x0 || y1 <= y0) return false;
    const auto specs = grid.tilesForViewport(core::DeviceRect{x0, y0, x1 - x0, y1 - y0});
    auto proxy = cache_.end();
    for (auto it = cache_.lower_bound(key(index, 0));
         it != cache_.end() && static_cast<int>(it->first >> 32) == index; ++it)
        if (it->second.tile && (proxy == cache_.end() || it->second.pxW > proxy->second.pxW))
            proxy = it;
    // Exactly one old-scale generation, only when no full-page proxy exists.
    int oldWidth = -1, bestDelta = 0;
    if (proxy == cache_.end()) {
        for (const auto& [id, tile] : tileCache_) {
            if (std::get<0>(id) != index || !tile.pixels ||
                std::get<1>(id) == grid.deviceWidth()) continue;
            const int d = std::abs(std::get<1>(id) - grid.deviceWidth());
            if (oldWidth < 0 || d < bestDelta) {
                oldWidth = std::get<1>(id);
                bestDelta = d;
            }
        }
    }
    bool hasPixels = proxy != cache_.end();
    for (const auto& spec : specs)
        hasPixels = hasPixels || tileCache_.count(
            TileId{index, grid.deviceWidth(), spec.tx, spec.ty}) != 0;
    if (!hasPixels && oldWidth > 0) {
        for (const auto& [id, tile] : tileCache_) {
            if (std::get<0>(id) != index || std::get<1>(id) != oldWidth ||
                !tile.pixels) continue;
            const core::TileGrid oldGrid(static_cast<float>(m.widthPt),
                static_cast<float>(m.heightPt),
                static_cast<float>(oldWidth) / static_cast<float>(m.widthPt), 0);
            const Rect r{dest.x + tile.rect.x * dest.w / oldGrid.deviceWidth(),
                         dest.y + tile.rect.y * dest.h / oldGrid.deviceHeight(),
                         tile.rect.w * dest.w / oldGrid.deviceWidth(),
                         tile.rect.h * dest.h / oldGrid.deviceHeight()};
            if (r.x < visible.right() && r.right() > visible.x &&
                r.y < visible.bottom() && r.bottom() > visible.y) {
                hasPixels = true; break;
            }
        }
    }
    if (!hasPixels) return false;
    // Cached blits only. Do NOT touch requestedTiles_, the worker, viewport
    // scheduling, image invalidation or cache lifetime from pointer moves.
    const auto tint = theme == folio::ui::PageTheme::Sepia ? IPaintTarget::ImageTint::Sepia
                    : theme == folio::ui::PageTheme::Dark ? IPaintTarget::ImageTint::Invert
                    : IPaintTarget::ImageTint::None;
    const auto paper = theme == folio::ui::PageTheme::Dark
        ? folio::render::Color{0, 0, 0, 255}
        : theme == folio::ui::PageTheme::Sepia
        ? folio::render::Color{241, 228, 200, 255}
        : folio::render::Color{255, 255, 255, 255};
    t.fillRect(visible, paper);
    t.save();
    t.clipRect(visible);
    if (proxy != cache_.end()) {
        const auto& pixels = *proxy->second.tile;
        t.drawImageBGRA(pixels.pixels.data(), pixels.width, pixels.height, pixels.stride,
                        dest, imageKey(index, proxy->second.pxW),
                        IPaintTarget::ImageFit::Transient, tint);
    } else if (oldWidth > 0) {
        const core::TileGrid oldGrid(static_cast<float>(m.widthPt),
            static_cast<float>(m.heightPt),
            static_cast<float>(oldWidth) / static_cast<float>(m.widthPt), 0);
        for (const auto& [id, tile] : tileCache_) {
            if (std::get<0>(id) != index || std::get<1>(id) != oldWidth ||
                !tile.pixels) continue;
            const Rect r{dest.x + tile.rect.x * dest.w / oldGrid.deviceWidth(),
                         dest.y + tile.rect.y * dest.h / oldGrid.deviceHeight(),
                         tile.rect.w * dest.w / oldGrid.deviceWidth(),
                         tile.rect.h * dest.h / oldGrid.deviceHeight()};
            if (r.x >= visible.right() || r.right() <= visible.x ||
                r.y >= visible.bottom() || r.bottom() <= visible.y) continue;
            const auto& pixels = *tile.pixels;
            t.drawImageBGRA(pixels.pixels.data(), pixels.width, pixels.height,
                            pixels.stride, r, tileImageKey(id),
                            IPaintTarget::ImageFit::Transient, tint);
        }
    }
    for (const auto& spec : specs) {
        const TileId id{index, grid.deviceWidth(), spec.tx, spec.ty};
        if (auto it = tileCache_.find(id);
            it != tileCache_.end() && it->second.pixels) {
            const Rect r{dest.x + spec.rect.x * dest.w / grid.deviceWidth(),
                         dest.y + spec.rect.y * dest.h / grid.deviceHeight(),
                         spec.rect.w * dest.w / grid.deviceWidth(),
                         spec.rect.h * dest.h / grid.deviceHeight()};
            const auto& pixels = *it->second.pixels;
            t.drawImageBGRA(pixels.pixels.data(), pixels.width, pixels.height,
                            pixels.stride, r, tileImageKey(id),
                            IPaintTarget::ImageFit::Final, tint);
        }
    }
    t.restore();
    return true;
}

void PdfiumPageSource::drawVisiblePage(IPaintTarget& t, int index, const Rect& dest,
                                       const Rect& visible, float scale,
                                       folio::ui::PageTheme theme) {
    if (index < 0 || index >= pageCount_ || dest.empty() || visible.empty()) return;
    if (!staleKeys_.empty()) {
        for (auto k : staleKeys_) t.invalidateImage(k);
        staleKeys_.clear();
    }
    if (!worker_->pageMetaLoaded(index)) {
        visibleFrameComplete_ = false;
        worker_->requestMeta(index);
        return; // paper until the real aspect ratio is known
    }
    const auto m = worker_->pageMeta(index);
    if (m.widthPt <= 0 || m.heightPt <= 0 || scale <= 0) return;
    // Pixel budget is the *viewport*, not a cap on the page dimensions.
    // Scale is fixed for the full page so scrolling doesn't shift tile seams.
    const core::TileGrid grid(static_cast<float>(m.widthPt),
                              static_cast<float>(m.heightPt), scale, 0);
    if (grid.deviceWidth() <= 0 || grid.deviceHeight() <= 0) return;
    const auto mapX = [&](float x) { return (x - dest.x) * grid.deviceWidth() / dest.w; };
    const auto mapY = [&](float y) { return (y - dest.y) * grid.deviceHeight() / dest.h; };
    const int x0 = std::clamp(static_cast<int>(std::floor(mapX(visible.x))), 0, grid.deviceWidth());
    const int y0 = std::clamp(static_cast<int>(std::floor(mapY(visible.y))), 0, grid.deviceHeight());
    const int x1 = std::clamp(static_cast<int>(std::ceil(mapX(visible.right()))), 0, grid.deviceWidth());
    const int y1 = std::clamp(static_cast<int>(std::ceil(mapY(visible.bottom()))), 0, grid.deviceHeight());
    if (x1 <= x0 || y1 <= y0) return;
    const auto tint = theme == folio::ui::PageTheme::Sepia ? IPaintTarget::ImageTint::Sepia
                    : theme == folio::ui::PageTheme::Dark ? IPaintTarget::ImageTint::Invert
                    : IPaintTarget::ImageTint::None;
    t.fillRect(visible, folio::render::Color{255, 255, 255, 255});
    t.save();
    t.clipRect(visible);
    const auto specs = grid.tilesForViewport(core::DeviceRect{x0, y0, x1 - x0, y1 - y0});
    bool allExact = true;
    for (const auto& spec : specs)
        if (!tileCache_.count(TileId{index, grid.deviceWidth(), spec.tx, spec.ty})) {
            allExact = false;
            break;
        }
    if (!allExact) visibleFrameComplete_ = false;
    // A small thumbnail is a cheap continuous proxy during zoom/scroll.
    // It is never submitted as a full-size canvas render.
    auto proxy = cache_.end();
    for (auto it = cache_.lower_bound(key(index, 0));
         it != cache_.end() && static_cast<int>(it->first >> 32) == index; ++it)
        if (it->second.tile && (proxy == cache_.end() || it->second.pxW > proxy->second.pxW))
            proxy = it;
    if (!allExact && proxy == cache_.end() && !failedPreview_.count(index)) {
        // First paint should show the whole page quickly, not a growing
        // checkerboard of white 512px tiles. Keep this proxy bounded to
        // <=512 x 768 device pixels (~1.5 MiB) even for very tall pages.
        const float previewScale = std::min(512.f / static_cast<float>(m.widthPt),
                                            768.f / static_cast<float>(m.heightPt));
        const int previewW = std::max(1, static_cast<int>(
            std::lround(static_cast<float>(m.widthPt) * previewScale)));
        previewWidth_[index] = previewW;
        if (requested_.insert(key(index, previewW)).second)
            worker_->requestRender(index, previewScale, 0, previewW, true);
    }
    if (proxy != cache_.end()) {
        proxy->second.lastUsed = ++clock_;
        proxy->second.lastFrame = frame_;
        const auto& pixels = *proxy->second.tile;
        t.drawImageBGRA(pixels.pixels.data(), pixels.width, pixels.height, pixels.stride,
                        dest, imageKey(index, static_cast<int>(proxy->first & 0xFFFFFFFFu)),
                        IPaintTarget::ImageFit::Transient, tint);
    }
    // Centre-first only visible specs are submitted; no speculative prefetch.
    if (!allExact) {
        // During an animated zoom rasterScale is held steady; when it snaps
        // to the final level, stretch the previous tiles instead of showing
        // paper for every frame until the new tiles are ready. Draw only one
        // old generation to bound extra image copies and GPU composites.
        int oldWidth = -1;
        int bestDelta = 0;
        for (const auto& [id, tile] : tileCache_) {
            if (std::get<0>(id) != index || std::get<1>(id) == grid.deviceWidth()) continue;
            const int delta = std::abs(std::get<1>(id) - grid.deviceWidth());
            if (oldWidth < 0 || delta < bestDelta) {
                oldWidth = std::get<1>(id);
                bestDelta = delta;
            }
        }
        if (oldWidth > 0) {
            const float oldScale = static_cast<float>(oldWidth) / static_cast<float>(m.widthPt);
            const core::TileGrid oldGrid(static_cast<float>(m.widthPt),
                                         static_cast<float>(m.heightPt), oldScale, 0);
            for (auto& [id, tile] : tileCache_) {
                if (std::get<0>(id) != index || std::get<1>(id) != oldWidth) continue;
                const Rect r{dest.x + tile.rect.x * dest.w / oldGrid.deviceWidth(),
                             dest.y + tile.rect.y * dest.h / oldGrid.deviceHeight(),
                             tile.rect.w * dest.w / oldGrid.deviceWidth(),
                             tile.rect.h * dest.h / oldGrid.deviceHeight()};
                if (r.right() <= visible.x || r.x >= visible.right() ||
                    r.bottom() <= visible.y || r.y >= visible.bottom()) continue;
                tile.lastUsed = ++clock_;
                tile.lastFrame = frame_;
                const auto& pixels = *tile.pixels;
                t.drawImageBGRA(pixels.pixels.data(), pixels.width, pixels.height,
                                pixels.stride, r, tileImageKey(id),
                                IPaintTarget::ImageFit::Transient, tint);
            }
        }
    }
    for (std::size_t order = 0; order < specs.size(); ++order) {
        const auto& spec = specs[order];
        const TileId id{index, grid.deviceWidth(), spec.tx, spec.ty};
        const auto k = tileRequestKey(id);
        nextRequestedTiles_.insert(k);
        const Rect tileDest{dest.x + spec.rect.x * dest.w / grid.deviceWidth(),
                            dest.y + spec.rect.y * dest.h / grid.deviceHeight(),
                            spec.rect.w * dest.w / grid.deviceWidth(),
                            spec.rect.h * dest.h / grid.deviceHeight()};
        if (auto it = tileCache_.find(id); it != tileCache_.end() && it->second.pixels) {
            it->second.lastUsed = ++clock_;
            it->second.lastFrame = frame_;
            ++stats_.hits;
            const auto& pixels = *it->second.pixels;
            t.drawImageBGRA(pixels.pixels.data(), pixels.width, pixels.height, pixels.stride,
                            tileDest, tileImageKey(id), IPaintTarget::ImageFit::Final, tint);
        } else {
            ++stats_.misses;
            worker_->requestTile(index, scale, grid.deviceWidth(), spec,
                                 static_cast<int>(order));
        }
    }
    t.restore();
}

void PdfiumPageSource::evictTilesTo(std::size_t budget) {
    while (tileBytes_ + bytes_ > budget && !tileCache_.empty()) {
        auto oldest = tileCache_.end();
        for (auto it = tileCache_.begin(); it != tileCache_.end(); ++it) {
            const auto k = tileRequestKey(it->first);
            // Keep the last presented viewport *and* the one currently being
            // painted. A soft-budget overrun for visible tiles is preferable
            // to evicting the first tile as the last tile arrives, which
            // otherwise leaves a permanent checkerboard at large DPR/zoom.
            if (it->second.lastFrame == frame_ || requestedTiles_.count(k) ||
                nextRequestedTiles_.count(k)) continue;
            if (oldest == tileCache_.end() || it->second.lastUsed < oldest->second.lastUsed)
                oldest = it;
        }
        if (oldest == tileCache_.end()) break;
        tileBytes_ -= oldest->second.pixels->pixels.size();
        staleKeys_.push_back(tileImageKey(oldest->first));
        tileCache_.erase(oldest);
    }
}

void PdfiumPageSource::trimIdle() {
    // A one-shot pass, not a periodic wakeup. The last completed paint is
    // already in the retained surface; its source pixels remain available.
    for (auto it = tileCache_.begin(); it != tileCache_.end();) {
        if (it->second.lastFrame == frame_) { ++it; continue; }
        tileBytes_ -= it->second.pixels->pixels.size();
        staleKeys_.push_back(tileImageKey(it->first));
        it = tileCache_.erase(it);
    }
    for (auto it = cache_.begin(); it != cache_.end();) {
        const int p = static_cast<int>(it->first >> 32);
        const bool tinyNeighbour = focusPage_ >= 0 &&
            adjacentPreviewQueued_.count(p) && std::abs(p - focusPage_) <= 2 &&
            it->second.pxW <= 240 && it->second.pxH <= 320;
        const bool gridProxy = gridBuffered(p) && it->second.pxW <= gridPixelW_ &&
                               it->second.pxH <= gridPixelH_;
        if (it->second.lastFrame == frame_ || tinyNeighbour || gridProxy) {
            ++it; continue;
        }
        bytes_ -= it->second.tile->pixels.size();
        staleKeys_.push_back(imageKey(static_cast<int>(it->first >> 32),
                                      static_cast<int>(it->first & 0xFFFFFFFFu)));
        requested_.erase(it->first);
        adjacentPreviewQueued_.erase(p);
        it = cache_.erase(it);
    }
    for (auto it = text_.begin(); it != text_.end();) {
        if (std::abs(it->first - focusPage_) <= 1) { ++it; continue; }
        uiGlyphs_.erase(it->first);
        textRequested_.erase(it->first);
        it = text_.erase(it);
    }
    stats_.pages = cache_.size() + tileCache_.size();
    stats_.bytes = bytes_ + tileBytes_;
}

void PdfiumPageSource::parkVisualCaches(int page, bool retainNearby) {
    // Tab switch: one screenshot-sized preview. Minimise: that preview plus
    // tiny adjacent proxies, never 4K tiles. GPU/Skia image copies are
    // purged by the shell independently; this CPU budget survives restore.
    worker_->suspendVisualWork();
    readingFirst_ = readingLast_ = -1;
    std::unordered_set<std::uint64_t> keepKeys;
    std::size_t keepBytes = 0;
    const auto choose = [&](int p, int maxW, int maxH) {
        auto best = cache_.end();
        for (auto it = cache_.lower_bound(key(p, 0));
             it != cache_.end() && static_cast<int>(it->first >> 32) == p; ++it) {
            if (!it->second.tile || it->second.pxW > maxW ||
                it->second.pxH > maxH) continue;
            if (best == cache_.end() || it->second.pxW > best->second.pxW)
                best = it;
        }
        if (best != cache_.end()) {
            keepKeys.insert(best->first);
            keepBytes += best->second.tile->pixels.size();
        }
    };
    choose(page, 512, 768);
    if (retainNearby) {
        for (int p : {page - 1, page + 1, page + 2})
            if (p >= 0 && p < pageCount_) choose(p, 240, 320);
    }
    for (const auto& [k, v] : tileCache_) staleKeys_.push_back(tileImageKey(k));
    tileCache_.clear();
    tileBytes_ = 0;
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (keepKeys.count(it->first)) { ++it; continue; }
        staleKeys_.push_back(imageKey(static_cast<int>(it->first >> 32),
                                      static_cast<int>(it->first & 0xFFFFFFFFu)));
        it = cache_.erase(it);
    }
    bytes_ = keepBytes;
    requestedTiles_.clear(); nextRequestedTiles_.clear();
    requested_ = std::move(keepKeys);
    previewWidth_.clear(); failedPreview_.clear(); adjacentPreviewQueued_.clear();
    if (retainNearby)
        for (int p : {page - 1, page + 1, page + 2})
            if (p >= 0 && p < pageCount_) adjacentPreviewQueued_.insert(p);
    text_.clear(); textRequested_.clear(); uiGlyphs_.clear();
    // Notes and imported marks are re-indexed on tab activation. Keeping
    // parsed annotations for every visited page in every inactive source
    // can dominate RAM even after all its visible pixels have been dropped.
    annotations_.clear();
    requestedAnnotations_.clear();
    stats_.pages = cache_.size();
    stats_.bytes = bytes_;
}

void PdfiumPageSource::suspendVisualCaches() {
    worker_->suspendVisualWork();
    readingFirst_ = readingLast_ = -1;
    for (const auto& [k, v] : tileCache_) staleKeys_.push_back(tileImageKey(k));
    for (const auto& [k, v] : cache_)
        staleKeys_.push_back(imageKey(static_cast<int>(k >> 32), static_cast<int>(k & 0xFFFFFFFFu)));
    tileCache_.clear();
    cache_.clear();
    tileBytes_ = bytes_ = 0;
    requestedTiles_.clear();
    nextRequestedTiles_.clear();
    requested_.clear();
    previewWidth_.clear();
    failedPreview_.clear();
    adjacentPreviewQueued_.clear();
    text_.clear();
    textRequested_.clear();
    uiGlyphs_.clear();
    annotations_.clear();
    requestedAnnotations_.clear();
    stats_.pages = stats_.bytes = 0;
}

void PdfiumPageSource::evictTo(std::size_t budget) {
    while (bytes_ > budget && cache_.size() > 1) {
        // Keep at least one raster for each visible/adjacent page. At 4K a
        // single page can nearly fill the soft budget: evicting the other
        // visible page then makes every zoom/hover frame request it again,
        // alternately drawing blank paper and thrashing PDFium/GPU memory.
        auto oldest = cache_.end();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            const int p = static_cast<int>(it->first >> 32);
            const bool nearby = gridBuffered(p) || visibleReadingPage(p) ||
                (focusPage_ >= 0 && std::abs(p - focusPage_) <= 1);
            const auto next = std::next(it);
            const bool lastForPage =
                (it == cache_.begin() || static_cast<int>(std::prev(it)->first >> 32) != p) &&
                (next == cache_.end() || static_cast<int>(next->first >> 32) != p);
            if ((nearby || visibleThumbnail(p,
                    static_cast<int>(it->first & 0xFFFFFFFFu))) && lastForPage) continue;
            if (oldest == cache_.end() || it->second.lastUsed < oldest->second.lastUsed)
                oldest = it;
        }
        if (oldest == cache_.end()) break; // temporary over-budget beats blinking
        const int p = static_cast<int>(oldest->first >> 32);
        bytes_ -= oldest->second.tile ? oldest->second.tile->pixels.size() : 0;
        staleKeys_.push_back(imageKey(p, static_cast<int>(oldest->first & 0xFFFFFFFFu)));
        requested_.erase(oldest->first); // a revisited width must queue a render
        cache_.erase(oldest);
        adjacentPreviewQueued_.erase(p); // no stale marker after eviction
    }
}

void PdfiumPageSource::trimTo(int page, int radius) {
    for (auto it = tileCache_.begin(); it != tileCache_.end();) {
        const int tilePage = std::get<0>(it->first);
        if (std::abs(tilePage - page) <= radius || visibleReadingPage(tilePage)) {
            ++it; continue;
        }
        tileBytes_ -= it->second.pixels->pixels.size();
        staleKeys_.push_back(tileImageKey(it->first));
        it = tileCache_.erase(it);
    }
    for (auto it = cache_.begin(); it != cache_.end();) {
        const int p = static_cast<int>(it->first >> 32);
        const int width = static_cast<int>(it->first & 0xFFFFFFFFu);
        if (std::abs(p - page) > radius && !visibleReadingPage(p) &&
            !visibleThumbnail(p, width) && !gridBuffered(p)) {
            bytes_ -= it->second.tile ? it->second.tile->pixels.size() : 0;
            staleKeys_.push_back(imageKey(p, width));
            requested_.erase(it->first);
            failedWidth_.erase(p);
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = uiGlyphs_.begin(); it != uiGlyphs_.end();)
        it = std::abs(it->first - page) > radius ? uiGlyphs_.erase(it) : std::next(it);
    for (auto it = text_.begin(); it != text_.end();) {
        if (std::abs(it->first - page) > radius) {
            textRequested_.erase(it->first);
            it = text_.erase(it);
        } else {
            ++it;
        }
    }
    stats_.pages = cache_.size() + tileCache_.size();
    stats_.bytes = bytes_ + tileBytes_;
}

}  // namespace folio::engine
