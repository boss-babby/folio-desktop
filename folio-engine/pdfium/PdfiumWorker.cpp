#include "pdfium/PdfiumWorker.h"

#include "pdfium/PdfiumLibrary.h"
#include "pdfium/PdfiumAnnotationIdentity.h"
#include "pdfium/PdfiumAnnotationWriter.h"

#include <fpdf_text.h>
#include <fpdf_annot.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <utility>

namespace folio::engine {
namespace {

/// ASCII-range case folding. Enough for search-as-you-type in Latin scripts;
/// CJK and the rest compare exactly, which is what users of those scripts
/// expect anyway. Full Unicode folding would mean pulling ICU into the engine
/// layer, and Skia already carries one copy.
constexpr char32_t fold(char32_t c) noexcept {
    if (c >= U'A' && c <= U'Z') return c + 32;
    return c;
}

// PDFium's character boxes are in *PDF user space*, not in the displayed
// page's [0,width] x [0,height]. /CropBox can start at a nonzero coordinate,
// and /Rotate changes the axes (FPDF_GetPageSizeByIndexF already swaps the
// displayed width/height). Dividing a raw char box by that size placed the
// text overlay on a different part of the image even though the raster was
// correct. Use PDFium's own page-to-device mapping, including its crop and
// intrinsic rotation. Sample the affine transform once per text page rather
// than making two PDFium calls per glyph. A wide baseline reduces rounding
// from FPDF_PageToDevice's integer device coordinates to <0.001 page units.
struct TextPageMap {
    double x0 = 0, y0 = 0, xx = 0, xy = 0, yx = 0, yy = 0;
    double w = 1, h = 1;
    bool valid = false;

    TextPageMap(FPDF_PAGE page, double widthPt, double heightPt) {
        if (!page || !std::isfinite(widthPt) || !std::isfinite(heightPt) ||
            widthPt <= 0 || heightPt <= 0) return;
        // 32 device pixels/point preserves subpixel text placement even when
        // a PDF has a non-integral crop origin. Cap pathological page sizes.
        const int dw = static_cast<int>(std::clamp(widthPt * 32., 1., 1000000.));
        const int dh = static_cast<int>(std::clamp(heightPt * 32., 1., 1000000.));
        constexpr double basis = 512.;
        int ox = 0, oy = 0, ax = 0, ay = 0, bx = 0, by = 0;
        valid = FPDF_PageToDevice(page, 0, 0, dw, dh, 0,
                                  0, 0, &ox, &oy) &&
                FPDF_PageToDevice(page, 0, 0, dw, dh, 0,
                                  basis, 0, &ax, &ay) &&
                FPDF_PageToDevice(page, 0, 0, dw, dh, 0,
                                  0, basis, &bx, &by);
        x0 = ox; y0 = oy;
        xx = (ax - ox) / basis; xy = (ay - oy) / basis;
        yx = (bx - ox) / basis; yy = (by - oy) / basis;
        w = dw; h = dh;
        valid = valid && (std::abs(xx * yy - xy * yx) > 1e-6);
    }

    void rect(double left, double right, double bottom, double top,
              TextGlyph& g) const {
        if (!valid || !std::isfinite(left) || !std::isfinite(right) ||
            !std::isfinite(bottom) || !std::isfinite(top)) return;
        const auto x = [this](double px, double py) {
            return (x0 + xx * px + yx * py) / w;
        };
        const auto y = [this](double px, double py) {
            return (y0 + xy * px + yy * py) / h;
        };
        const double xs[] = {x(left, bottom), x(left, top),
                             x(right, bottom), x(right, top)};
        const double ys[] = {y(left, bottom), y(left, top),
                             y(right, bottom), y(right, top)};
        const auto [minX, maxX] = std::minmax_element(std::begin(xs), std::end(xs));
        const auto [minY, maxY] = std::minmax_element(std::begin(ys), std::end(ys));
        // PDF text extraction also returns glyphs outside the crop box. They
        // must be searchable but must NEVER paint a giant off-page band.
        if (*maxX <= 0 || *minX >= 1 || *maxY <= 0 || *minY >= 1 ||
            !std::isfinite(*minX) || !std::isfinite(*maxX) ||
            !std::isfinite(*minY) || !std::isfinite(*maxY)) return;
        g.l = static_cast<float>(std::clamp(*minX, 0., 1.));
        g.r = static_cast<float>(std::clamp(*maxX, 0., 1.));
        g.t = static_cast<float>(std::clamp(*minY, 0., 1.));
        g.b = static_cast<float>(std::clamp(*maxY, 0., 1.));
    }
};

}  // namespace

PdfiumWorker::PdfiumWorker() {
    thread_ = std::thread([this] { threadMain(); });
}

PdfiumWorker::~PdfiumWorker() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

PdfiumWorker::PageMeta PdfiumWorker::pageMeta(int index) const {
    std::lock_guard<std::mutex> lk(m_);
    if (index < 0 || index >= static_cast<int>(meta_.size())) return {};
    if (metaLoaded_[static_cast<std::size_t>(index)] == 1)
        return meta_[static_cast<std::size_t>(index)];
    // The cover need not resemble the body. Search only actual resolved
    // sizes; std::set avoids an O(pages^2) layout on large PDFs.
    auto right = resolvedMeta_.lower_bound(index);
    if (right == resolvedMeta_.end()) return meta_[static_cast<std::size_t>(*resolvedMeta_.rbegin())];
    if (right == resolvedMeta_.begin()) return meta_[static_cast<std::size_t>(*right)];
    const int left = *std::prev(right);
    return meta_[static_cast<std::size_t>(index - left <= *right - index ? left : *right)];
}

bool PdfiumWorker::pageMetaLoaded(int index) const {
    std::lock_guard<std::mutex> lk(m_);
    return index >= 0 && index < static_cast<int>(metaLoaded_.size()) &&
           metaLoaded_[static_cast<std::size_t>(index)] == 1;
}

std::vector<PdfiumRasterizer::Bookmark> PdfiumWorker::outline() const {
    std::lock_guard<std::mutex> lk(m_);
    return outline_;
}

bool PdfiumWorker::open(std::vector<std::byte> bytes, std::string* error) {
    std::unique_lock<std::mutex> lk(m_);
    pendingLoad_ = std::move(bytes);
    pendingPath_.clear();
    loadRequested_ = true;
    loadDone_ = false;
    renderQueue_.clear();
    tileQueue_.clear();
    textQueue_.clear();
    annotationsQueue_.clear();
    exportJob_.reset();
    done_.clear();
    lk.unlock();
    cv_.notify_all();

    lk.lock();
    cv_.wait(lk, [this] { return loadDone_ || quit_; });
    if (error) *error = loadError_;
    return loadOk_;
}

bool PdfiumWorker::openFile(std::string path, std::string* error) {
    std::unique_lock<std::mutex> lk(m_);
    pendingLoad_.clear();
    pendingPath_ = std::move(path);
    loadRequested_ = true;
    loadDone_ = false;
    renderQueue_.clear();
    tileQueue_.clear();
    textQueue_.clear();
    annotationsQueue_.clear();
    exportJob_.reset();
    done_.clear();
    lk.unlock();
    cv_.notify_all();
    lk.lock();
    cv_.wait(lk, [this] { return loadDone_ || quit_; });
    if (error) *error = loadError_;
    return loadOk_;
}

void PdfiumWorker::requestMeta(int page) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (page < 0 || page >= static_cast<int>(metaLoaded_.size()) ||
            metaLoaded_[static_cast<std::size_t>(page)]) return;
        metaLoaded_[static_cast<std::size_t>(page)] = 2; // queued or in flight
        Job job;
        job.kind = Kind::Meta;
        job.page = page;
        metaQueue_[page] = std::move(job);
    }
    cv_.notify_all();
}

void PdfiumWorker::requestRender(int page, float scale, int rotationDeg,
                                 int requestedWidth, bool visiblePreview) {
    {
        std::lock_guard<std::mutex> lk(m_);
        const auto k = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(page)) << 32) |
                       static_cast<std::uint32_t>(requestedWidth);
        Job job;
        job.kind = Kind::Render;
        job.page = page;
        job.scale = scale;
        job.rotation = rotationDeg;
        job.requestedWidth = requestedWidth;
        job.visualEpoch = visualEpoch_;
        job.visiblePreview = visiblePreview;
        // Drop obsolete *canvas* scales, never the small thumbnail/preview.
        // The worker cannot interrupt a PDFium call already in progress, but
        // it must not spend seconds rendering all the zoom levels skipped by
        // a fast wheel gesture before reaching the current one.
        if (requestedWidth >= 768) {
            for (auto it = renderQueue_.begin(); it != renderQueue_.end();) {
                if (it->second.page == page && it->second.requestedWidth >= 768 &&
                    it->first != k) it = renderQueue_.erase(it);
                else ++it;
            }
        }
        renderQueue_[k] = std::move(job);
    }
    cv_.notify_all();
}

void PdfiumWorker::requestTile(int page, float scale, int width,
                               const folio::core::TileSpec& spec, int rank) {
    {
        std::lock_guard<std::mutex> lk(m_);
        Job j;
        j.kind = Kind::Tile;
        j.page = page;
        j.scale = scale;
        j.requestedWidth = width;
        j.spec = spec;
        j.rank = rank;
        j.visualEpoch = visualEpoch_;
        const auto id = std::tuple{page, width, spec.tx, spec.ty};
        // A finished but not-yet-drained tile, or one currently being drawn,
        // is already promised to the viewport. Queuing it again doubled
        // PDFium CPU during a slow scroll with frequent animation frames.
        if (activeTile_ == id && activeTileEpoch_ == visualEpoch_) return;
        for (const Result& r : done_)
            if (r.kind == Kind::Tile && r.tile &&
                std::tuple{r.page, r.requestedWidth, r.spec.tx, r.spec.ty} == id)
                return;
        tileQueue_[id] = std::move(j);
    }
    cv_.notify_all();
}

void PdfiumWorker::dropQueuedTiles() {
    std::lock_guard<std::mutex> lk(m_);
    tileQueue_.clear();
}

void PdfiumWorker::retainQueuedTiles(
    const std::unordered_set<std::uint64_t>& wanted) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = tileQueue_.begin(); it != tileQueue_.end();) {
        const auto& [page, width, x, y] = it->first;
        const auto id = (static_cast<std::uint64_t>(static_cast<std::uint16_t>(page)) << 48) |
                        (static_cast<std::uint64_t>(static_cast<std::uint16_t>(width)) << 32) |
                        (static_cast<std::uint64_t>(static_cast<std::uint16_t>(y)) << 16) |
                         static_cast<std::uint16_t>(x);
        it = wanted.count(id) ? std::next(it) : tileQueue_.erase(it);
    }
}

void PdfiumWorker::suspendVisualWork() {
    std::lock_guard<std::mutex> lk(m_);
    ++visualEpoch_;
    tileQueue_.clear();
    renderQueue_.clear();
    textQueue_.clear();
    annotationsQueue_.clear();
    // Export is deliberately left intact: saving a PDF must survive a tab switch.
    for (auto it = done_.begin(); it != done_.end();)
        it = it->kind == Kind::Export ? std::next(it) : done_.erase(it);
}

bool PdfiumWorker::setSuppressed(Suppressed indices) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (suppressed_ == indices) return false;
        suppressed_ = std::move(indices);
        ++suppressionRevision_;
        ++visualEpoch_;
        tileQueue_.clear(); renderQueue_.clear(); annotationsQueue_.clear();
        for (auto it = done_.begin(); it != done_.end();)
            it = it->kind == Kind::Export ? std::next(it) : done_.erase(it);
    }
    cv_.notify_all();
    return true;
}

void PdfiumWorker::requestAnnotations(int page) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (page < 0 || page >= static_cast<int>(meta_.size()) || quit_) return;
        Job job;
        job.kind = Kind::Annotations;
        job.page = page;
        job.visualEpoch = visualEpoch_;
        annotationsQueue_[page] = std::move(job);
    }
    cv_.notify_all();
}

void PdfiumWorker::requestText(int page) {
    {
        std::lock_guard<std::mutex> lk(m_);
        Job job;
        job.kind = Kind::Text;
        job.page = page;
        job.visualEpoch = visualEpoch_;
        textQueue_[page] = std::move(job);
    }
    cv_.notify_all();
}

bool PdfiumWorker::requestExport(std::string source, std::string target,
                                  std::vector<AnnotSpec> marks) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (exporting_ || loadRequested_ || quit_) return false;
        exporting_ = true;
        exportJob_ = std::make_unique<Job>();
        exportJob_->kind = Kind::Export;
        exportJob_->source = std::move(source);
        exportJob_->target = std::move(target);
        exportJob_->marks = std::move(marks);
    }
    cv_.notify_all();
    return true;
}

std::vector<PdfiumWorker::Result> PdfiumWorker::drain() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Result> out;
    out.swap(done_);
    return out;
}

void PdfiumWorker::setWakeCallback(std::function<void()> wake) {
    std::lock_guard<std::mutex> lk(m_);
    wake_ = std::move(wake);
}

bool PdfiumWorker::hasResults() const {
    std::lock_guard<std::mutex> lk(m_);
    return !done_.empty();
}

bool PdfiumWorker::busy() const {
    std::lock_guard<std::mutex> lk(m_);
    return inFlight_ > 0 || !tileQueue_.empty() || !renderQueue_.empty() ||
           !textQueue_.empty() || !annotationsQueue_.empty() || !metaQueue_.empty() ||
           (outlinePending_ && firstRenderDone_) || !done_.empty();
}

void PdfiumWorker::setThumbnailRange(int first, int last) {
    std::lock_guard<std::mutex> lk(m_);
    if (first == thumbnailFirst_ && last == thumbnailLast_) return;
    thumbnailFirst_ = first;
    thumbnailLast_ = last;
    const int focus = focus_.load(std::memory_order_relaxed);
    for (auto it = renderQueue_.begin(); it != renderQueue_.end();) {
        const Job& job = it->second;
        if (!job.visiblePreview && job.requestedWidth <= 256 &&
            (job.page < first || job.page > last) &&
            std::abs(job.page - focus) > 2 &&
            !(gridBufferFirst_ >= 0 && job.page >= gridBufferFirst_ &&
              job.page <= gridBufferLast_))
            it = renderQueue_.erase(it);
        else ++it;
    }
}

void PdfiumWorker::setGridRange(int first, int last, int bufferFirst, int bufferLast) {
    std::lock_guard<std::mutex> lk(m_);
    gridFirst_ = first; gridLast_ = last;
    gridBufferFirst_ = bufferFirst; gridBufferLast_ = bufferLast;
    const int focus = focus_.load(std::memory_order_relaxed);
    for (auto it = renderQueue_.begin(); it != renderQueue_.end();) {
        const Job& j = it->second;
        const bool grid = bufferFirst >= 0 && j.page >= bufferFirst &&
                          j.page <= bufferLast;
        const bool thumb = !j.visiblePreview && j.requestedWidth <= 256 &&
                           j.page >= thumbnailFirst_ && j.page <= thumbnailLast_;
        it = !grid && !thumb && std::abs(j.page - focus) > 2
            ? renderQueue_.erase(it) : std::next(it);
    }
}

void PdfiumWorker::dropQueuedOutside(int page, int radius) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = renderQueue_.begin(); it != renderQueue_.end();) {
        const Job& job = it->second;
        const bool visibleThumb = !job.visiblePreview && job.requestedWidth <= 256 &&
                                  job.page >= thumbnailFirst_ && job.page <= thumbnailLast_;
        const bool grid = gridBufferFirst_ >= 0 &&
                          job.page >= gridBufferFirst_ && job.page <= gridBufferLast_;
        it = std::abs(job.page - page) > radius && !visibleThumb && !grid
            ? renderQueue_.erase(it) : std::next(it);
    }
    for (auto it = tileQueue_.begin(); it != tileQueue_.end();)
        it = std::abs(it->second.page - page) > radius ? tileQueue_.erase(it)
                                                       : std::next(it);
}

/// Pick the outstanding job closest to the page the user is looking at.
/// Render beats text: a visible page that is still blank is a worse failure
/// than a selection that is not ready yet.
bool PdfiumWorker::takeNextJob(Job& out) {
    const int focus = focus_.load(std::memory_order_relaxed);
    auto pickFrom = [&](std::map<int, Job>& q) -> bool {
        if (q.empty()) return false;
        auto best = q.begin();
        int bestDist = std::abs(best->first - focus);
        for (auto it = std::next(q.begin()); it != q.end(); ++it) {
            const int d = std::abs(it->first - focus);
            if (d < bestDist) { best = it; bestDist = d; }
        }
        out = best->second;
        q.erase(best);
        return true;
    };
    if (exportJob_) {
        out = std::move(*exportJob_);
        exportJob_.reset();
        return true;
    }
    // Grid paints EVERY on-screen page at its small device size. The usual
    // +/-2 page focus would starve all but one column of the same viewport.
    if (gridFirst_ >= 0) {
        for (auto it = metaQueue_.lower_bound(gridFirst_);
             it != metaQueue_.end() && it->first <= gridLast_; ++it) {
            out = std::move(it->second);
            metaQueue_.erase(it);
            return true;
        }
        for (auto it = renderQueue_.begin(); it != renderQueue_.end(); ++it) {
            if (it->second.page < gridFirst_ || it->second.page > gridLast_)
                continue;
            out = std::move(it->second);
            renderQueue_.erase(it);
            return true;
        }
    }
    // Later pages initially advertise page-one dimensions. Resolve the
    // focused page's actual geometry before allocating its preview/tiles;
    // it is a single cheap metadata job, not a scan of all PDF pages.
    if (auto it = metaQueue_.find(focus); it != metaQueue_.end()) {
        out = std::move(it->second);
        metaQueue_.erase(it);
        return true;
    }
    // A single bounded, low-resolution image covers the entire visible
    // page before slow high-DPI tiles arrive. Prioritise it once, not every
    // thumbnail or full-page raster (those would starve visible tiles).
    for (auto it = renderQueue_.begin(); it != renderQueue_.end(); ++it) {
        if (it->second.visiblePreview && it->second.page == focus) {
            out = std::move(it->second);
            renderQueue_.erase(it);
            return true;
        }
    }
    // A Pages panel can be scrolled far away from the canvas page. Its
    // visible thumbnails are *also* foreground work; one after two tiles
    // keeps the panel populated without starving the sharp canvas.
    if (tilesSinceThumbnail_ >= 2 || tileQueue_.empty()) {
        auto best = renderQueue_.end();
        for (auto it = renderQueue_.begin(); it != renderQueue_.end(); ++it) {
            if (it->second.visiblePreview || it->second.requestedWidth > 256 ||
                it->second.page < thumbnailFirst_ ||
                it->second.page > thumbnailLast_) continue;
            if (best == renderQueue_.end() ||
                it->second.page < best->second.page) best = it;
        }
        if (best != renderQueue_.end()) {
            out = std::move(best->second);
            renderQueue_.erase(best);
            tilesSinceThumbnail_ = 0;
            return true;
        }
    }
    // Once four sharp foreground tiles have landed, render ONE tiny nearby
    // proxy before resuming tiles. A long high-DPI viewport can need dozens
    // of tiles; without this bounded interleave the next page stays blank
    // throughout a fast fling. The current page's first preview and the
    // Pages panel's visible thumbnails still win above this branch.
    if (tilesSinceNeighbour_ >= 4) {
        auto best = renderQueue_.end();
        for (auto it = renderQueue_.begin(); it != renderQueue_.end(); ++it) {
            const Job& j = it->second;
            if (!j.visiblePreview || j.page == focus ||
                std::abs(j.page - focus) > 2 || j.requestedWidth > 256) continue;
            if (best == renderQueue_.end() ||
                std::abs(j.page - focus) < std::abs(best->second.page - focus))
                best = it;
        }
        if (best != renderQueue_.end()) {
            out = std::move(best->second);
            renderQueue_.erase(best);
            tilesSinceNeighbour_ = 0;
            return true;
        }
    }
    if (!tileQueue_.empty()) {
        auto best = tileQueue_.begin();
        for (auto it = std::next(best); it != tileQueue_.end(); ++it)
            if (it->second.rank < best->second.rank ||
                (it->second.rank == best->second.rank &&
                 std::abs(it->second.page - focus) < std::abs(best->second.page - focus))) best = it;
        out = std::move(best->second);
        tileQueue_.erase(best);
        ++tilesSinceThumbnail_;
        ++tilesSinceNeighbour_;
        return true;
    }
    if (!renderQueue_.empty()) {
        auto best = renderQueue_.begin();
        for (auto it = std::next(best); it != renderQueue_.end(); ++it) {
            const int a = std::abs(it->second.page - focus);
            const int b = std::abs(best->second.page - focus);
            // A visible canvas bitmap outranks an icon-sized thumbnail of
            // the same page; neither can suppress the other's request.
            if (a < b || (a == b && it->second.requestedWidth > best->second.requestedWidth))
                best = it;
        }
        out = std::move(best->second);
        renderQueue_.erase(best);
        return true;
    }
    if (pickFrom(textQueue_)) return true;
    if (pickFrom(annotationsQueue_)) return true;
    if (pickFrom(metaQueue_)) return true;
    if (outlinePending_ && firstRenderDone_) {
        outlinePending_ = false;
        out = Job{};
        out.kind = Kind::Outline;
        return true;
    }
    return false;
}

void PdfiumWorker::threadMain() {
    ensurePdfiumInitialised();

    // Constructed here, so this is the thread it binds to. Nothing outside
    // this function may touch it.
    PdfiumRasterizer rast;
    rast.bindToCurrentThread();
    std::string openedPath;
    std::uint64_t appliedSuppression = 0;
    std::unordered_set<int> filteredPages;

    for (;;) {
        Job job;
        bool haveJob = false;
        std::vector<std::byte> toLoad;
        std::string pathToLoad;
        bool doLoad = false;

        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] {
                return quit_ || loadRequested_ || !tileQueue_.empty() || !renderQueue_.empty() ||
                       !textQueue_.empty() || !annotationsQueue_.empty() || !metaQueue_.empty() ||
                       (outlinePending_ && firstRenderDone_) || exportJob_ != nullptr;
            });
            if (quit_) return;

            if (loadRequested_) {
                loadRequested_ = false;
                toLoad.swap(pendingLoad_);
                pathToLoad.swap(pendingPath_);
                doLoad = true;
            } else {
                haveJob = takeNextJob(job);
                if (haveJob) {
                    ++inFlight_;
                    if (job.kind == Kind::Tile) {
                        activeTile_ = std::tuple{job.page, job.requestedWidth,
                                                 job.spec.tx, job.spec.ty};
                        activeTileEpoch_ = job.visualEpoch;
                    }
                    foregroundInFlight_ = job.kind != Kind::Meta &&
                                          job.kind != Kind::Outline;
                }
            }
        }

        if (doLoad) {
            std::lock_guard<std::recursive_mutex> apiLock(pdfiumApiMutex());
            folio::core::SnapshotPtr snapshot;
            bool ok = false;
            if (!pathToLoad.empty()) {
                openedPath = pathToLoad;
                ok = rast.openFile(pathToLoad);
                appliedSuppression = 0;
                filteredPages.clear();
            } else {
                snapshot = folio::core::DocumentSnapshot::Builder::fromBase(
                    std::move(toLoad));
                ok = rast.setSnapshot(snapshot);
            }

            std::vector<PageMeta> meta;
            std::vector<PdfiumRasterizer::Bookmark> toc;
            std::string err;
            if (ok) {
                // Path opens return after page one: scanning every page and
                // walking the full bookmark tree here delays first paint by
                // minutes on large files. A memory snapshot keeps its legacy
                // eager metadata contract for engine clients.
                const auto n = static_cast<int>(rast.pageCount());
                if (n <= 0) {
                    err = "document has no pages";
                } else if (!pathToLoad.empty()) {
                    const auto first = rast.pageInfo(folio::core::PageId{0});
                    meta.assign(static_cast<std::size_t>(n),
                                PageMeta{first.widthPt, first.heightPt, first.rotation});
                } else {
                    toc = rast.outline();
                    meta.reserve(static_cast<std::size_t>(n));
                    for (int i = 0; i < n; ++i) {
                        const auto info = rast.pageInfo(
                            folio::core::PageId{static_cast<std::uint32_t>(i)});
                        meta.push_back(
                            PageMeta{info.widthPt, info.heightPt, info.rotation});
                    }
                }
            } else {
                err = "PDFium could not parse this file";
            }

            rast.releasePages(); // even page-one metadata has a temporary handle
            std::lock_guard<std::mutex> lk(m_);
            snapshot_ = snapshot;
            meta_ = std::move(meta);
            metaLoaded_.assign(meta_.size(), pathToLoad.empty() ? 1 : 0);
            resolvedMeta_.clear();
            if (!metaLoaded_.empty()) metaLoaded_[0] = 1;
            if (pathToLoad.empty()) {
                for (int i = 0; i < static_cast<int>(meta_.size()); ++i)
                    resolvedMeta_.insert(i);
            } else if (!meta_.empty()) resolvedMeta_.insert(0);
            metaQueue_.clear();
            annotationsQueue_.clear();
            outline_ = std::move(toc);
            outlinePending_ = !pathToLoad.empty() && !meta_.empty();
            firstRenderDone_ = false;
            geometryRevision_.store(0, std::memory_order_release);
            outlineRevision_.store(0, std::memory_order_release);
            loadOk_ = ok && !meta_.empty();
            loadError_ = err;
            loadDone_ = true;
            cv_.notify_all();
            continue;
        }

        if (!haveJob) continue;

        std::unique_lock<std::recursive_mutex> apiLock(pdfiumApiMutex());
        Result res;
        res.kind = job.kind;
        res.page = job.page;

        // Only this worker touches the PDFium document. Changing the Folio
        // overlay set reopens a fresh view document so undo cannot leave a
        // previously hidden imported object permanently hidden in memory.
        Suppressed active;
        std::uint64_t wantedRevision = 0;
        { std::lock_guard<std::mutex> lk(m_);
          active = suppressed_;
          wantedRevision = suppressionRevision_;
        }
        if (wantedRevision != appliedSuppression && !openedPath.empty()) {
            rast.openFile(openedPath);
            filteredPages.clear();
            appliedSuppression = wantedRevision;
        }
        if (job.kind != Kind::Export && job.kind != Kind::Outline &&
            !filteredPages.count(job.page)) {
            if (auto found = active.find(job.page); found != active.end()) {
                if (FPDF_PAGE page = rast.borrowPage(
                    folio::core::PageId{static_cast<std::uint32_t>(job.page)})) {
                    auto refs = found->second;
                    std::sort(refs.begin(), refs.end(), std::greater<>{});
                    for (const auto& [index, type] : refs) {
                        if (index < 0 || index >= FPDFPage_GetAnnotCount(page)) continue;
                        FPDF_ANNOTATION mark = FPDFPage_GetAnnot(page, index);
                        const bool same = mark && static_cast<int>(FPDFAnnot_GetSubtype(mark)) == type;
                        if (mark) FPDFPage_CloseAnnot(mark);
                        if (same) FPDFPage_RemoveAnnot(page, index);
                    }
                }
            }
            filteredPages.insert(job.page);
        }

        if (job.kind == Kind::Export) {
            if (writePdfiumAnnotations(job.source, job.target, job.marks, res.error))
                res.exportPath = std::move(job.target);
        } else if (job.kind == Kind::Render) {
            res.requestedWidth = job.requestedWidth;
            float renderScale = job.scale;
            if (job.visiblePreview) {
                // Other pages temporarily inherit page-one geometry while
                // their own metadata loads. Bound preview allocation using
                // the REAL page dimensions on the PDFium thread, not that
                // placeholder; otherwise a wide page can allocate a huge
                // supposed '512px' preview.
                const auto info = rast.pageInfo(
                    folio::core::PageId{static_cast<std::uint32_t>(job.page)});
                if (info.widthPt > 0.f && info.heightPt > 0.f) {
                    const bool neighbour = job.page !=
                        focus_.load(std::memory_order_relaxed) &&
                        job.requestedWidth <= 256;
                    renderScale = std::min((neighbour ? 240.f : 512.f) / info.widthPt,
                                           (neighbour ? 320.f : 768.f) / info.heightPt);
                }
            }
            res.scale = renderScale;
            auto tile = rast.renderFullPage(
                folio::core::PageId{static_cast<std::uint32_t>(job.page)},
                                            renderScale, job.rotation,
                                            folio::core::ColorScheme::Light);
            if (tile) {
                res.pxW = tile->width;
                res.pxH = tile->height;
                res.tile = std::move(tile);
            }
        } else if (job.kind == Kind::Tile) {
            res.scale = job.scale;
            res.requestedWidth = job.requestedWidth;
            res.spec = job.spec;
            folio::core::TileRequest req;
            req.page = static_cast<folio::core::PageId>(job.page);
            req.tileX = job.spec.tx;
            req.tileY = job.spec.ty;
            req.scale = job.scale;
            req.priority = folio::core::Priority::VisibleTile;
            req.snapshotGen = rast.generation();
            res.tile = rast.renderTile(req, job.spec);
            if (res.tile) {
                res.pxW = res.tile->width;
                res.pxH = res.tile->height;
            }
        } else if (job.kind == Kind::Meta) {
            const auto info = rast.pageInfo(
                folio::core::PageId{static_cast<std::uint32_t>(job.page)});
            {
                std::lock_guard<std::mutex> lk(m_);
                if (job.page >= 0 && job.page < static_cast<int>(meta_.size())) {
                    meta_[static_cast<std::size_t>(job.page)] =
                        PageMeta{info.widthPt, info.heightPt, info.rotation};
                    metaLoaded_[static_cast<std::size_t>(job.page)] = 1;
                    resolvedMeta_.insert(job.page);
                    geometryRevision_.fetch_add(1, std::memory_order_release);
                }
            }
        } else if (job.kind == Kind::Annotations) {
            if (FPDF_PAGE page = rast.borrowPage(
                    folio::core::PageId{static_cast<std::uint32_t>(job.page)})) {
                // Cap hostile documents. We never build a duplicate painted
                // overlay; PDFium already renders the original annotation.
                const int count = std::clamp(FPDFPage_GetAnnotCount(page), 0, 5000);
                for (int i = 0; i < count; ++i) {
                    FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
                    if (!a) continue;
                    const int type = FPDFAnnot_GetSubtype(a);
                    const auto bytes = FPDFAnnot_GetStringValue(a, "Contents", nullptr, 0);
                    std::string contents;
                    if (bytes > 2 && bytes <= 8192 && bytes % 2 == 0) {
                        std::vector<FPDF_WCHAR> chars(bytes / 2);
                        if (FPDFAnnot_GetStringValue(a, "Contents", chars.data(), bytes) == bytes) {
                            for (std::size_t k = 0; k + 1 < chars.size(); ++k) {
                                std::uint32_t cp = chars[k];
                                if (cp >= 0xD800 && cp <= 0xDBFF && k + 2 < chars.size())
                                    cp = 0x10000 + ((cp - 0xD800) << 10) +
                                         (static_cast<std::uint32_t>(chars[++k]) - 0xDC00);
                                if (cp < 0x80) contents.push_back(static_cast<char>(cp));
                                else if (cp < 0x800) {
                                    contents.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                                    contents.push_back(static_cast<char>(0x80 | (cp & 63)));
                                } else if (cp < 0x10000) {
                                    contents.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                                    contents.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
                                    contents.push_back(static_cast<char>(0x80 | (cp & 63)));
                                } else if (cp <= 0x10FFFF) {
                                    contents.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                                    contents.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
                                    contents.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
                                    contents.push_back(static_cast<char>(0x80 | (cp & 63)));
                                }
                            }
                        }
                    }
                    PdfAnnotationInfo info;
                    info.index = i;
                    // The view hides converted originals by deleting only
                    // from its in-memory /Annots array. Map remaining indices
                    // back to the untouched source array for safe Save As.
                    if (auto hidden = active.find(job.page); hidden != active.end()) {
                        auto refs = hidden->second;
                        std::sort(refs.begin(), refs.end());
                        for (const auto& [sourceIndex, sourceType] : refs) {
                            (void)sourceType;
                            if (sourceIndex <= info.index) ++info.index;
                        }
                    }
                    info.subtype = type;
                    info.fingerprint = pdfAnnotationFingerprint(a);
                    info.contents = std::move(contents);
                    info.editable = type == FPDF_ANNOT_HIGHLIGHT || type == FPDF_ANNOT_UNDERLINE ||
                        type == FPDF_ANNOT_STRIKEOUT || type == FPDF_ANNOT_TEXT ||
                        type == FPDF_ANNOT_SQUARE || type == FPDF_ANNOT_CIRCLE;
                    if (!info.fingerprint) info.editable = false;
                    if ((type == FPDF_ANNOT_HIGHLIGHT || type == FPDF_ANNOT_UNDERLINE ||
                         type == FPDF_ANNOT_STRIKEOUT) &&
                        FPDFAnnot_CountAttachmentPoints(a) != 1)
                        info.editable = false; // multi-line/quads must not flatten into a box
                    FS_RECTF rect{};
                    if (FPDFAnnot_GetRect(a, &rect)) {
                        const int w = std::max(1, static_cast<int>(std::lround(FPDF_GetPageWidthF(page) * 100)));
                        const int h = std::max(1, static_cast<int>(std::lround(FPDF_GetPageHeightF(page) * 100)));
                        const auto device = [&](float x, float y) {
                            int dx = 0, dy = 0;
                            FPDF_PageToDevice(page, 0, 0, w, h, 0, x, y, &dx, &dy);
                            return std::pair{std::clamp(static_cast<float>(dx) / w, 0.f, 1.f),
                                             std::clamp(static_cast<float>(dy) / h, 0.f, 1.f)};
                        };
                        const auto a0 = device(rect.left, rect.bottom);
                        const auto b0 = device(rect.right, rect.top);
                        info.l = std::min(a0.first, b0.first); info.r = std::max(a0.first, b0.first);
                        info.t = std::min(a0.second, b0.second); info.b = std::max(a0.second, b0.second);
                        if (info.editable && (type == FPDF_ANNOT_HIGHLIGHT ||
                                              type == FPDF_ANNOT_UNDERLINE ||
                                              type == FPDF_ANNOT_STRIKEOUT)) {
                            FS_QUADPOINTSF q{};
                            if (!FPDFAnnot_GetAttachmentPoints(a, 0, &q)) info.editable = false;
                            else {
                                const auto p1 = device(q.x1, q.y1), p2 = device(q.x2, q.y2);
                                const auto p3 = device(q.x3, q.y3), p4 = device(q.x4, q.y4);
                                const float xl = std::min({p1.first,p2.first,p3.first,p4.first});
                                const float xr = std::max({p1.first,p2.first,p3.first,p4.first});
                                const float yt = std::min({p1.second,p2.second,p3.second,p4.second});
                                const float yb = std::max({p1.second,p2.second,p3.second,p4.second});
                                for (const auto& v : {p1,p2,p3,p4}) {
                                    if (!(std::fabs(v.first-xl) < .005f ||
                                          std::fabs(v.first-xr) < .005f) ||
                                        !(std::fabs(v.second-yt) < .005f ||
                                          std::fabs(v.second-yb) < .005f)) info.editable = false;
                                }
                                if (info.editable) { info.l=xl; info.r=xr; info.t=yt; info.b=yb; }
                            }
                        }
                    }
                    unsigned red = 255, green = 214, blue = 10, alpha = 255;
                    if (FPDFAnnot_GetColor(a, FPDFANNOT_COLORTYPE_Color,
                                           &red, &green, &blue, &alpha)) {
                        info.red = static_cast<std::uint8_t>(red);
                        info.green = static_cast<std::uint8_t>(green);
                        info.blue = static_cast<std::uint8_t>(blue);
                        info.alpha = static_cast<std::uint8_t>(alpha);
                    }
                    res.annotations.push_back(std::move(info));
                    FPDFPage_CloseAnnot(a);
                }
            }
        } else if (job.kind == Kind::Outline) {
            auto toc = rast.outline();
            {
                std::lock_guard<std::mutex> lk(m_);
                outline_ = std::move(toc);
                outlineRevision_.fetch_add(1, std::memory_order_release);
            }
        } else {
            auto text = std::make_shared<PageText>();
            if (FPDF_PAGE page = rast.borrowPage(
                    folio::core::PageId{static_cast<std::uint32_t>(job.page)})) {
                if (FPDF_TEXTPAGE tp = FPDFText_LoadPage(page)) {
                    const int n = FPDFText_CountChars(tp);
                    // The metadata queue can lag a first-page text request.
                    // Never normalise with its provisional 612x792 size.
                    const auto info = rast.pageInfo(
                        folio::core::PageId{static_cast<std::uint32_t>(job.page)});
                    const TextPageMap map(page, info.widthPt, info.heightPt);

                    text->glyphs.reserve(static_cast<std::size_t>(std::max(0, n)));
                    text->folded.reserve(static_cast<std::size_t>(std::max(0, n)));
                    for (int i = 0; i < n; ++i) {
                        const auto u =
                            static_cast<char32_t>(FPDFText_GetUnicode(tp, i));
                        double l = 0, r = 0, b = 0, t = 0;
                        if (!FPDFText_GetCharBox(tp, i, &l, &r, &b, &t)) {
                            // PDFium synthesises spaces/newlines without a
                            // drawable character box. Dropping them glues
                            // separate words and paragraphs into one run;
                            // mobile long-press can then select the ENTIRE
                            // passage instead of the touched word. Keep
                            // separators as zero-area text glyphs. They
                            // bound word selection, but never paint or hit.
                            if (u == U' ' || u == U'\t' || u == U'\r' ||
                                u == U'\n' || u == 0xA0 || u == 0x3000) {
                                text->glyphs.push_back(TextGlyph{u, 0.f, 0.f,
                                                                0.f, 0.f});
                                text->folded.push_back(fold(u));
                            }
                            continue;
                        }

                        TextGlyph g;
                        g.code = u;
                        map.rect(l, r, b, t, g);

                        text->glyphs.push_back(g);
                        text->folded.push_back(fold(u));
                    }
                    FPDFText_ClosePage(tp);
                }
            }
            res.text = std::move(text);
        }

        // Nothing parsed survives this job. The open FPDF_DOCUMENT/mapping
        // remains ready in its tab, while FPDF_PAGE is closed after use.
        rast.releasePages();
        apiLock.unlock();
        std::function<void()> wake;
        {
            std::lock_guard<std::mutex> lk(m_);
            wake = wake_;
            if (res.kind == Kind::Export) exporting_ = false;
            if (res.kind == Kind::Tile) activeTile_.reset();
            if (res.kind == Kind::Render || res.kind == Kind::Tile) firstRenderDone_ = true;
            if (res.kind != Kind::Meta && res.kind != Kind::Outline &&
                (res.kind == Kind::Export || job.visualEpoch == visualEpoch_))
                done_.push_back(std::move(res));
            foregroundInFlight_ = false;
            --inFlight_;
        }
        if (wake) wake(); // platform wake() is thread-safe; never call UI methods here
    }
}

}  // namespace folio::engine
