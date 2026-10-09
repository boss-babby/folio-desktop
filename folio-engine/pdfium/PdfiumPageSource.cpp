// folio-engine/pdfium/PdfiumPageSource.cpp

#include "pdfium/PdfiumPageSource.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

namespace folio::engine {

namespace core = folio::core;
using folio::render::IPaintTarget;
using folio::render::Rect;

namespace {
/// Above this the bitmap costs more than it is worth and the blit upscales
/// instead. 4096 on the long edge is one 64 MB page at BGRA.
constexpr int kMaxLongEdge = 4096;
/// Resident page bitmaps. The stage keeps its own raster cache above this
/// one; this budget only covers PDFium output.
constexpr std::size_t kByteBudget = 192u * 1024u * 1024u;
}  // namespace

PdfiumPageSource::PdfiumPageSource() : worker_(std::make_unique<PdfiumWorker>()) {}
PdfiumPageSource::~PdfiumPageSource() = default;

bool PdfiumPageSource::open(const std::string& path) {
    cache_.clear();
    requested_.clear();
    text_.clear();
    textRequested_.clear();
    uiGlyphs_.clear();
    bytes_ = 0;
    stats_ = CacheStats{};
    pageCount_ = 0;
    error_.clear();
    static std::uint64_t s_salt = 0;
    salt_ = ++s_salt;
    staleKeys_.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error_ = "cannot open " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto len = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> fileBytes(len);
    in.read(reinterpret_cast<char*>(fileBytes.data()),
            static_cast<std::streamsize>(len));
    if (!in && in.gcount() != static_cast<std::streamsize>(len)) {
        error_ = "short read";
        return false;
    }
    if (fileBytes.empty()) {
        error_ = "empty file";
        return false;
    }

    std::string err;
    if (!worker_->open(std::move(fileBytes), &err)) {
        error_ = err.empty() ? "PDFium refused the file" : err;
        return false;
    }

    pageCount_ = worker_->pageCount();
    outline_.clear();
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

bool PdfiumPageSource::tick() {
    auto results = worker_->drain();
    if (results.empty()) return false;

    bool changed = false;
    for (auto& r : results) {
        if (r.kind == PdfiumWorker::Kind::Text) {
            text_[r.page] = std::move(r.text);
            changed = true;
            continue;
        }
        if (!r.tile) continue;
        const std::size_t sz = r.tile->pixels.size();
        Rendered entry;
        entry.pxW = r.pxW;
        entry.pxH = r.pxH;
        entry.lastUsed = ++clock_;
        entry.tile = std::move(r.tile);

        const auto k = key(r.page, r.pxW);
        if (auto it = cache_.find(k); it != cache_.end()) {
            bytes_ -= it->second.tile ? it->second.tile->pixels.size() : 0;
            staleKeys_.push_back(imageKey(r.page, r.pxW));
        }
        cache_[k] = std::move(entry);
        bytes_ += sz;
        changed = true;
    }
    evictTo(kByteBudget);
    stats_.pages = cache_.size();
    stats_.bytes = bytes_;
    return changed;
}

bool PdfiumPageSource::pending() const { return worker_->busy(); }

void PdfiumPageSource::setFocusPage(int page) {
    if (page >= 0 && page < pageCount_) worker_->setFocusPage(page);
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

    const int pxW = std::max(1, static_cast<int>(std::lround(m.widthPt * eff)));

    // 1. exact size - the normal path.
    if (auto it = cache_.find(key(index, pxW)); it != cache_.end() && it->second.tile) {
        it->second.lastUsed = ++clock_;
        ++stats_.hits;
        const auto& tile = *it->second.tile;
        t.drawImageBGRA(tile.pixels.data(), tile.width, tile.height, tile.stride,
                        dest, imageKey(index, pxW),
                        IPaintTarget::ImageFit::Final, tint);
        return;
    }

    ++stats_.misses;
    if (requested_[index] != pxW) {
        requested_[index] = pxW;
        worker_->requestRender(index, eff, kExtraRotation);
    }

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
        ++stats_.proxies;
        const auto& tile = *best->second.tile;
        t.drawImageBGRA(tile.pixels.data(), tile.width, tile.height, tile.stride,
                        dest, imageKey(index, best->second.pxW),
                        IPaintTarget::ImageFit::Transient, tint);
        return;
    }

    // 3. nothing yet - paper.
    t.fillRect(dest, folio::render::Color{0xFF, 0xFF, 0xFF, 0xFF});
}

void PdfiumPageSource::evictTo(std::size_t budget) {
    while (bytes_ > budget && cache_.size() > 1) {
        auto oldest = cache_.begin();
        for (auto it = std::next(cache_.begin()); it != cache_.end(); ++it)
            if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
        bytes_ -= oldest->second.tile ? oldest->second.tile->pixels.size() : 0;
        staleKeys_.push_back(imageKey(static_cast<int>(oldest->first >> 32),
                                      oldest->second.pxW));
        cache_.erase(oldest);
    }
}

void PdfiumPageSource::trimTo(int page, int radius) {
    for (auto it = cache_.begin(); it != cache_.end();) {
        const int p = static_cast<int>(it->first >> 32);
        if (std::abs(p - page) > radius) {
            bytes_ -= it->second.tile ? it->second.tile->pixels.size() : 0;
            staleKeys_.push_back(imageKey(p, it->second.pxW));
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
    stats_.pages = cache_.size();
    stats_.bytes = bytes_;
}

}  // namespace folio::engine
