#include "pdfium/PdfiumWorker.h"

#include "pdfium/PdfiumLibrary.h"

#include <fpdf_text.h>

#include <algorithm>
#include <cmath>
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
    if (index < 0 || index >= static_cast<int>(meta_.size())) return {};
    return meta_[static_cast<std::size_t>(index)];
}

std::vector<PdfiumRasterizer::Bookmark> PdfiumWorker::outline() const {
    std::lock_guard<std::mutex> lk(m_);
    return outline_;
}

bool PdfiumWorker::open(std::vector<std::byte> bytes, std::string* error) {
    std::unique_lock<std::mutex> lk(m_);
    pendingLoad_ = std::move(bytes);
    loadRequested_ = true;
    loadDone_ = false;
    renderQueue_.clear();
    textQueue_.clear();
    done_.clear();
    lk.unlock();
    cv_.notify_all();

    lk.lock();
    cv_.wait(lk, [this] { return loadDone_ || quit_; });
    if (error) *error = loadError_;
    return loadOk_;
}

void PdfiumWorker::requestRender(int page, float scale, int rotationDeg) {
    {
        std::lock_guard<std::mutex> lk(m_);
        renderQueue_[page] = Job{Kind::Render, page, scale, rotationDeg};
    }
    cv_.notify_all();
}

void PdfiumWorker::requestText(int page) {
    {
        std::lock_guard<std::mutex> lk(m_);
        textQueue_[page] = Job{Kind::Text, page, 1.0f, 0};
    }
    cv_.notify_all();
}

std::vector<PdfiumWorker::Result> PdfiumWorker::drain() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Result> out;
    out.swap(done_);
    return out;
}

bool PdfiumWorker::busy() const {
    std::lock_guard<std::mutex> lk(m_);
    return inFlight_ > 0 || !renderQueue_.empty() || !textQueue_.empty() ||
           !done_.empty();
}

void PdfiumWorker::dropQueuedOutside(int page, int radius) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = renderQueue_.begin(); it != renderQueue_.end();)
        it = std::abs(it->first - page) > radius ? renderQueue_.erase(it)
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
    if (pickFrom(renderQueue_)) return true;
    return pickFrom(textQueue_);
}

void PdfiumWorker::threadMain() {
    ensurePdfiumInitialised();

    // Constructed here, so this is the thread it binds to. Nothing outside
    // this function may touch it.
    PdfiumRasterizer rast;
    rast.bindToCurrentThread();

    for (;;) {
        Job job;
        bool haveJob = false;
        std::vector<std::byte> toLoad;
        bool doLoad = false;

        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] {
                return quit_ || loadRequested_ || !renderQueue_.empty() ||
                       !textQueue_.empty();
            });
            if (quit_) return;

            if (loadRequested_) {
                loadRequested_ = false;
                toLoad.swap(pendingLoad_);
                doLoad = true;
            } else {
                haveJob = takeNextJob(job);
                if (haveJob) ++inFlight_;
            }
        }

        if (doLoad) {
            auto snapshot = folio::core::DocumentSnapshot::Builder::fromBase(
                std::move(toLoad));
            const bool ok = rast.setSnapshot(snapshot);

            std::vector<PageMeta> meta;
            std::vector<PdfiumRasterizer::Bookmark> toc;
            std::string err;
            if (ok) {
                // Read on this thread, where the FPDF_DOCUMENT lives.
                toc = rast.outline();
                const int n = rast.pageCount();
                meta.reserve(static_cast<std::size_t>(std::max(0, n)));
                for (int i = 0; i < n; ++i) {
                    const auto info = rast.pageInfo(folio::core::PageId{i});
                    meta.push_back(
                        PageMeta{info.widthPt, info.heightPt, info.rotation});
                }
                if (meta.empty()) err = "document has no pages";
            } else {
                err = "PDFium could not parse this file";
            }

            std::lock_guard<std::mutex> lk(m_);
            snapshot_ = snapshot;
            meta_ = std::move(meta);
            outline_ = std::move(toc);
            loadOk_ = ok && !meta_.empty();
            loadError_ = err;
            loadDone_ = true;
            cv_.notify_all();
            continue;
        }

        if (!haveJob) continue;

        Result res;
        res.kind = job.kind;
        res.page = job.page;

        if (job.kind == Kind::Render) {
            res.scale = job.scale;
            auto tile = rast.renderFullPage(folio::core::PageId{job.page},
                                            job.scale, job.rotation,
                                            folio::core::ColorScheme::Light);
            if (tile) {
                res.pxW = tile->width;
                res.pxH = tile->height;
                res.tile = std::move(tile);
            }
        } else {
            auto text = std::make_shared<PageText>();
            if (FPDF_PAGE page = rast.borrowPage(folio::core::PageId{job.page})) {
                if (FPDF_TEXTPAGE tp = FPDFText_LoadPage(page)) {
                    const int n = FPDFText_CountChars(tp);
                    const auto m = pageMeta(job.page);
                    const double wPt = m.widthPt > 0 ? m.widthPt : 1.0;
                    const double hPt = m.heightPt > 0 ? m.heightPt : 1.0;

                    text->glyphs.reserve(static_cast<std::size_t>(std::max(0, n)));
                    text->folded.reserve(static_cast<std::size_t>(std::max(0, n)));
                    for (int i = 0; i < n; ++i) {
                        double l = 0, r = 0, b = 0, t = 0;
                        if (!FPDFText_GetCharBox(tp, i, &l, &r, &b, &t)) continue;
                        const auto u =
                            static_cast<char32_t>(FPDFText_GetUnicode(tp, i));

                        TextGlyph g;
                        g.code = u;
                        // PDF user space is bottom-left origin; the UI is
                        // top-left. Flip y and normalise both axes.
                        g.l = static_cast<float>(l / wPt);
                        g.r = static_cast<float>(r / wPt);
                        g.t = static_cast<float>(1.0 - t / hPt);
                        g.b = static_cast<float>(1.0 - b / hPt);
                        if (g.r < g.l) std::swap(g.l, g.r);
                        if (g.b < g.t) std::swap(g.t, g.b);

                        text->glyphs.push_back(g);
                        text->folded.push_back(fold(u));
                    }
                    FPDFText_ClosePage(tp);
                }
            }
            res.text = std::move(text);
        }

        {
            std::lock_guard<std::mutex> lk(m_);
            done_.push_back(std::move(res));
            --inFlight_;
        }
    }
}

}  // namespace folio::engine
