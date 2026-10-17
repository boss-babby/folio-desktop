#include "components/PageStage.h"

#include "folio/ui/Icons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace folio::ui {
namespace {

/// The prototype's TT array: one heading per page.
constexpr const char* kTitles[] = {
    "Annual Report 2026", "Executive summary", "Quarterly Overview", "Highlights",
    "Revenue by region", "Regional trends", "Customer growth", "Cost structure",
    "Operating costs", "Headcount", "Segment results", "Consumer", "Enterprise",
    "Partners", "Pricing", "Retention", "Outlook", "Risks", "Priorities",
    "Roadmap", "Appendix A", "Appendix B", "Glossary", "Contacts"};
constexpr std::size_t kTitleCount = sizeof(kTitles) / sizeof(kTitles[0]);

/// The prototype's PP array.
constexpr const char* kParas[] = {
    "Revenue grew 12% over the previous quarter, led by enterprise subscriptions "
    "and a stronger renewal rate across all regions.",
    "Operating costs held steady while the team expanded, and the margin improved "
    "by two points compared with the same period last year.",
    "Customer growth was strongest in the second half of the quarter, helped by "
    "the new onboarding flow and two partner launches.",
    "The outlook remains positive. We expect continued growth, with investment "
    "focused on product quality and customer support."};
constexpr std::size_t kParaCount = sizeof(kParas) / sizeof(kParas[0]);

}  // namespace

namespace {

constexpr float kPageGap = 20.f;
constexpr float kGridGap = 18.f;
constexpr float kMargin = 28.f;
constexpr float kMinZoom = 0.25f;
constexpr float kMaxZoom = 6.f;
constexpr float kFlingFriction = 4.6f; // exponential velocity decay, not a forced slow glide
constexpr float kFlingCutoff = 10.f;
constexpr float kOverscrollLimit = 64.f; // logical dp; never fling through a page boundary
constexpr float kSelectionGripOffset = 6.f; // outwards, while hit radius stays 22dp

// Preserve the existing 2.6%-page-height default marker at the toolbar's
// default 1.2pt width, while exposing six genuinely different, controlled
// thicknesses. A linear 10/1.2 ratio would swallow whole lines of text.
float highlightBandHeight(float width) {
    return std::clamp(.026f * std::sqrt(std::max(.1f, width) / 1.2f),
                      .01f, .09f);
}

Color paperOf(PageTheme t) {
    switch (t) {
        case PageTheme::Sepia: return tokens::pageTheme_sepia.paper;
        case PageTheme::Dark:  return tokens::pageTheme_dark.paper;
        case PageTheme::Light: break;
    }
    return tokens::pageTheme_light.paper;
}
Color inkOf(PageTheme t) {
    switch (t) {
        case PageTheme::Sepia: return tokens::pageTheme_sepia.ink;
        case PageTheme::Dark:  return tokens::pageTheme_dark.ink;
        case PageTheme::Light: break;
    }
    return tokens::pageTheme_light.ink;
}

struct SelectionInk {
    Color top, bottom, edge, halo, handle;
};

// Warm reading-paper selection, not the generic saturated blue OS overlay.
// The sepia palette follows the reference; light and dark retain its gentle
// warmth while changing contrast for the actual paper beneath it.
SelectionInk selectionInk(PageTheme t) {
    switch (t) {
        case PageTheme::Sepia:
            return {{226, 149, 65, 48}, {211, 119, 50, 69},
                    {163, 92, 48, 137}, {247, 226, 188, 226},
                    {168, 102, 51, 255}};
        case PageTheme::Dark:
            return {{244, 175, 93, 52}, {221, 142, 73, 70},
                    {245, 186, 116, 161}, {55, 41, 30, 230},
                    {255, 190, 110, 255}};
        case PageTheme::Light:
        default:
            return {{247, 173, 90, 51}, {224, 131, 59, 65},
                    {174, 95, 43, 138}, {255, 238, 208, 229},
                    {175, 100, 45, 255}};
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// PlaceholderPageSource
// ---------------------------------------------------------------------------

void PlaceholderPageSource::drawPage(IPaintTarget& t, int index, const Rect& dest,
                                     float scale, PageTheme theme) {
    // The prototype's page is a real document, not a grey mock: a heading,
    // a paragraph, a four-bar chart in the document's accent colour, a
    // second paragraph and a page number in the footer. All of its metrics
    // are container-query units (cqw) - percentages of the page width - so
    // the page scales as one piece at any zoom. The constants below are
    // those cqw values divided by 100.
    const Color ink = inkOf(theme);
    const float W = dest.w;
    const float pad = W * 0.093f;                  // .in padding: 9.3cqw
    const float gap = W * 0.023f;                  // .in gap: 2.3cqw
    const float colW = W - pad * 2.f;
    float y = dest.y + pad;

    // --- heading: .blk.h, 4.64cqw, weight 700 ------------------------------
    const std::string title = kTitles[static_cast<std::size_t>(index) % kTitleCount];
    const float hSize = W * 0.0464f;
    if (scale > 0.08f) {
        t.drawTextInBox(title, Rect{dest.x + pad, y, colW, hSize * 1.2f},
                        folio::render::TextStyle{hSize, folio::render::FontWeight::Bold, 0.f, ink},
                        folio::render::TextAlign::Left);
    } else {
        t.fillRect(Rect{dest.x + pad, y + hSize * 0.25f, colW * 0.56f, hSize * 0.55f},
                   fade(ink, 0.8f));
    }
    y += hSize * 1.2f + gap;

    // --- body paragraph: .blk, 1.9cqw, line-height 1.55 --------------------
    const float bSize = W * 0.019f;
    const float lead = bSize * 1.55f;
    auto paragraph = [&](const std::string& text) {
        if (scale > 0.12f) {
            // drawTextInBox does not wrap, so the paragraph is split on
            // word boundaries at the measured column width.
            std::size_t start = 0;
            while (start < text.size()) {
                std::size_t best = start, i = start;
                while (i < text.size()) {
                    const std::size_t sp = text.find(' ', i + 1);
                    const std::size_t end = (sp == std::string::npos) ? text.size() : sp;
                    if (t.measureText(text.substr(start, end - start),
                                      folio::render::TextStyle{bSize, folio::render::FontWeight::Regular, 0.f, ink}) > colW)
                        break;
                    best = end;
                    i = end;
                }
                if (best == start) best = text.size();
                t.drawTextInBox(text.substr(start, best - start),
                                Rect{dest.x + pad, y, colW, lead},
                                folio::render::TextStyle{bSize, folio::render::FontWeight::Regular, 0.f, ink},
                                folio::render::TextAlign::Left);
                y += lead;
                start = (best < text.size() && text[best] == ' ') ? best + 1 : best;
            }
        } else {
            for (int i = 0; i < 3; ++i) {
                t.fillRect(Rect{dest.x + pad, y + lead * 0.3f, colW * (i == 2 ? 0.6f : 1.f),
                                std::max(0.75f, bSize * 0.34f)},
                           fade(ink, 0.46f));
                y += lead;
            }
        }
    };
    paragraph(kParas[static_cast<std::size_t>(index) % kParaCount]);
    y += gap;

    // --- chart: .ch, height 20cqw, four bars in the document accent --------
    const float chH = W * 0.20f;
    if (y + chH < dest.bottom() - pad) {
        const Rect ch{dest.x + pad, y, colW, chH};
        t.fillRoundedRect(ch, Corners::all(W * 0.014f),
                          theme == PageTheme::Dark ? fade(ink, 0.10f)
                                                   : Color{0xEC, 0xEB, 0xFB, 0xFF});
        const float barW = W * 0.08f;
        const float barGap = W * 0.03f;
        const float inner = W * 0.04f;
        for (int i = 0; i < 4; ++i) {
            // Deterministic heights, matching the prototype's formula.
            const int pct[4] = {50 + index * 17 % 40, 60 + index * 29 % 35,
                                40 + index * 13 % 45, 55 + index * 7 % 40};
            const float bh = (chH - inner * 2.f) * static_cast<float>(pct[i]) / 100.f;
            t.fillRoundedRect(Rect{ch.x + inner + static_cast<float>(i) * (barW + barGap),
                                   ch.bottom() - inner - bh, barW, bh},
                              Corners::all(W * 0.007f), accent_);
        }
        y += chH + gap;
    }

    // --- second paragraph ---------------------------------------------------
    paragraph(kParas[(static_cast<std::size_t>(index) + 1) % kParaCount]);

    // --- footer: .fo, 1.5cqw, muted, centred -------------------------------
    if (scale > 0.1f) {
        const float fSize = W * 0.015f;
        t.drawTextInBox(std::to_string(index + 1),
                        Rect{dest.x + pad, dest.bottom() - pad - fSize * 1.6f, colW,
                             fSize * 1.6f},
                        folio::render::TextStyle{fSize, folio::render::FontWeight::Regular, 0.f,
                                  Color{0x8A, 0x93, 0xA3, 0xFF}},
                        folio::render::TextAlign::Centre);
    }
}

// ---------------------------------------------------------------------------
// PageStage
// ---------------------------------------------------------------------------

PageStage::PageStage() {
    acceptsPointer = true;
    clipsContent = true;
    debugName = "PageStage";
    source_ = std::make_shared<PlaceholderPageSource>();
}

void PageStage::setSource(std::shared_ptr<IPageSource> src, double now) {
    // Selection indices are document-specific. Carrying them to a new PDF
    // painted hundreds of unrelated glyph-line rectangles over ordinary
    // text (and could even index beyond the new text layer).
    clearSelection();
    highlightMagnifierActive_ = false;
    highlightTouchControlled_ = highlightStrokeLocked_ = false;
    thumbCandidate_ = thumbDeferred_ = thumbTextSelecting_ =
        thumbAwaitingTap_ = thumbGestureFromPinned_ = thumbToolApplied_ = false;
    thumbLayer_ = ThumbLayer::Closed;
    thumbClosing_ = false;
    thumbToolsExit_.reset(1.f);
    thumbExit_.reset(1.f);
    thumbReveal_.reset(0.f);
    thumbOptionsReveal_.reset(0.f);
    thumbValuesReveal_.reset(0.f);
    searchHits_.clear();
    searchCurrent_ = -1;
    selecting_ = dragging_ = authoring_ = movingAnnot_ = false;
    selAnnot_ = 0;
    if (store_) store_->cancelDraft();
    source_ = std::move(src);
    lastGeometryRevision_ = source_ ? source_->geometryRevision() : 0;
    current_ = 0;
    scrollX_ = scrollY_ = 0.f;
    contentW_ = contentH_ = 0.f;
    placed_.clear();
    entrance_.clear();
    anchorValid_ = false;
    rasterScale_ = 0.f;
    velocity_ = overscrollY_ = overscrollVelocity_ = 0.f;
    smoothing_ = false;
    touch_.clear();
    touchAnnotId_ = 0;
    touchAxis_ = TouchAxis::Undecided;
    barDrag_ = barHover_ = Bar::None;
    barTouchHeld_ = barTrackPress_ = false;
    touchPanning_ = touchDrawing_ = pinchWasActive_ = false;
    lastLayoutW_ = 0.f;  // force relayout
    replayEntrance(now);
}

void PageStage::replayEntrance(double now) {
    if (!source_) return;
    const int n = source_->pageCount();
    entrance_.assign(static_cast<std::size_t>(std::max(0, n)), Timeline{});
    for (int i = 0; i < n && i < 24; ++i) {
        entrance_[static_cast<std::size_t>(i)].start(
            now, tokens::dur::pageEnter, tokens::delay::pageEnterStep * i,
            Curve::OutCubic);
    }
}

float PageStage::fitScale(bool width) const {
    if (!source_ || source_->pageCount() == 0) return 1.f;
    const PageSizePt ps = source_->pageSize(current_);
    const bool rotated = (rotation_ % 2) != 0;
    const float pw = rotated ? ps.h : ps.w;
    const float phh = rotated ? ps.w : ps.h;
    const float availW = std::max(1.f, frame.w - kMargin * 2.f);
    const float availH = std::max(1.f, frame.h - kMargin * 2.f);
    if (width) return availW / pw;
    return std::min(availW / pw, availH / phh);
}

float PageStage::effectiveScale(double now) const {
    return std::clamp(zoomAnim_.value(now), kMinZoom, kMaxZoom);
}

void PageStage::setZoomFocus(float localX, float localY) {
    if (frame.w > 1.f) anchorViewFracX_ = std::clamp(localX / frame.w, 0.f, 1.f);
    if (frame.h > 1.f) anchorViewFrac_ = std::clamp(localY / frame.h, 0.f, 1.f);
}

void PageStage::clearZoomFocus() {
    anchorViewFracX_ = 0.5f;
    anchorViewFrac_ = 0.5f;
}

void PageStage::captureZoomAnchor() {
    anchorValid_ = false;
    if (placed_.empty() || frame.h <= 1.f) return;
    const float probe = scrollY_ + frame.h * anchorViewFrac_;
    const float probeX = scrollX_ + frame.w * anchorViewFracX_;
    for (const Placed& p : placed_) {
        if (probe >= p.rect.y && probe < p.rect.bottom()) {
            anchorPage_ = p.index;
            anchorFrac_ = std::clamp((probe - p.rect.y) /
                                         std::max(1.f, p.rect.h), 0.f, 1.f);
            // A focal point in the page margin must anchor to the nearest
            // edge, never extrapolate past the page and pull it sideways.
            anchorFracX_ = std::clamp((probeX - p.rect.x) /
                                          std::max(1.f, p.rect.w), 0.f, 1.f);
            anchorValid_ = true;
            return;
        }
    }
    // The centre landed in the gap between two pages; anchor to the first
    // page starting below it so the jump is at most one gap.
    for (const Placed& p : placed_) {
        if (p.rect.y >= probe) {
            anchorPage_ = p.index;
            anchorFrac_ = 0.f;
            anchorFracX_ = std::clamp((probeX - p.rect.x) /
                                          std::max(1.f, p.rect.w), 0.f, 1.f);
            anchorValid_ = true;
            return;
        }
    }
}

void PageStage::applyZoomAnchor() {
    if (!anchorValid_ || placed_.empty()) return;
    for (const Placed& p : placed_) {
        if (p.index != anchorPage_) continue;
        scrollY_ = p.rect.y + anchorFrac_ * p.rect.h - frame.h * anchorViewFrac_;
        // A narrow anchor page can coexist with a much wider page in the
        // same continuous PDF. In that case the shared content width scrolls
        // even though THIS page still fits the viewport. Leaving scrollX at
        // zero pins the whole document to the left after zooming back in
        // from minimum scale (the vertical midpoint may be on a narrow page).
        if (contentW_ > frame.w + 1.f) {
            // Use the precise focal point even when the anchor page is
            // narrower than the viewport: off-centre pinches must still
            // keep the document point under the moving fingertips.
            scrollX_ = p.rect.x + anchorFracX_ * p.rect.w -
                       frame.w * anchorViewFracX_;
        }
        clampScroll();
        return;
    }
}

void PageStage::setZoom(float z, double now) {
    if (thumbLayer_ != ThumbLayer::Closed) closeThumb(now);
    captureZoomAnchor();
    zoomMode_ = ZoomMode::Free;
    zoomTarget_ = std::clamp(z, kMinZoom, kMaxZoom);
    zoomAnim_.animateTo(zoomTarget_, tokens::dur::pageWidth, now);
}

void PageStage::setZoomMode(ZoomMode m, double now) {
    if (thumbLayer_ != ThumbLayer::Closed) closeThumb(now);
    captureZoomAnchor();
    zoomMode_ = m;
    if (m == ZoomMode::Free) return;
    zoomTarget_ = fitScale(m == ZoomMode::FitWidth);
    zoomAnim_.animateTo(zoomTarget_, tokens::dur::pageWidth, now);
}

void PageStage::snapZoomToTarget(double now) {
    zoomAnim_.reset(std::clamp(zoomTarget_, kMinZoom, kMaxZoom));
    relayout(now);
    lastFitW_ = frame.w;
    rasterScale_ = 0.f;
    anchorValid_ = false;
}

PageStage::ViewState PageStage::captureViewState(double now) const {
    ViewState v;
    v.layout = layout_;
    v.zoomMode = zoomMode_;
    v.zoom = effectiveScale(now); // preserve what was actually on screen
    v.scrollX = scrollX_;
    v.scrollY = scrollY_;
    v.viewportW = frame.w;
    v.gridSize = gridSize_;
    v.rotation = rotation_;
    v.page = currentPage();
    return v;
}

void PageStage::restoreViewState(const ViewState& v, double now) {
    layout_ = v.layout;
    zoomMode_ = v.zoomMode;
    gridSize_ = v.gridSize;
    rotation_ = ((v.rotation % 4) + 4) % 4;
    rotationAnim_.reset(static_cast<float>(rotation_) * 90.f);
    zoomTarget_ = std::clamp(v.zoom, kMinZoom, kMaxZoom);
    if (zoomMode_ != ZoomMode::Free &&
        std::fabs(v.viewportW - frame.w) > 0.5f)
        zoomTarget_ = fitScale(zoomMode_ == ZoomMode::FitWidth);
    zoomAnim_.reset(zoomTarget_);
    current_ = source_ ? std::clamp(v.page - 1, 0,
                   std::max(0, source_->pageCount() - 1)) : 0;
    touch_.clear();
    touchAnnotId_ = 0;
    touchPanning_ = touchDrawing_ = pinchWasActive_ = false;
    movingAnnot_ = dragging_ = selecting_ = false;
    velocity_ = overscrollY_ = overscrollVelocity_ = 0.f;
    smoothing_ = anchorValid_ = false;
    scrollX_ = scrollY_ = 0.f;
    lastLayoutW_ = 0.f;
    rasterScale_ = 0.f;
    relayout(now);
    entrance_.clear(); // a tab switch is not a new-document fly-in
    scrollX_ = v.scrollX;
    scrollY_ = v.scrollY;
    clampScroll();
    lastFitW_ = frame.w;
    if (source_) source_->setFocusPage(current_);
    if (onPageChanged) onPageChanged(currentPage());
}

void PageStage::setLayout(LayoutMode m, double now) {
    if (m == layout_) return;
    layout_ = m;
    lastLayoutW_ = 0.f;
    if (zoomMode_ != ZoomMode::Free) setZoomMode(zoomMode_, now);
    replayEntrance(now);
}

void PageStage::rotate(int delta, double now) {
    rotation_ = ((rotation_ + delta) % 4 + 4) % 4;
    // Animate the visual angle continuously so a 270->0 step turns the short
    // way rather than spinning three quarters backwards.
    rotationAnim_.animateTo(rotationAnim_.target() + static_cast<float>(delta) * 90.f,
                            tokens::dur::pageRotate, now);
    lastLayoutW_ = 0.f;
    if (zoomMode_ != ZoomMode::Free) setZoomMode(zoomMode_, now);
}

void PageStage::goToPage(int oneBased, double now) {
    if (!source_) return;
    const int n = source_->pageCount();
    const int idx = std::clamp(oneBased - 1, 0, std::max(0, n - 1));
    if (idx == current_ && !placed_.empty()) return;
    current_ = idx;
    relayout(now);
    for (const Placed& p : placed_) {
        if (p.index != idx) continue;
        smoothScroll_.reset(scrollY_);
        smoothScroll_.animateTo(std::clamp(p.rect.y - kMargin, 0.f,
                                           std::max(0.f, contentH_ - frame.h)),
                                tokens::dur::screenSwap, now);
        smoothing_ = true;
        break;
    }
    if (onPageChanged) onPageChanged(currentPage());
}

void PageStage::relayout(double now) {
    placed_.clear();
    if (!source_) { contentH_ = 0.f; contentW_ = 0.f; return; }
    const int n = source_->pageCount();
    const float scale = effectiveScale(now);
    const bool rotated = (rotation_ % 2) != 0;

    auto sizeOf = [&](int i) {
        const PageSizePt ps = source_->pageSize(i);
        const float w = (rotated ? ps.h : ps.w) * scale;
        const float h = (rotated ? ps.w : ps.h) * scale;
        return Rect{0.f, 0.f, w, h};
    };

    float y = kMargin;
    switch (layout_) {
        case LayoutMode::Single: {
            // One page, centred, no vertical run.
            if (n > 0) {
                Rect r = sizeOf(current_);
                r.x = std::max(kMargin, (frame.w - r.w) * 0.5f);
                r.y = std::max(kMargin, (frame.h - r.h) * 0.5f);
                placed_.push_back({current_, r});
                y = r.bottom() + kMargin;
            }
            break;
        }
        case LayoutMode::TwoUp: {
            for (int i = 0; i < n; i += 2) {
                Rect a = sizeOf(i);
                Rect b = (i + 1 < n) ? sizeOf(i + 1) : Rect{};
                const float totalW = a.w + (b.w > 0.f ? kPageGap + b.w : 0.f);
                float x = std::max(kMargin, (frame.w - totalW) * 0.5f);
                a.x = x; a.y = y;
                placed_.push_back({i, a});
                if (b.w > 0.f) {
                    b.x = x + a.w + kPageGap;
                    b.y = y;
                    placed_.push_back({i + 1, b});
                }
                y += std::max(a.h, b.h) + kPageGap;
            }
            break;
        }
        case LayoutMode::Continuous: {
            for (int i = 0; i < n; ++i) {
                Rect r = sizeOf(i);
                r.x = std::max(kMargin, (frame.w - r.w) * 0.5f);
                r.y = y;
                placed_.push_back({i, r});
                y += r.h + kPageGap;
            }
            break;
        }
        case LayoutMode::Grid: {
            const float cell = gridSize_;
            const float cellH = cell * 1.42f;  // 7:10-ish with room for a label
            const int cols = std::max(1, static_cast<int>((frame.w - kMargin) /
                                                          (cell + kGridGap)));
            const float gridW = static_cast<float>(cols) * cell +
                                static_cast<float>(cols - 1) * kGridGap;
            const float x0 = std::max(kMargin, (frame.w - gridW) * 0.5f);
            for (int i = 0; i < n; ++i) {
                const int col = i % cols;
                const int row = i / cols;
                placed_.push_back(
                    {i, Rect{x0 + static_cast<float>(col) * (cell + kGridGap),
                             kMargin + static_cast<float>(row) * (cellH + kGridGap),
                             cell, cellH}});
            }
            const int rows = (n + cols - 1) / cols;
            y = kMargin + static_cast<float>(rows) * (cellH + kGridGap);
            break;
        }
    }
    contentH_ = y;

    // Centre every continuous-reading page in ONE common content width.
    // Centring each page in the viewport independently left narrower pages
    // stuck against the left edge when the next page was wide. It also made
    // the document seem to sidestep on a mixed-size PDF while scrolling.
    if (layout_ == LayoutMode::Continuous) {
        float widest = 0.f;
        for (const Placed& p : placed_) widest = std::max(widest, p.rect.w);
        contentW_ = std::max(frame.w, widest + 2.f * kMargin);
        for (Placed& p : placed_) p.rect.x = (contentW_ - p.rect.w) * 0.5f;
    } else {
        contentW_ = 0.f;
        for (const Placed& p : placed_)
            contentW_ = std::max(contentW_, p.rect.right() + kMargin);
    }
    if (contentW_ <= frame.w + 1.f) scrollX_ = 0.f;
    clampScroll();
    lastLayoutW_ = frame.w;
    lastLayoutScale_ = scale;
}

void PageStage::clampScroll() {
    const float maxS = std::max(0.f, contentH_ - frame.h);
    scrollY_ = std::clamp(scrollY_, 0.f, maxS);
    const float maxX = std::max(0.f, contentW_ - frame.w);
    scrollX_ = std::clamp(scrollX_, 0.f, maxX);
}

// --- scrollbars -------------------------------------------------------------
//
// Deliberately overlay bars rather than gutters that shrink the page area:
// the stage is the one place in the app where every pixel is content, and a
// permanent 12px gutter on two sides is a lot of paper to give up for
// something that is only occasionally needed.

namespace {
constexpr float kBarThick = 10.f;   // visual width of the bar
constexpr float kBarInset = 3.f;    // gap from the stage edge
constexpr float kBarMinLen = 36.f;  // never shrink the thumb past this
}  // namespace

Rect PageStage::vBarTrack() const {
    const Rect b = localBounds();
    const float bottomGap = canScrollX() ? kBarThick + kBarInset * 2.f : 0.f;
    return Rect{b.w - kBarThick - kBarInset, kBarInset, kBarThick,
                std::max(0.f, b.h - kBarInset * 2.f - bottomGap)};
}

Rect PageStage::vBarThumb() const {
    const Rect tr = vBarTrack();
    if (!canScrollY() || tr.h <= 0.f) return Rect{};
    const float frac = std::clamp(frame.h / std::max(1.f, contentH_), 0.f, 1.f);
    const float len = std::max(kBarMinLen, tr.h * frac);
    const float maxS = std::max(1.f, contentH_ - frame.h);
    const float t = std::clamp(scrollY_ / maxS, 0.f, 1.f);
    return Rect{tr.x, tr.y + t * (tr.h - len), tr.w, len};
}

Rect PageStage::hBarTrack() const {
    const Rect b = localBounds();
    const float rightGap = canScrollY() ? kBarThick + kBarInset * 2.f : 0.f;
    return Rect{kBarInset, b.h - kBarThick - kBarInset,
                std::max(0.f, b.w - kBarInset * 2.f - rightGap), kBarThick};
}

Rect PageStage::hBarThumb() const {
    const Rect tr = hBarTrack();
    if (!canScrollX() || tr.w <= 0.f) return Rect{};
    const float frac = std::clamp(frame.w / std::max(1.f, contentW_), 0.f, 1.f);
    const float len = std::max(kBarMinLen, tr.w * frac);
    const float maxS = std::max(1.f, contentW_ - frame.w);
    const float t = std::clamp(scrollX_ / maxS, 0.f, 1.f);
    return Rect{tr.x + t * (tr.w - len), tr.y, len, tr.h};
}

void PageStage::paintScrollbars(IPaintTarget& t, const PaintCtx& ctx) {
    if (!focusBarsVisible_) return;
    const float dark = tokens::darkness(ctx.pal);
    // Over a mid-grey backdrop neither black nor white reads well on its
    // own, so the thumb picks up the theme and the track stays nearly
    // invisible until it is needed.
    const auto thumbCol = [&](bool active) {
        const std::uint8_t v = dark > 0.5f ? 255 : 0;
        return Color{v, v, v, static_cast<std::uint8_t>(active ? 150 : 90)};
    };
    const auto trackCol = [&] {
        const std::uint8_t v = dark > 0.5f ? 255 : 0;
        return Color{v, v, v, 28};
    };

    if (canScrollY()) {
        const Rect tr = vBarTrack(), th = vBarThumb();
        t.fillRoundedRect(tr, Corners::all(kBarThick * 0.5f), trackCol());
        t.fillRoundedRect(th, Corners::all(kBarThick * 0.5f),
                          thumbCol(barDrag_ == Bar::Vertical ||
                                   barHover_ == Bar::Vertical));
    }
    if (canScrollX()) {
        const Rect tr = hBarTrack(), th = hBarThumb();
        t.fillRoundedRect(tr, Corners::all(kBarThick * 0.5f), trackCol());
        t.fillRoundedRect(th, Corners::all(kBarThick * 0.5f),
                          thumbCol(barDrag_ == Bar::Horizontal ||
                                   barHover_ == Bar::Horizontal));
    }
}

bool PageStage::handleScrollbar(const PointerEvent& e) {
    if (!focusBarsVisible_) return false;
    if (barTrackPress_ && e.pointerId == barTrackId_) {
        if (e.phase == PointerPhase::Up || e.phase == PointerPhase::Cancel)
            barTrackPress_ = false;
        return true; // a track press can never turn into a thumb drag
    }
    // A touch target wider than the painted bar is essential on a phone.
    const Rect v = vBarTrack(), h = hBarTrack();
    const bool overV = canScrollY() &&
        (e.isTouch() ? Rect{v.x - 22.f, v.y, v.w + 24.f, v.h} : v)
            .contains(e.pos.x, e.pos.y);
    const bool overH = canScrollX() &&
        (e.isTouch() ? Rect{h.x, h.y - 22.f, h.w, h.h + 24.f} : h)
            .contains(e.pos.x, e.pos.y);

    if (e.phase == PointerPhase::Move && barDrag_ == Bar::None) {
        const Bar was = barHover_;
        barHover_ = overV ? Bar::Vertical : (overH ? Bar::Horizontal : Bar::None);
        return barHover_ != Bar::None || was != Bar::None ? barHover_ != Bar::None
                                                          : false;
    }

    if (e.phase == PointerPhase::Down && (overV || overH)) {
        // The rail is not a page-jump target. Swallow a press on its empty
        // portion (including later Move/Up), so it neither teleports the PDF
        // nor accidentally paints/activates the page beneath it.
        const Rect th = overV ? vBarThumb() : hBarThumb();
        const Rect grab = e.isTouch()
            ? (overV ? Rect{th.x - 22.f, th.y, th.w + 24.f, th.h}
                     : Rect{th.x, th.y - 22.f, th.w, th.h + 24.f})
            : th;
        if (!grab.contains(e.pos.x, e.pos.y)) {
            barTrackPress_ = true;
            barTrackId_ = e.pointerId;
            return true;
        }
        if (overV) {
            barDrag_ = Bar::Vertical;
            barGrabOffset_ = e.pos.y - th.y;
        } else {
            barDrag_ = Bar::Horizontal;
            barGrabOffset_ = e.pos.x - th.x;
        }
        smoothing_ = false;
        velocity_ = 0.f;
    }

    if (barDrag_ == Bar::None) return false;

    if (e.phase == PointerPhase::Down || e.phase == PointerPhase::Move) {
        if (barDrag_ == Bar::Vertical) {
            const Rect tr = vBarTrack();
            const float len = vBarThumb().h;
            const float span = std::max(1.f, tr.h - len);
            const float t0 = std::clamp((e.pos.y - barGrabOffset_ - tr.y) / span,
                                        0.f, 1.f);
            scrollY_ = t0 * std::max(0.f, contentH_ - frame.h);
        } else {
            const Rect tr = hBarTrack();
            const float len = hBarThumb().w;
            const float span = std::max(1.f, tr.w - len);
            const float t0 = std::clamp((e.pos.x - barGrabOffset_ - tr.x) / span,
                                        0.f, 1.f);
            scrollX_ = t0 * std::max(0.f, contentW_ - frame.w);
        }
        clampScroll();
        updateCurrentFromScroll();
        return true;
    }

    if (e.phase == PointerPhase::Up || e.phase == PointerPhase::Cancel) {
        barDrag_ = Bar::None;
        return true;
    }
    return false;
}

void PageStage::updateCurrentFromScroll() {
    if (placed_.empty() || layout_ == LayoutMode::Single) return;
    // The current page is the one covering the viewport's upper third - a
    // more stable choice than "topmost visible", which flickers between two
    // pages at a boundary.
    const float probe = scrollY_ + frame.h * 0.33f;
    int best = current_;
    for (const Placed& p : placed_) {
        if (probe >= p.rect.y && probe < p.rect.bottom()) { best = p.index; break; }
        if (p.rect.y > probe) break;
        best = p.index;
    }
    if (best != current_) {
        current_ = best;
        if (onPageChanged) onPageChanged(currentPage());
    }
}

Rect PageStage::sourceRect(const Rect& target) const noexcept {
    if ((rotation_ & 1) == 0) return target;
    return Rect{target.x + (target.w - target.h) * .5f,
                target.y + (target.h - target.w) * .5f,
                target.h, target.w};
}

Point PageStage::toViewNorm(Point n) const noexcept {
    switch (rotation_ & 3) {
        case 1: return {1.f - n.y, n.x};
        case 2: return {1.f - n.x, 1.f - n.y};
        case 3: return {n.y, 1.f - n.x};
        default: return n;
    }
}

Point PageStage::toSourceNorm(Point n) const noexcept {
    switch (rotation_ & 3) {
        case 1: return {n.y, 1.f - n.x};
        case 2: return {1.f - n.x, 1.f - n.y};
        case 3: return {1.f - n.y, n.x};
        default: return n;
    }
}

Rect PageStage::toViewRect(const Rect& box, const Rect& target) const noexcept {
    const Point a = toViewNorm({box.x, box.y});
    const Point b = toViewNorm({box.right(), box.bottom()});
    const float l = std::min(a.x, b.x), tp = std::min(a.y, b.y);
    return {target.x + l * target.w, target.y + tp * target.h,
            std::fabs(a.x - b.x) * target.w,
            std::fabs(a.y - b.y) * target.h};
}

bool PageStage::hitPage(Point local, int* pageOut, Point* normOut) const {
    const float sy = scrollY_ - overscrollY_, sx = scrollX_;
    for (const Placed& p : placed_) {
        const Rect r{p.rect.x - sx, p.rect.y - sy, p.rect.w, p.rect.h};
        if (!r.contains(local.x, local.y)) continue;
        if (pageOut) *pageOut = p.index;
        if (normOut)
            *normOut = toSourceNorm(Point{
                (local.x - r.x) / std::max(1.f, r.w),
                (local.y - r.y) / std::max(1.f, r.h)});
        return true;
    }
    return false;
}

namespace {
/// Normalised box between two normalised corners, always positive-sized.
Rect boxBetween(Point a, Point b) {
    return Rect{std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x),
                std::fabs(b.y - a.y)};
}
}  // namespace

bool PageStage::deleteSelectedAnnot() {
    if (!selAnnot_ || !store_) return false;
    const Annotation* a = store_->get(selAnnot_);
    if (!a) { selAnnot_ = 0; return false; }
    // Delete the whole markup, not the line that happened to be clicked.
    const std::vector<std::uint64_t> ids = store_->groupMembers(selAnnot_);
    std::vector<Annotation> copies;
    copies.reserve(ids.size());
    for (std::uint64_t id : ids)
        if (const Annotation* m = store_->get(id)) copies.push_back(*m);
    for (std::uint64_t id : ids) store_->removeById(id);
    selAnnot_ = 0;
    if (onAnnotSelectionChanged) onAnnotSelectionChanged(0, false);
    if (onAnnotationGroupDeleted) onAnnotationGroupDeleted(std::move(copies));
    else if (onAnnotationDeleted)
        for (const Annotation& c : copies) onAnnotationDeleted(c);
    if (onAnnotationsChanged) onAnnotationsChanged();
    return true;
}

/// Pick up, drag and drop an existing annotation. Runs only for the Select
/// tool, and only before text selection gets a look at the event - clicking
/// a highlight should grab the highlight, not start selecting the words
/// underneath it.
namespace {
bool textAnchored(const Annotation& a) { return a.textLinked; }
Rect annotBounds(const Annotation& a) {
    if (a.points.empty()) return a.box;
    float l = a.points[0].x, r = l, t = a.points[0].y, b = t;
    for (const Point& p : a.points) {
        l = std::min(l, p.x); r = std::max(r, p.x);
        t = std::min(t, p.y); b = std::max(b, p.y);
    }
    return Rect{l, t, r - l, b - t};
}
}

int PageStage::hitAnnotGrip(Point local, bool touch) const {
    const Annotation* a = store_ ? store_->get(selAnnot_) : nullptr;
    if (!a || textAnchored(*a)) return -1;
    const auto it = std::find_if(placed_.begin(), placed_.end(),
        [a](const Placed& p) { return p.index == a->page; });
    if (it == placed_.end()) return -1;
    const Rect b = annotBounds(*a);
    const Point grips[4] = {{b.x, b.y}, {b.right(), b.y},
                            {b.x, b.bottom()}, {b.right(), b.bottom()}};
    const float radius = touch ? 22.f : 9.f; // even in empty corners
    for (int k = 0; k < 4; ++k) {
        const Point v = toViewNorm(grips[k]);
        const float gx = it->rect.x - scrollX_ + v.x * it->rect.w;
        const float gy = it->rect.y - scrollY_ + overscrollY_ + v.y * it->rect.h;
        const float dx = local.x - gx, dy = local.y - gy;
        if (dx * dx + dy * dy <= radius * radius) return k;
    }
    return -1;
}

bool PageStage::selectionContains(Point local) const {
    if (!hasSelection()) return false;
    TextPos a = selA_, b = selB_;
    if (b < a) std::swap(a, b);
    for (const auto& p : placed_) {
        if (p.index < a.page || p.index > b.page) continue;
        const auto* glyphs = source_ ? source_->glyphs(p.index) : nullptr;
        if (!glyphs) continue;
        std::vector<Rect> lines;
        selectionLines(p.index, p.index == a.page ? a.glyph : 0,
                       p.index == b.page ? b.glyph : static_cast<int>(glyphs->size()) - 1,
                       lines);
        for (const Rect& line : lines) {
            Rect box = toViewRect(line, p.rect);
            box.x -= scrollX_;
            box.y -= scrollY_ - overscrollY_;
            if (box.inset(-4.f).contains(local.x, local.y)) return true;
        }
    }
    return false;
}

bool PageStage::handleAnnotSelection(const PointerEvent& e) {
    if (!store_ || editTool_ != EditTool::None) return false;

    switch (e.phase) {
        case PointerPhase::Down: {
            // A hollow ellipse has no ink at the bounding-box corners.
            // Search the CURRENT selection's handles in screen space before
            // hit-testing marks or even requiring the point to lie on paper.
            const int corner = hitAnnotGrip(e.pos, e.isTouch());
            int page = 0;
            Point n{0.f, 0.f};
            if (corner >= 0) {
                const Annotation* selected = store_->get(selAnnot_);
                page = selected->page;
                const auto it = std::find_if(placed_.begin(), placed_.end(),
                    [page](const Placed& p) { return p.index == page; });
                if (it == placed_.end()) return false;
                n = toSourceNorm(Point{
                    (e.pos.x + scrollX_ - it->rect.x) / std::max(1.f, it->rect.w),
                    (e.pos.y + scrollY_ - overscrollY_ - it->rect.y) /
                        std::max(1.f, it->rect.h)});
            } else if (!hitPage(e.pos, &page, &n)) {
                pendingAnnotClear_ = selAnnot_ != 0;
                blankDown_ = e.pos;
                return false;
            }
            std::uint64_t hit = corner >= 0 ? selAnnot_ :
                store_->hitTest(page, n, e.isTouch() ? 0.025f : 0.008f);
            if (!hit && onImportedAnnotHit)
                hit = onImportedAnnotHit(page, n);
            if (!hit) {
                // Wait for Up: this may be the start of a pan. Never drop
                // selection at Down or the user loses their edit handles.
                pendingAnnotClear_ = selAnnot_ != 0;
                blankDown_ = e.pos;
                return false;
            }
            pendingAnnotClear_ = false;
            selAnnot_ = hit;
            if (const Annotation* a = store_->get(hit)) moveBefore_ = *a;
            movingAnnot_ = true;
            resizeCorner_ = corner;
            annotChanged_ = false;
            moveLast_ = n;
            if (onAnnotSelectionChanged) onAnnotSelectionChanged(hit, e.isTouch());
            return true;
        }
        case PointerPhase::Move: {
            // `pressed` is the router's own press state for this widget;
            // e.buttonsDown is not populated by every host.
            if (!movingAnnot_ || (!pressed && !e.isTouch())) return false;
            Annotation* a = store_->get(selAnnot_);
            if (!a) return true;
            int page = 0;
            Point n{0.f, 0.f};
            if (!hitPage(e.pos, &page, &n) || page != a->page) {
                // A finger can leave the paper while moving a mark. Clamp
                // against the ORIGINAL page instead of silently dropping
                // the drag or teleporting onto the next page.
                const auto it = std::find_if(placed_.begin(), placed_.end(),
                    [a](const Placed& p) { return p.index == a->page; });
                if (it == placed_.end()) return true;
                n.x = std::clamp((e.pos.x + scrollX_ - it->rect.x) /
                                 std::max(1.f, it->rect.w), 0.f, 1.f);
                n.y = std::clamp((e.pos.y + scrollY_ - overscrollY_ - it->rect.y) /
                                 std::max(1.f, it->rect.h), 0.f, 1.f);
            }
            if (textAnchored(*a)) return true; // text quads never drift
            if (resizeCorner_ >= 0) {
                const Rect old = annotBounds(moveBefore_);
                const Point fixed{resizeCorner_ % 2 == 0 ? old.right() : old.x,
                                  resizeCorner_ < 2 ? old.bottom() : old.y};
                const float minW = std::min(.012f, old.w);
                const float minH = std::min(.012f, old.h);
                const float xx = std::clamp(n.x, 0.f, 1.f);
                const float yy = std::clamp(n.y, 0.f, 1.f);
                const float movingX = resizeCorner_ % 2 == 0
                    ? std::min(xx, fixed.x - minW)
                    : std::max(xx, fixed.x + minW);
                const float movingY = resizeCorner_ < 2
                    ? std::min(yy, fixed.y - minH)
                    : std::max(yy, fixed.y + minH);
                const Rect box{std::min(movingX, fixed.x),
                               std::min(movingY, fixed.y),
                               std::fabs(movingX - fixed.x),
                               std::fabs(movingY - fixed.y)};
                a->box = box;
                if (!moveBefore_.points.empty()) {
                    a->points = moveBefore_.points;
                    for (Point& p : a->points) {
                        p.x = box.x + (p.x - old.x) * box.w / std::max(old.w, 1e-6f);
                        p.y = box.y + (p.y - old.y) * box.h / std::max(old.h, 1e-6f);
                    }
                }
                if (a->kind == AnnotKind::Text && moveBefore_.fontSize > 0.f)
                    a->fontSize = moveBefore_.fontSize * box.h /
                                  std::max(old.h, 1e-6f);
            } else {
                const float dx = n.x - moveLast_.x, dy = n.y - moveLast_.y;
                moveLast_ = n;
                a->box.x += dx;
                a->box.y += dy;
                for (Point& p : a->points) { p.x += dx; p.y += dy; }
            }
            annotChanged_ = true;
            store_->touch();
            return true;
        }
        case PointerPhase::Cancel:
            if (!movingAnnot_) return false;
            movingAnnot_ = false;
            resizeCorner_ = -1;
            annotChanged_ = false;
            // A second finger steals this gesture for pinch; undo any
            // uncommitted drag without creating a history entry.
            if (store_->get(selAnnot_)) store_->replace(selAnnot_, moveBefore_);
            return true;
        case PointerPhase::Up: {
            if (!movingAnnot_) return false;
            movingAnnot_ = false;
            resizeCorner_ = -1;
            const Annotation* a = store_->get(selAnnot_);
            if (a && annotChanged_) {
                if (onAnnotationMoved) onAnnotationMoved(moveBefore_, *a);
                if (onAnnotationsChanged) onAnnotationsChanged();
            } else if (a && e.isTouch() && onAnnotSelectionChanged) {
                // Open the inspector only AFTER a tap is released. Opening
                // it on Down steals a possible move/resize from the finger.
                onAnnotSelectionChanged(selAnnot_, false);
            }
            annotChanged_ = false;
            return true;
        }
        default:
            break;
    }
    return false;
}

void PageStage::paintAnnotSelection(IPaintTarget& t, const PaintCtx& ctx) {
    if (!selAnnot_ || !store_) return;
    const Annotation* a = store_->get(selAnnot_);
    if (!a) return;

    // A multi-line markup is several annotations but one selection. Outline
    // every line, and put the handles on the union - otherwise clicking the
    // middle line of a highlighted paragraph looks like it picked one line
    // while the colour buttons quietly change all three.
    const std::vector<std::uint64_t> ids = store_->groupMembers(selAnnot_);
    Rect uni{0.f, 0.f, 0.f, 0.f};
    bool haveUni = false;
    for (std::uint64_t id : ids) {
        const Annotation* m = store_->get(id);
        if (!m) continue;
        const Rect* mpr = nullptr;
        for (const auto& p : placed_)
            if (p.index == m->page) { mpr = &p.rect; break; }
        if (!mpr) continue;

        // Ink has no meaningful box until it is measured, so derive one.
        Rect b = m->box;
        if (!m->points.empty()) {
            float l = m->points[0].x, r = l, tp = m->points[0].y, bt = tp;
            for (const Point& p : m->points) {
                l = std::min(l, p.x); r = std::max(r, p.x);
                tp = std::min(tp, p.y); bt = std::max(bt, p.y);
            }
            b = Rect{l, tp, r - l, bt - tp};
        }
        const Rect ms = toViewRect(b, *mpr);
        const Rect mo = ms.inset(-3.f);
        if (ids.size() > 1 || textAnchored(*m))
            t.strokeRoundedRect(mo, Corners::all(3.f),
                                fade(ctx.pal.accent, 0.65f), 1.f);
        if (!haveUni) { uni = mo; haveUni = true; }
        else {
            const float l = std::min(uni.x, mo.x), tp = std::min(uni.y, mo.y);
            const float r = std::max(uni.right(), mo.right());
            const float bt = std::max(uni.bottom(), mo.bottom());
            uni = Rect{l, tp, r - l, bt - tp};
        }
    }
    if (!haveUni) return;
    // Never draw a paragraph-sized bounding box across multiple text quads.
    if (textAnchored(*a)) return;
    const Rect o = ids.size() > 1 ? uni.inset(-3.f) : uni.inset(-2.f);
    t.strokeRoundedRect(o, Corners::all(4.f), ctx.pal.accent, 1.5f);
    // Text markup belongs to the glyphs it was created from. It can be
    // recoloured/deleted, but has neither move nor resize grips.
    if (textAnchored(*a)) return;
    // Free-form shapes and ink get four corner resize grips.
    const float h = 3.5f;
    for (const Point& c : {Point{o.x, o.y}, Point{o.right(), o.y},
                           Point{o.x, o.bottom()}, Point{o.right(), o.bottom()}}) {
        t.fillRoundedRect(Rect{c.x - h, c.y - h, h * 2.f, h * 2.f},
                          Corners::all(h), ctx.pal.accent);
        t.strokeRoundedRect(Rect{c.x - h, c.y - h, h * 2.f, h * 2.f},
                            Corners::all(h), Color{255, 255, 255, 220}, 1.f);
    }
}

// --- free text ---------------------------------------------------------------
//
// A free-text annotation is typed directly onto the page rather than into a
// floating input. That means this widget has to own a caret for as long as
// the edit lasts, and has to be careful to hand every other key back to the
// screen - otherwise Ctrl+S would insert an "s" instead of saving.

void PageStage::setTextStyle(float normalisedSize,
                             folio::render::FontFamily family) {
    textSize_ = normalisedSize;
    textFont_ = family;
    if (!store_) return;
    // Apply to the live target as well as the default, so the panel reads
    // as a property editor rather than a setting for next time.
    const std::uint64_t id = editText_ ? editText_ : selAnnot_;
    if (!id) return;
    if (Annotation* a = store_->get(id); a && a->kind == AnnotKind::Text) {
        a->fontSize = normalisedSize;
        a->font = family;
        a->box.h = normalisedSize / 0.74f;
        store_->touch();
    }
}

void PageStage::beginTextEdit(std::uint64_t id) {
    if (!store_) return;
    const Annotation* a = store_->get(id);
    if (!a || a->kind != AnnotKind::Text) return;
    commitText();
    editText_   = id;
    if (onTextInputRequested) onTextInputRequested(true);
    textCaret_  = a->text.size();
    textIsNew_  = false;
    caretBlink_ = now();
    selAnnot_   = 0;
}

void PageStage::commitText(bool cancel) {
    if (!editText_ || !store_) { editText_ = 0; return; }
    const std::uint64_t id = editText_;
    editText_ = 0;
    if (onTextInputRequested) onTextInputRequested(false);
    const Annotation* a = store_->get(id);
    if (!a) return;

    // An empty box is not an annotation, it is a stray click. Drop it
    // silently and - crucially - without an undo entry, so Ctrl+Z after
    // misclicking still undoes whatever the user actually did last.
    if (cancel || a->text.empty()) {
        const Annotation copy = *a;
        store_->removeById(id);
        if (!textIsNew_ && onAnnotationDeleted) onAnnotationDeleted(copy);
        if (onAnnotationsChanged) onAnnotationsChanged();
        return;
    }
    if (textIsNew_ && onAnnotationAdded) onAnnotationAdded(*a);
    textIsNew_ = false;
    if (onAnnotationsChanged) onAnnotationsChanged();
}

bool PageStage::handleTextKey(const KeyEvent& e) {
    if (!editText_ || !store_) return false;
    if (!e.pressed) return false;
    Annotation* a = store_->get(editText_);
    if (!a) { editText_ = 0; return false; }

    // Modifier combinations stay with the application. Only bare typing and
    // the editing keys belong to the caret.
    const bool mods =
        (e.modifiers & ModCtrl) || (e.modifiers & ModAlt) || (e.modifiers & ModMeta);

    if (e.key == Key::Escape) { commitText(true); return true; }
    if (e.key == Key::Enter)  { commitText(false); return true; }

    if (!mods && e.key == Key::Backspace) {
        if (textCaret_ > 0) {
            a->text.erase(textCaret_ - 1, 1);
            --textCaret_;
            store_->touch();
        }
        caretBlink_ = now();
        return true;
    }
    if (!mods && e.key == Key::Delete) {
        if (textCaret_ < a->text.size()) { a->text.erase(textCaret_, 1); store_->touch(); }
        caretBlink_ = now();
        return true;
    }
    if (e.key == Key::Left)  { if (textCaret_) --textCaret_; caretBlink_ = now(); return true; }
    if (e.key == Key::Right) { if (textCaret_ < a->text.size()) ++textCaret_; caretBlink_ = now(); return true; }

    if (!mods && !e.text.empty()) {
        a->text.insert(textCaret_, e.text);
        textCaret_ += e.text.size();
        store_->touch();
        caretBlink_ = now();
        // Grow the box with the text so a long line is not clipped. Rough
        // advance per character is good enough here - the real measure
        // happens at paint time, and the box only bounds the hit area.
        const float em = a->fontSize > 0.f ? a->fontSize : a->box.h * 0.74f;
        a->box.w = std::max(a->box.w,
                            em * 0.56f * static_cast<float>(a->text.size()) + 0.02f);
        return true;
    }
    return false;   // not ours - let the screen have it
}

void PageStage::paintTextCaret(IPaintTarget& t, const PaintCtx& ctx) {
    if (!editText_ || !store_) return;
    const Annotation* a = store_->get(editText_);
    if (!a) return;
    const Rect* pr = nullptr;
    for (const auto& p : placed_)
        if (p.index == a->page) { pr = &p.rect; break; }
    if (!pr) return;

    const Rect source = sourceRect(*pr);
    t.save();
    t.translate(pr->x + pr->w * .5f, pr->y + pr->h * .5f);
    t.rotateDegrees(static_cast<float>(rotation_) * 90.f);
    t.translate(-(pr->x + pr->w * .5f), -(pr->y + pr->h * .5f));
    const Rect box{source.x + a->box.x * source.w,
                   source.y + a->box.y * source.h,
                   a->box.w * source.w, a->box.h * source.h};
    // A dashed-looking frame while editing, so it is obvious the thing is
    // live and will keep swallowing keys until Enter or Escape.
    t.fillRoundedRect(box.inset(-4.f), Corners::all(4.f),
                      Color{ctx.pal.accent.r, ctx.pal.accent.g, ctx.pal.accent.b, 18});
    t.strokeRoundedRect(box.inset(-4.f), Corners::all(4.f), ctx.pal.accent, 1.5f);

    // Caret: 530 ms on, 530 ms off, restarted on every keystroke so it is
    // always solid while the user is actually typing.
    const double since = now() - caretBlink_;
    if (std::fmod(since, 1.06) < 0.53) {
        const float em = (a->fontSize > 0.f ? a->fontSize : a->box.h * 0.74f) * source.h;
        const float adv = em * 0.56f;   // matches the glyph advance
        const float cx = box.x + 2.f + adv * static_cast<float>(textCaret_);
        t.fillRect(Rect{cx, box.y + 2.f, 1.6f, std::max(10.f, box.h - 4.f)},
                   ctx.pal.accent);
    }
    t.restore();
}

bool PageStage::handleAuthoring(const PointerEvent& e) {
    if (!store_ || editTool_ == EditTool::None) return false;

    int page = 0;
    Point n{0.f, 0.f};
    const bool over = hitPage(e.pos, &page, &n);

    switch (e.phase) {
        case PointerPhase::Down: {
            if (!over) return false;
            if (editTool_ == EditTool::Eraser) {
                // Capture the victim before it goes: an eraser stroke has to
                // be undoable, and once removeById has run the annotation is
                // unrecoverable.
                if (const std::uint64_t id = store_->hitTest(page, n, 0.012f)) {
                    const Annotation* a = store_->get(id);
                    const Annotation copy = a ? *a : Annotation{};
                    store_->removeById(id);
                    if (selAnnot_ == id) selAnnot_ = 0;
                    if (onAnnotationDeleted) onAnnotationDeleted(copy);
                    if (onAnnotationsChanged) onAnnotationsChanged();
                }
                authoring_ = true;
                authorPage_ = page;
                return true;
            }
            Annotation a;
            a.page = page;
            a.color = annotColor_;
            a.width = annotWidth_;
            a.opacity = annotOpacity_;
            switch (editTool_) {
                case EditTool::Highlight: a.kind = AnnotKind::Highlight; break;
                case EditTool::Ink:       a.kind = AnnotKind::Ink;       break;
                case EditTool::Note:      a.kind = AnnotKind::Note;      break;
                case EditTool::Box:       a.kind = AnnotKind::Box;       break;
                case EditTool::Ellipse:   a.kind = AnnotKind::Ellipse;   break;
                case EditTool::Line:      a.kind = AnnotKind::Line;      break;
                case EditTool::Arrow:     a.kind = AnnotKind::Arrow;     break;
                case EditTool::Text:      a.kind = AnnotKind::Text;      break;
                case EditTool::Stamp:     a.kind = AnnotKind::Stamp;     break;
                case EditTool::Signature: a.kind = AnnotKind::Signature; break;
                default: return false;
            }
            if (a.kind == AnnotKind::Ink || a.kind == AnnotKind::Signature ||
                a.kind == AnnotKind::Line || a.kind == AnnotKind::Arrow) {
                a.points.push_back(n);
                if (a.kind == AnnotKind::Line || a.kind == AnnotKind::Arrow)
                    a.points.push_back(n);
            } else {
                a.box = Rect{n.x, n.y, 0.f, 0.f};
            }
            if (a.kind == AnnotKind::Note) {
                // A note is a fixed-size pin, placed on press.
                a.box = Rect{n.x, n.y, 0.03f, 0.03f};
                const auto id = store_->add(a);
                selAnnot_ = id;
                if (onAnnotationAdded) onAnnotationAdded(*store_->get(id));
                if (onAnnotSelectionChanged) onAnnotSelectionChanged(id, true);
                if (onAnnotationsChanged) onAnnotationsChanged();
                authoring_ = true;
                authorPage_ = page;
                return true;
            }
            if (a.kind == AnnotKind::Text) {
                // Free text is placed on press and typed in place - there
                // is no drag to size it, because the text decides the size.
                // Committing the previous one first means clicking a second
                // spot finishes the first rather than orphaning it.
                commitText();
                a.text.clear();
                a.fontSize = textSize_;
                a.font = textFont_;
                a.box = Rect{n.x, n.y, 0.26f, textSize_ / 0.74f};
                const std::uint64_t id = store_->add(a);
                editText_  = id;
                if (onTextInputRequested) onTextInputRequested(true);
                textCaret_ = 0;
                textIsNew_ = true;
                caretBlink_ = now();
                selAnnot_ = 0;
                if (onAnnotationsChanged) onAnnotationsChanged();
                return true;
            }
            store_->beginDraft(std::move(a));
            authoring_ = true;
            authorPage_ = page;
            authorStart_ = n;
            highlightTouchControlled_ = editTool_ == EditTool::Highlight && e.isTouch();
            highlightStrokeLocked_ = false;
            highlightTouchDown_ = e.pos;
            if (highlightTouchControlled_ && magnifierEnabled_ &&
                layout_ != LayoutMode::Grid) {
                // This tool paints a HORIZONTAL strip. The preview begins
                // immediately at Down, while the draft still has zero width.
                highlightMagnifierActive_ = true;
                highlightPointer_ = e.pointerId;
                highlightCursor_ = n;
            }
            return true;
        }
        case PointerPhase::Move: {
            if (!authoring_) return false;
            if (editTool_ == EditTool::Eraser) {
                // Same capture-then-remove as the press, so dragging the
                // eraser across five marks produces five undo entries
                // rather than five irreversible deletions.
                if (over && page == authorPage_) {
                    if (const std::uint64_t id = store_->hitTest(page, n, 0.012f)) {
                        const Annotation* a = store_->get(id);
                        const Annotation copy = a ? *a : Annotation{};
                        store_->removeById(id);
                        if (selAnnot_ == id) selAnnot_ = 0;
                        if (onAnnotationDeleted) onAnnotationDeleted(copy);
                        if (onAnnotationsChanged) onAnnotationsChanged();
                    }
                }
                return true;
            }
            Annotation* d = store_->draft();
            if (!d) return false;
            // Map every sample against the page where the stroke began.
            // hitPage() may now report the *next* page (or no page), whose
            // normalised coordinates would make the pen jump across paper.
            Point c = n;
            if (!over || page != authorPage_) {
                auto it = std::find_if(placed_.begin(), placed_.end(),
                    [this](const Placed& p) { return p.index == authorPage_; });
                if (it == placed_.end()) return true;
                c = Point{(e.pos.x + scrollX_ - it->rect.x) /
                              std::max(1.f, it->rect.w),
                          (e.pos.y + scrollY_ - it->rect.y) /
                              std::max(1.f, it->rect.h)};
            }
            c.x = std::clamp(c.x, 0.f, 1.f);
            c.y = std::clamp(c.y, 0.f, 1.f);
            if (d->kind == AnnotKind::Line || d->kind == AnnotKind::Arrow) {
                d->points.back() = c;
                d->box = boxBetween(authorStart_, c);
            } else if (d->kind == AnnotKind::Ink || d->kind == AnnotKind::Signature) {
                // Drop samples closer than ~0.3% of the page: raw pointer
                // streams are far denser than the curve needs and every extra
                // point costs on every later frame.
                if (d->points.empty() ||
                    std::fabs(c.x - d->points.back().x) +
                            std::fabs(c.y - d->points.back().y) >
                        0.003f) {
                    d->points.push_back(c);
                }
            } else if (d->kind == AnnotKind::Highlight) {
                if (highlightTouchControlled_ && !highlightStrokeLocked_) {
                    // Preparation, not painting: let the finger (and loupe)
                    // find the right row without leaving a vertical mark.
                    // Use logical dp, not normalised PDF width, so zoom does
                    // not change what counts as an intentional horizontal
                    // drag. Small X jitter follows the preview but draws 0.
                    if (std::fabs(e.pos.x - highlightTouchDown_.x) <=
                        gesture::kSlop) {
                        authorStart_ = c;
                        d->box = {c.x, c.y, 0.f, 0.f};
                        if (highlightMagnifierActive_) highlightCursor_ = c;
                        return true;
                    }
                    // Freeze the LAST previewed row. The first sideways
                    // sample may also have diagonal finger wobble; taking
                    // its Y would jump the mark just as drawing begins.
                    highlightStrokeLocked_ = true;
                }
                if (highlightMagnifierActive_ && e.isTouch() &&
                    e.pointerId == highlightPointer_)
                    highlightCursor_ = {c.x, authorStart_.y};
                const float bandH = highlightBandHeight(annotWidth_);
                d->box = Rect{std::min(authorStart_.x, c.x),
                              authorStart_.y - bandH * .5f,
                              std::fabs(c.x - authorStart_.x), bandH};
            } else {
                d->box = boxBetween(authorStart_, c);
            }
            return true;
        }
        case PointerPhase::Cancel:
            if (!authoring_) return false;
            authoring_ = false;
            highlightTouchControlled_ = highlightStrokeLocked_ = false;
            store_->cancelDraft();
            penPressSum_ = 0.f;
            penPressN_ = 0;
            return true;
        case PointerPhase::Up: {
            if (!authoring_) return false;
            authoring_ = false;
            highlightTouchControlled_ = highlightStrokeLocked_ = false;
            if (editTool_ == EditTool::Note) {
                // Only now raise the properties sheet; on Down it would
                // steal the finger that is finishing placement.
                if (onAnnotSelectionChanged) onAnnotSelectionChanged(selAnnot_, false);
                return true;
            }
            // A pen stroke commits at a width scaled by how hard it was
            // pressed. Mean pressure over the stroke, not the value at
            // lift-off, which is always near zero as the nib comes away.
            if (penPressN_ > 0 && store_) {
                if (Annotation* d = store_->draft()) {
                    if (d->kind == AnnotKind::Ink ||
                        d->kind == AnnotKind::Signature) {
                        const float mean =
                            penPressSum_ / static_cast<float>(penPressN_);
                        d->width *= std::clamp(0.45f + mean, 0.45f, 1.75f);
                    }
                }
                penPressSum_ = 0.f;
                penPressN_ = 0;
            }
            if (editTool_ != EditTool::Eraser) {
                const std::uint64_t id = store_->commitDraft();
                if (id && onAnnotationAdded) {
                    if (const Annotation* a = store_->get(id))
                        onAnnotationAdded(*a);
                }
                if (id && onAnnotationsChanged) onAnnotationsChanged();
            }
            return true;
        }
        default:
            break;
    }
    return false;
}

namespace {
/// Append `cp` to `out` as UTF-8.
void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}
}  // namespace

void PageStage::selectWordAt(int page, int glyph) {
    const auto* gs = source_ ? source_->glyphs(page) : nullptr;
    if (!gs || gs->empty()) return;
    const int last = static_cast<int>(gs->size()) - 1;
    if (glyph < 0 || glyph > last) return;

    // Expand over a run of ONE script class, not over "everything that is
    // not a space". Japanese has no spaces, so a whitespace-only rule
    // selects the whole page; worse, it is silent about it. Classing the
    // characters instead costs nothing and gets CJK roughly right:
    // a kanji stem, its hiragana okurigana and a katakana loanword each
    // form their own run, which is where a reader expects the selection
    // to stop. Real segmentation needs a dictionary (MeCab/ICU); this is
    // the honest approximation until one is linked.
    enum Cls { Sep = 0, Word, Han, Kana, Kata, Hangul };
    const auto classOf = [](char32_t c) -> Cls {
        if (c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == 0xA0 ||
            c == 0x3000)
            return Sep;
        if ((c >= 0x3001 && c <= 0x303F) ||            // 、。「」etc
            (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) ||
            (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65))
            return Sep;                                // fullwidth punctuation
        if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) ||
            (c >= 0xF900 && c <= 0xFAFF))
            return Han;
        if (c >= 0x3040 && c <= 0x309F) return Kana;
        if ((c >= 0x30A0 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) ||
            (c >= 0xFF66 && c <= 0xFF9D))
            return Kata;
        if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF))
            return Hangul;
        if (c < 0x80 && !((c >= U'0' && c <= U'9') ||
                          (c >= U'A' && c <= U'Z') ||
                          (c >= U'a' && c <= U'z') || c == U'\'' || c == U'_'))
            return Sep;                                // ASCII punctuation
        return Word;   // Latin, fullwidth alnum, Cyrillic, Greek, ...
    };

    const auto& touched = (*gs)[static_cast<std::size_t>(glyph)];
    const Cls hit = classOf(touched.code);
    if (hit == Sep) {
        // A long-press directly on a trailing comma should not collapse to
        // a non-selection. Include the attached word AND its punctuation;
        // only do this when the previous glyph is spatially adjacent, never
        // across a paragraph break or an unrelated column.
        if ((touched.code == U',' || touched.code == U'\u060c' ||
             touched.code == U'\uff0c') && glyph > 0) {
            const auto& prev = (*gs)[static_cast<std::size_t>(glyph - 1)];
            const Cls before = classOf(prev.code);
            if (before != Sep && prev.r > prev.l && prev.b > prev.t &&
                touched.l <= prev.r + std::max(.012f, prev.b - prev.t) &&
                std::fabs((prev.t + prev.b - touched.t - touched.b) * .5f) <=
                    (prev.b - prev.t)) {
                int a = glyph - 1;
                while (a > 0 && classOf((*gs)[static_cast<std::size_t>(a - 1)].code)
                                       == before) --a;
                selA_ = TextPos{page, a};
                selB_ = TextPos{page, glyph};
                return;
            }
        }
        selA_ = selB_ = TextPos{page, glyph};
        return;
    }
    const auto same = [&](int i) {
        return classOf((*gs)[static_cast<std::size_t>(i)].code) == hit;
    };
    int a = glyph, b = glyph;
    while (a > 0 && same(a - 1)) --a;
    while (b < last && same(b + 1)) ++b;
    selA_ = TextPos{page, a};
    selB_ = TextPos{page, b};
}

int PageStage::glyphAt(int page, Point n, bool nearbyOnly) {
    if (!source_) return -1;
    const auto* gs = source_->glyphs(page);
    if (!gs || gs->empty()) return -1;

    const auto drawable = [](const PageGlyph& g) {
        return std::isfinite(g.l) && std::isfinite(g.r) &&
               std::isfinite(g.t) && std::isfinite(g.b) &&
               g.r > g.l && g.b > g.t &&
               g.l >= 0.f && g.r <= 1.f &&
               g.t >= 0.f && g.b <= 1.f;
    };
    // Do not hit synthetic whitespace/newline separators (zero-area boxes)
    // or invalid OCR geometry. They still belong in the text stream to stop
    // long-press word selection at real word and paragraph boundaries.
    for (std::size_t i = 0; i < gs->size(); ++i) {
        const auto& g = (*gs)[i];
        if (drawable(g) && n.x >= g.l && n.x <= g.r &&
            n.y >= g.t && n.y <= g.b)
            return static_cast<int>(i);
    }
    // Otherwise the closest glyph, weighting vertical distance heavily so a
    // drag that wanders past the end of a line stays on that line rather
    // than jumping to whatever is horizontally nearest two lines down.
    int best = -1;
    float bestD = 1e9f;
    for (std::size_t i = 0; i < gs->size(); ++i) {
        const auto& g = (*gs)[i];
        if (!drawable(g)) continue;
        const float cy = (g.t + g.b) * 0.5f;
        const float dy = std::fabs(n.y - cy);
        const float dx = n.x < g.l ? g.l - n.x : (n.x > g.r ? n.x - g.r : 0.f);
        if (nearbyOnly && (dy > std::max(.012f, (g.b - g.t) * 1.5f) ||
                           dx > std::max(.020f, (g.b - g.t) * 3.f)))
            continue;
        const float d = dy * 8.f + dx;
        if (d < bestD) { bestD = d; best = static_cast<int>(i); }
    }
    return best;
}

void PageStage::selectionLines(int page, int from, int to,
                               std::vector<Rect>& out) const {
    if (!source_) return;
    const auto* gs = source_->glyphs(page);
    if (!gs || gs->empty()) return;
    from = std::max(0, from);
    to = std::min(to, static_cast<int>(gs->size()) - 1);
    if (from > to) return;

    // Work in page-normalised coordinates, which markup and the animated
    // overlay both use. Never use the growing rectangle as the line test:
    // tightly led lines can overlap its loose tolerance, then each new line
    // increases its height/tolerance again and the entire paragraph becomes
    // one giant band. Lock classification to the FIRST body glyph's baseline
    // and height, and treat a return to the left edge as a new line too.
    bool open = false;
    bool hasBodyGlyph = false;
    float l = 0, t = 0, r = 0, b = 0;
    float inkTop = 0, inkBottom = 0;
    float baseline = 0, bodyH = 0;
    const std::size_t firstOut = out.size();
    std::vector<std::pair<float, float>> bodyExtents;
    // Commas and periods often have boxes entirely below the midpoint of
    // the letters beside them. They belong to the same *text line*, but
    // must not increase that line's vertical extent: at minimum zoom such
    // an outlier would make adjacent amber bands overlap.
    const auto punctuation = [](char32_t c) {
        return c == U',' || c == U'.' || c == U';' || c == U':' ||
               c == U'!' || c == U'?' || c == U'\u060c' || c == U'\uff0c';
    };
    auto flush = [&] {
        if (!open) return;
        const float top = std::min(t, inkTop);
        const float bottom = std::max(b, inkBottom);
        out.push_back(Rect{l, top, r - l, bottom - top});
        bodyExtents.emplace_back(t, b);
        open = false;
    };
    for (int i = from; i <= to; ++i) {
        const auto& g = (*gs)[static_cast<std::size_t>(i)];
        if (g.code == U'\n' || g.code == U'\r') { flush(); continue; }
        if (!std::isfinite(g.l) || !std::isfinite(g.t) ||
            !std::isfinite(g.r) || !std::isfinite(g.b) ||
            g.r <= g.l || g.b <= g.t || g.l < 0.f || g.r > 1.f ||
            g.t < 0.f || g.b > 1.f) continue;
        const bool isPunctuation = punctuation(g.code);
        const float glyphH = g.b - g.t;
        const float centre = (g.t + g.b) * 0.5f;
        // Protect columns AND left-edge returns. The previous rule only
        // checked distance from the *start* of a line; returning to the
        // same indent therefore passed and joined the next paragraph row.
        const float maxGap = std::clamp(bodyH * 3.0f, 0.020f, 0.075f);
        const float maxBack = std::max(0.012f, bodyH * 1.5f);
        const bool horizontal = open && g.l >= r - maxBack &&
                                g.l <= r + maxGap;
        // Commas have tiny boxes well below the letter centres. Permit that
        // vertical offset ONLY for punctuation (or a body glyph after an
        // initial comma). Ordinary letters stay anchored to a fixed line
        // centre, so a tight next line can never enlarge the acceptance zone.
        const float limit = (!hasBodyGlyph || isPunctuation)
            ? std::max(bodyH, glyphH) * 0.95f
            : bodyH * 0.65f + glyphH * 0.25f;
        // PDFium's glyph boxes can change height and vertical origin at a
        // font/word boundary (italic headings are a common example). Require
        // ink overlap for letters, rather than splitting a single text row
        // into a box for every word. The fixed baseline still rejects the
        // next densely led row, even when the horizontal gap is small.
        const bool overlapsRow = g.t < b - bodyH * .08f &&
                                 g.b > t + bodyH * .08f;
        const bool sameLine = horizontal &&
            (std::fabs(centre - baseline) <= limit ||
             (!isPunctuation && overlapsRow &&
              std::fabs(centre - baseline) <= bodyH * 1.05f));
        if (!sameLine) {
            flush();
            l = g.l; t = g.t; r = g.r; b = g.b;
            inkTop = g.t; inkBottom = g.b;
            baseline = centre;
            bodyH = glyphH;
            hasBodyGlyph = !isPunctuation;
            open = true;
        } else {
            r = std::max(r, g.r);
            if (!isPunctuation && !hasBodyGlyph) {
                // A selection beginning at the comma takes the next letter's
                // baseline; the short punctuation box cannot define a line.
                t = g.t; b = g.b;
                baseline = centre;
                bodyH = glyphH;
            } else if (!isPunctuation || !hasBodyGlyph) {
                // Only glyphs belonging to this fixed baseline contribute
                // height. A low comma still widens the band but does not
                // push its bottom into the next line.
                t = std::min(t, g.t);
                b = std::max(b, g.b);
            }
            hasBodyGlyph = hasBodyGlyph || !isPunctuation;
            // Keep the comma itself under the amber wash, not just its x
            // position. Cap a malformed punctuation box against the fixed
            // body height so it cannot swallow the neighbouring paragraph.
            inkTop = std::min(inkTop,
                              std::max(g.t, baseline - bodyH * 1.3f));
            inkBottom = std::max(inkBottom,
                                 std::min(g.b, baseline + bodyH * 1.3f));
        }
    }
    flush();
    // A descender may physically intrude into the next row on tightly led
    // PDFs. Split any overlapping adjacent washes at the BODY glyph gap;
    // never tint the next line merely to cover a punctuation tail.
    for (std::size_t j = 1; j < bodyExtents.size(); ++j) {
        Rect& prev = out[firstOut + j - 1];
        Rect& next = out[firstOut + j];
        if (prev.x >= next.right() || next.x >= prev.right() ||
            next.y <= prev.y || prev.bottom() <= next.y) continue;
        const float cutoff = std::clamp(
            (bodyExtents[j - 1].second + bodyExtents[j].first) * 0.5f,
            prev.y, prev.bottom());
        const float nextBottom = next.bottom();
        prev.h = std::max(0.f, cutoff - prev.y);
        if (next.y < cutoff) {
            next.y = cutoff;
            next.h = std::max(0.f, nextBottom - cutoff);
        }
    }
}

void PageStage::selectionRects(int page, int from, int to,
                               std::vector<Rect>& out) {
    const Rect* pr = nullptr;
    for (const auto& p : placed_)
        if (p.index == page) { pr = &p.rect; break; }
    if (!pr) return;
    std::vector<Rect> norm;
    selectionLines(page, from, to, norm);
    const Rect unrotated = sourceRect(*pr);
    for (const Rect& n : norm)
        out.push_back(Rect{unrotated.x + n.x * unrotated.w,
                           unrotated.y + n.y * unrotated.h,
                           n.w * unrotated.w, n.h * unrotated.h});
}

bool PageStage::selectionHandlePoint(bool start, Point& out) const {
    if (!hasSelection() || !source_) return false;
    TextPos first = selA_, last = selB_;
    if (last < first) std::swap(first, last);
    const int page = start ? first.page : last.page;
    const auto* gs = source_->glyphs(page);
    if (!gs || gs->empty()) return false;
    const int from = page == first.page ? first.glyph : 0;
    const int to = page == last.page ? last.glyph
                                     : static_cast<int>(gs->size()) - 1;
    std::vector<Rect> lines;
    selectionLines(page, from, to, lines);
    if (lines.empty()) return false; // e.g. hidden/cropped OCR-only text
    const auto it = std::find_if(placed_.begin(), placed_.end(),
                                 [page](const Placed& p) { return p.index == page; });
    if (it == placed_.end()) return false;
    const Rect& box = start ? lines.front() : lines.back();
    const Point n = toViewNorm({start ? box.x : box.right(),
                                start ? box.y : box.bottom()});
    out.x = it->rect.x + n.x * it->rect.w +
            (start ? -kSelectionGripOffset : kSelectionGripOffset);
    out.y = it->rect.y + n.y * it->rect.h +
            (start ? -kSelectionGripOffset : kSelectionGripOffset);
    return std::isfinite(out.x) && std::isfinite(out.y);
}

PageStage::SelectionHandle PageStage::hitSelectionHandle(Point local,
                                                         float radius) const {
    if (!hasSelection() || editTool_ != EditTool::None) return SelectionHandle::None;
    SelectionHandle closest = SelectionHandle::None;
    float best = radius * radius;
    for (const bool start : {true, false}) {
        Point p{};
        if (!selectionHandlePoint(start, p)) continue;
        const float x = p.x - scrollX_;
        const float y = p.y - scrollY_ + overscrollY_;
        if (x < -radius || x > frame.w + radius ||
            y < -radius || y > frame.h + radius) continue;
        const float dx = local.x - x, dy = local.y - y;
        const float d = dx * dx + dy * dy;
        if (d <= best) { best = d; closest = start ? SelectionHandle::Start
                                                   : SelectionHandle::End; }
    }
    return closest;
}

void PageStage::paintSelectionHandles(IPaintTarget& t,
                                      const PaintCtx& ctx) const {
    if (!hasSelection() || editTool_ != EditTool::None) return;
    const SelectionInk ink = selectionInk(pageTheme_);
    for (const bool start : {true, false}) {
        Point p{};
        if (!selectionHandlePoint(start, p)) continue;
        const float x = p.x - scrollX_;
        const float y = p.y - scrollY_ + overscrollY_;
        // Never paint an orphaned dot over the toolbar/another page when its
        // line scrolls away. The 44dp hit area is virtual, not a visible disk.
        if (x < 1.f || x > frame.w - 1.f ||
            y < 1.f || y > frame.h - 1.f) continue;
        t.fillCircle(p, 9.f, ink.halo);
        t.fillCircle(p, 6.2f, ink.handle);
    }
}

void PageStage::paintSelectionBands(IPaintTarget& t, const Rect& page,
                                    const std::vector<Rect>& rects) const {
    if (rects.empty()) return;
    const SelectionInk ink = selectionInk(pageTheme_);
    t.save();
    t.clipRoundedRect(page, Corners::all(2.f), true);
    for (std::size_t ri = 0; ri < rects.size(); ++ri) {
        const Rect& sr = rects[ri];
        if (sr.w <= 0.f || sr.h <= 0.f) continue;
        // Padding scales with the glyphs, never a fixed-height pill that
        // runs into the neighbouring text line at low zoom.
        const float yPad = std::min(2.5f, sr.h * 0.10f);
        const float xPad = std::min(1.25f, sr.h * 0.05f);
        float padTop = yPad, padBottom = yPad;
        float gapTop = 1e6f, gapBottom = 1e6f;
        const auto horizontalOverlap = [&](const Rect& other) {
            return sr.x < other.right() && other.x < sr.right();
        };
        if (ri > 0 && horizontalOverlap(rects[ri - 1]) &&
            rects[ri - 1].bottom() <= sr.y) {
            gapTop = sr.y - rects[ri - 1].bottom();
            padTop = std::min(padTop, gapTop * 0.32f);
        }
        if (ri + 1 < rects.size() && horizontalOverlap(rects[ri + 1]) &&
            sr.bottom() <= rects[ri + 1].y) {
            gapBottom = rects[ri + 1].y - sr.bottom();
            padBottom = std::min(padBottom, gapBottom * 0.32f);
        }
        const Rect band{sr.x - xPad, sr.y - padTop,
                        sr.w + xPad * 2.f, sr.h + padTop + padBottom};
        const float radius = std::min(2.5f,
                                       std::min(band.w, band.h) * 0.18f);
        const Corners corners = Corners::all(radius);
        t.save();
        t.clipRoundedRect(band, corners, true);
        // PDF pixels already contain the authentic font raster. Re-drawing
        // extracted Unicode on top would be wrong for embedded/rotated or
        // scanned text. Multiply leaves black ink black; Screen leaves
        // white (inverted dark-page) ink white. Both tint the PAPER rather
        // than fogging the glyphs with ordinary alpha-over.
        t.setBlendMode(pageTheme_ == PageTheme::Dark
                           ? folio::render::BlendMode::Screen
                           : folio::render::BlendMode::Multiply);
        t.fillRectVerticalGradient(band, ink.top, ink.bottom);
        // IPaintTarget's blend setting is not scoped by save/restore. Reset
        // explicitly before the fine border and every subsequent page blit.
        t.setBlendMode(folio::render::BlendMode::SrcOver);
        t.restore();
        t.strokeRoundedRect(band, corners, ink.edge,
            std::min({0.85f, sr.h * 0.06f,
                      std::min(gapTop, gapBottom) * 0.20f}));
    }
    t.restore();
}

void PageStage::paintSelectionMagnifier(IPaintTarget& t, const PaintCtx& ctx,
                                        float deviceScale, int page,
                                        const std::vector<Rect>& lines) const {
    if (!magnifierEnabled_ || !source_ || frame.w < 115.f ||
        frame.h < 115.f || touch_.count() != 1) return;
    const Annotation* draft = store_ ? store_->draft() : nullptr;
    const bool marker = highlightMagnifierActive_ && authoring_ &&
        editTool_ == EditTool::Highlight && draft &&
        draft->kind == AnnotKind::Highlight && draft->page == authorPage_;
    const bool textSelection = !marker && magnifierActive_ &&
        (editTool_ == EditTool::None || thumbTextSelecting_) && hasSelection();
    if (!marker && !textSelection) return;
    const PointerId pointer = marker ? highlightPointer_ : magnifierPointer_;
    // No floating bubble after a lost pointer/cancelled capture. The drawing
    // tool itself retains its original event ownership and hit testing.
    const auto finger = std::find_if(touch_.pointers().begin(), touch_.pointers().end(),
        [pointer](const ActivePointer& p) { return p.id == pointer; });
    if (finger == touch_.pointers().end()) return;
    if (marker) page = authorPage_;
    if (page < 0) return;
    const auto it = std::find_if(placed_.begin(), placed_.end(),
        [page](const Placed& p) { return p.index == page; });
    if (it == placed_.end()) return;
    const Rect dest{it->rect.x - scrollX_, it->rect.y - scrollY_ + overscrollY_,
                    it->rect.w, it->rect.h};
    if (dest.empty()) return;
    // The drawing cursor is the clamped active stroke endpoint. Its Y never
    // follows vertical finger wobble; it stays on the initial marker line.
    const Point cursor = toViewNorm({highlightCursor_.x, authorStart_.y});
    const Point point = marker
        ? Point{dest.x + cursor.x * dest.w, dest.y + cursor.y * dest.h}
        : magnifierFinger_;
    constexpr float magnification = 2.f;
    constexpr float margin = 5.f;
    constexpr float fingerGap = 18.f;
    // The bubble follows the CURSOR, not the touch. In the first 60dp of a
    // page there is no room above it; dock the marker preview to its side
    // instead of hiding it on touch-down. Text selection keeps its original
    // above-the-finger rule and never moves into a nearby glyph line.
    float radius = std::min(52.f, (point.y - fingerGap - margin) * .5f);
    const bool sideDocked = marker && radius < 20.f;
    if (sideDocked) radius = 36.f;
    if (radius < 20.f) return;
    const float cx = sideDocked
        ? std::clamp(point.x + (point.x < frame.w * .5f
                                   ? radius + 26.f : -radius - 26.f),
                     radius + margin, frame.w - radius - margin)
        : std::clamp(point.x, radius + margin, frame.w - radius - margin);
    const float cy = sideDocked ? radius + margin
                                : point.y - radius - fingerGap;
    if (cy < radius + margin || cy > frame.h - radius - margin) return;
    const Point focus{std::clamp(point.x, dest.x, dest.right()),
                      std::clamp(point.y, dest.y, dest.bottom())};
    const float half = radius / magnification;
    const Rect sample{focus.x - half, focus.y - half, half * 2.f, half * 2.f};
    const Rect unrotated = sourceRect(dest);
    const auto unrotate = [&](float x, float y) {
        const Point n = toSourceNorm({(x - dest.x) / std::max(1.f, dest.w),
                                      (y - dest.y) / std::max(1.f, dest.h)});
        return Point{unrotated.x + n.x * unrotated.w,
                     unrotated.y + n.y * unrotated.h};
    };
    const Point q[] = {unrotate(sample.x, sample.y),
                       unrotate(sample.right(), sample.y),
                       unrotate(sample.x, sample.bottom()),
                       unrotate(sample.right(), sample.bottom())};
    float left = q[0].x, right = left, top = q[0].y, bottom = top;
    for (const Point& v : q) {
        left = std::min(left, v.x); right = std::max(right, v.x);
        top = std::min(top, v.y); bottom = std::max(bottom, v.y);
    }
    const float x0 = std::max(unrotated.x, left);
    const float y0 = std::max(unrotated.y, top);
    const float x1 = std::min(unrotated.right(), right);
    const float y1 = std::min(unrotated.bottom(), bottom);
    if (x1 <= x0 || y1 <= y0) return;
    const Rect rim{cx - radius, cy - radius, radius * 2.f, radius * 2.f};
    const auto corners = Corners::all(radius);
    t.save();
    t.clipRoundedRect(rim, corners, true);
    t.translate(cx - magnification * focus.x,
                cy - magnification * focus.y);
    t.scale(magnification, magnification);
    const auto turnPage = [&](Point centre) {
        t.translate(centre.x, centre.y);
        t.rotateDegrees(static_cast<float>(rotation_) * 90.f);
        t.translate(-centre.x, -centre.y);
    };
    t.save();
    turnPage({dest.x + dest.w * .5f, dest.y + dest.h * .5f});
    const bool drawn = source_->drawCachedRegion(t, page, unrotated,
        Rect{x0, y0, x1 - x0, y1 - y0}, deviceScale, pageTheme_);
    t.restore();
    if (drawn && (marker || !lines.empty())) {
        // The cached PDF and live overlays get the identical page rotation.
        t.save();
        t.translate(-scrollX_, -scrollY_ + overscrollY_);
        turnPage({it->rect.x + it->rect.w * .5f,
                  it->rect.y + it->rect.h * .5f});
        const Rect content = sourceRect(it->rect);
        t.clipRect(Rect{x0 + scrollX_, y0 + scrollY_ - overscrollY_,
                        x1 - x0, y1 - y0});
        if (marker) {
            if (!draft->empty())
                drawAnnotation(t, *draft, content, 1.f,
                               pageTheme_ == PageTheme::Dark);
            Annotation tip = *draft;
            const float w = std::min(.08f, std::max(.003f, 12.f / content.w));
            const float x = highlightCursor_.x;
            const bool advancing = x >= authorStart_.x;
            float l = draft->empty() ? x - w * .5f : (advancing ? x : x - w);
            l = std::clamp(l, 0.f, 1.f - w);
            const float bandH = highlightBandHeight(annotWidth_);
            tip.box = {l, authorStart_.y - bandH * .5f, w, bandH};
            drawAnnotation(t, tip, content, 1.f,
                           pageTheme_ == PageTheme::Dark);
            const float guideX = content.x + x * content.w;
            const float guideY = content.y + authorStart_.y * content.h;
            const Color guide = selectionInk(pageTheme_).handle;
            t.drawLine({guideX, guideY - 3.f}, {guideX, guideY + 3.f},
                       guide, .75f);
        } else {
            paintSelectionBands(t, content, lines);
        }
        t.restore();
    }
    t.restore();
    if (!drawn) return; // never show an empty bubble during first-page loading
    const SelectionInk ink = selectionInk(pageTheme_);
    t.strokeRoundedRect(rim.inset(0.5f), corners, ink.halo, 2.f);
    t.strokeRoundedRect(rim.inset(1.f), Corners::all(radius - 1.f),
                        ink.handle, 1.15f);
    // A very small, passive reference point makes it clear which glyph is
    // being moved without painting an opaque disk over the enlarged letters.
    t.drawLine({cx - 3.f, cy}, {cx - 1.f, cy}, ink.handle, 1.f);
    t.drawLine({cx + 1.f, cy}, {cx + 3.f, cy}, ink.handle, 1.f);
}

void PageStage::selectionQuads(std::vector<std::pair<int, Rect>>& out) const {
    if (!hasSelection() || !source_) return;
    TextPos a = selA_, bb = selB_;
    if (bb < a) std::swap(a, bb);
    for (int p = a.page; p <= bb.page; ++p) {
        const auto* gs = source_->glyphs(p);
        if (!gs) continue;
        const int from = p == a.page ? a.glyph : 0;
        const int to = p == bb.page ? bb.glyph : static_cast<int>(gs->size()) - 1;
        std::vector<Rect> lines;
        selectionLines(p, from, to, lines);
        for (const Rect& r : lines) out.emplace_back(p, r);
    }
}

std::string PageStage::selectedText() {
    if (!hasSelection() || !source_) return {};
    TextPos a = selA_, bb = selB_;
    if (bb < a) std::swap(a, bb);
    std::string out;
    for (int p = a.page; p <= bb.page; ++p) {
        const auto* gs = source_->glyphs(p);
        if (!gs) continue;
        const int from = p == a.page ? a.glyph : 0;
        const int to = p == bb.page ? bb.glyph : static_cast<int>(gs->size()) - 1;
        for (int i = std::max(0, from);
             i <= std::min(to, static_cast<int>(gs->size()) - 1); ++i)
            appendUtf8(out, (*gs)[static_cast<std::size_t>(i)].code);
        if (p != bb.page) out.push_back('\n');
    }
    return out;
}

void PageStage::selectAll() {
    if (!source_ || source_->pageCount() <= 0) return;
    const int last = source_->pageCount() - 1;
    const auto* gs = source_->glyphs(last);
    selA_ = TextPos{0, 0};
    selB_ = TextPos{last, gs ? static_cast<int>(gs->size()) - 1 : 0};
}


// On known blank paper a still finger opens tools at 500 ms. A press on
// actual text takes the existing selection path instead, at the same time.
constexpr double kThumbHoldSeconds = .5;

// The top arc is deliberately above the finger. The horizontal corridor at
// finger height belongs only to zoom; ordinary pan never enters this mode.
Rect PageStage::thumbItem(int i, ThumbLayer layer) const {
    const float x = thumbMenuX_, y = thumbMenuY_;
    if (layer == ThumbLayer::Tools) {
        // Tighter arc with unchanged 44dp finger targets. Artwork is 24dp.
        static constexpr float xx[4] = {-66.f, -22.f, 22.f, 66.f};
        static constexpr float yy[4] = {-100.f, -112.f, -112.f, -100.f};
        if (i < 0 || i >= 4) return {};
        return {x + xx[i] - 22.f, y + yy[i] - 22.f, 44.f, 44.f};
    }
    if (layer == ThumbLayer::Options) {
        if (i < 0 || i >= 4) return {};
        return {x + (static_cast<float>(i) - 1.5f) * 46.f - 22.f, y - 56.f, 44.f, 44.f};
    }
    if (layer == ThumbLayer::Values) {
        const int count = thumbOption_ == 0 ? 24 : thumbOption_ == 1 ? 6 :
                          thumbOption_ == 2 ? 4 : 5;
        if (i < 0 || i >= count) return {};
        if (thumbOption_ == 0)
            return {x + (static_cast<float>(i % 6) - 2.5f) * 44.f - 20.f,
                    y - 97.f - static_cast<float>(i / 6) * 41.f - 20.f, 40.f, 40.f};
        return {x + (static_cast<float>(i) - static_cast<float>(count - 1) * .5f) * 46.f - 21.f,
                y - 96.f - 21.f, 42.f, 42.f};
    }
    return {};
}

bool PageStage::thumbOptionAvailable(int option) const noexcept {
    if (option < 0 || option >= 4 || thumbTool_ == 0) return false;
    if (thumbTool_ == 3) return option == 1; // an eraser has size, not ink
    return option != 3 || thumbTool_ == 2;  // shapes belong to the pen
}

void PageStage::dismissThumbIfOutside(Point windowPoint, double now) {
    if (!thumbAwaitingTap_ || thumbClosing_) return;
    const Point origin = toWindow({0.f, 0.f});
    const Point p{windowPoint.x - origin.x, windowPoint.y - origin.y};
    const float x = thumbMenuX_, y = thumbMenuY_;
    const bool onTools = thumbOption_ < 0 &&
        Rect{x - 99.f, y - 150.f, 198.f, 83.f}.contains(p.x, p.y);
    const bool onOptions = thumbTool_ > 0 &&
        Rect{x - 104.f, y - 62.f, 208.f, 58.f}.contains(p.x, p.y);
    const int count = thumbOption_ == 1 ? 6 : thumbOption_ == 2 ? 4 : 5;
    const float width = thumbOption_ == 0 ? 270.f : count * 46.f + 18.f;
    const bool onValues = thumbOption_ >= 0 &&
        Rect{x - width * .5f, y - (thumbOption_ == 0 ? 249.f : 126.f),
             width, thumbOption_ == 0 ? 183.f : 60.f}.contains(p.x, p.y);
    if (!onTools && !onOptions && !onValues) closeThumb(now);
}

void PageStage::closeThumb(double now) {
    if (thumbLayer_ == ThumbLayer::Closed || thumbClosing_) return;
    if (thumbLayer_ == ThumbLayer::Zoom) {
        thumbLayer_ = ThumbLayer::Closed;
        return;
    }
    thumbAwaitingTap_ = thumbGestureFromPinned_ = thumbToolApplied_ = false;
    thumbClosing_ = true;
    thumbExit_.reset(1.f);
    thumbExit_.animateTo(0.f, tokens::dur::hoverFast, now, Curve::Standard);
}

void PageStage::beginThumb(double now, bool pinned) {
    thumbCandidate_ = false;
    thumbLayer_ = ThumbLayer::Tools;
    thumbMenuX_ = std::clamp(thumbDown_.pos.x, 134.f,
                             std::max(134.f, frame.w - 134.f));
    // The third-tier ink grid needs roughly 250dp above the menu. On a
    // landscape phone (or a hold near the top), pin the geometry within the
    // available viewport without changing the actual pointer origin. A
    // user can finish from this persistent menu with individual taps.
    const float lowest = std::max(0.f, frame.h - 8.f);
    thumbMenuY_ = std::clamp(thumbDown_.pos.y, std::min(254.f, lowest),
                              std::max(254.f, lowest));
    thumbTool_ = thumbOption_ = thumbHover_ = -1;
    thumbGrow_.reset(0.f);
    thumbAwaitingTap_ = pinned;
    thumbGestureFromPinned_ = thumbToolApplied_ = thumbClosing_ = false;
    thumbToolsExit_.reset(1.f);
    thumbExit_.reset(1.f);
    thumbOptionsReveal_.reset(0.f);
    thumbValuesReveal_.reset(0.f);
    thumbReveal_.reset(0.f);
    thumbReveal_.animateTo(1.f, tokens::dur::popoverReveal,
                           now, Curve::OutBack);
}

bool PageStage::handleThumb(const PointerEvent& e) {
    if ((!e.isTouch() && !e.isPen() && thumbLayer_ == ThumbLayer::Closed) ||
        thumbReplay_) return false;
    if (e.phase == PointerPhase::Cancel && e.pointerId == 0 &&
        (thumbCandidate_ || thumbTextSelecting_ ||
         thumbLayer_ != ThumbLayer::Closed)) {
        if (thumbLayer_ != ThumbLayer::Closed) closeThumb(e.timestamp);
        thumbCandidate_ = thumbDeferred_ = thumbTextSelecting_ =
            thumbAwaitingTap_ = thumbGestureFromPinned_ = thumbToolApplied_ = false;
        magnifierActive_ = selecting_ = false;
        if (authoring_ && highlightTouchControlled_ && store_) {
            store_->cancelDraft();
            authoring_ = touchDrawing_ = false;
            highlightTouchControlled_ = highlightStrokeLocked_ = false;
        }
        touch_.clear();
        return true;
    }
    if (thumbTextSelecting_) {
        if (e.pointerId != thumbDown_.pointerId) {
            if (e.phase == PointerPhase::Down) {
                thumbTextSelecting_ = selecting_ = magnifierActive_ = false;
                return false; // let the existing pinch take over
            }
            return true;
        }
        touch_.onPointer(e);
        if (e.phase == PointerPhase::Move) {
            int page = -1;
            Point norm{};
            if (hitPage(e.pos, &page, &norm)) {
                const int g = glyphAt(page, norm);
                if (g >= 0) selB_ = TextPos{page, g};
            }
            magnifierFinger_ = e.pos;
        } else if (e.phase == PointerPhase::Up || e.phase == PointerPhase::Cancel) {
            thumbTextSelecting_ = selecting_ = magnifierActive_ = false;
            if (e.phase == PointerPhase::Up && hasSelection() && onSelectionCommitted)
                onSelectionCommitted(e.windowPos);
        }
        return true;
    }
    if (thumbClosing_ && thumbLayer_ != ThumbLayer::Closed &&
        e.phase != PointerPhase::Down && !thumbCandidate_) {
        // Keep painting the reverse reveal, but don't block the page. A new
        // Down may already seed its next hold while this image fades out.
        return false;
    }
    if (thumbLayer_ != ThumbLayer::Closed && !thumbClosing_) {
        if (thumbAwaitingTap_) {
            if (e.phase != PointerPhase::Down) return e.isTouch() || e.isPen();
            // Every tier survives a lift. A fresh finger can choose a tool,
            // property or value; an outside Down dismisses and passes through
            // to the ordinary page gesture (including a new zoom/pinch).
            if (thumbOption_ < 0) {
                for (int i = 0; i < 4; ++i) {
                    if (!thumbItem(i, ThumbLayer::Tools).contains(e.pos.x, e.pos.y))
                        continue;
                    const bool changed = thumbTool_ != i;
                    thumbDown_.pointerId = e.pointerId; // preserve popup origin
                    thumbAwaitingTap_ = false;
                    thumbGestureFromPinned_ = true;
                    thumbLayer_ = ThumbLayer::Tools;
                    thumbTool_ = i;
                    thumbHover_ = i;
                    if (changed) {
                        thumbToolApplied_ = false;
                        thumbOptionsReveal_.reset(0.f);
                        if (i > 0) thumbOptionsReveal_.animateTo(
                            1.f, tokens::dur::popoverReveal,
                            e.timestamp, Curve::OutBack);
                    }
                    return true;
                }
            }
            for (int i = 0; i < 4; ++i) {
                if (!thumbOptionAvailable(i) ||
                    !thumbItem(i, ThumbLayer::Options).contains(e.pos.x, e.pos.y))
                    continue;
                const bool changed = thumbOption_ != i;
                thumbDown_.pointerId = e.pointerId; // keep original menu origin
                thumbAwaitingTap_ = false;
                thumbGestureFromPinned_ = true;
                thumbOption_ = i;
                thumbHover_ = -1;
                thumbLayer_ = ThumbLayer::Options;
                if (changed) {
                    thumbValuesReveal_.reset(0.f);
                    thumbValuesReveal_.animateTo(1.f, tokens::dur::popoverReveal,
                                                e.timestamp, Curve::OutBack);
                }
                return true;
            }
            if (thumbOption_ >= 0) {
                const int count = thumbOption_ == 0 ? 24 : thumbOption_ == 1 ? 6 :
                                  thumbOption_ == 2 ? 4 : 5;
                for (int i = 0; i < count; ++i) {
                    if (!thumbItem(i, ThumbLayer::Values).inset(-2.f)
                             .contains(e.pos.x, e.pos.y)) continue;
                    thumbDown_.pointerId = e.pointerId;
                    thumbAwaitingTap_ = false;
                    thumbGestureFromPinned_ = true;
                    thumbLayer_ = ThumbLayer::Values;
                    thumbHover_ = i;
                    return true;
                }
            }
            closeThumb(e.timestamp);
            return false;
        }
        if (e.phase == PointerPhase::Down && e.pointerId != thumbDown_.pointerId) {
            // The second finger always wins. Seed the first one if an armed
            // pen had deferred its Down; the ordinary pinch path cancels it.
            const bool deferred = thumbDeferred_;
            closeThumb(e.timestamp);
            thumbCandidate_ = thumbDeferred_ = false;
            if (deferred) {
                thumbReplay_ = true;
                onPointer(thumbDown_);
                thumbReplay_ = false;
            }
            return false;
        }
        if (e.pointerId != thumbDown_.pointerId && e.pointerId != 0) return true;
        if (e.phase == PointerPhase::Move) {
            thumbLast_ = e.pos;
            const float dx = e.pos.x - thumbDown_.pos.x;
            const float dy = e.pos.y - thumbDown_.pos.y;
            if (thumbLayer_ == ThumbLayer::Tools && thumbTool_ < 0 &&
                std::fabs(dx) > 30.f && std::fabs(dy) < 24.f) {
                thumbLayer_ = ThumbLayer::Zoom;
                thumbZoomStart_ = effectiveScale(e.timestamp);
            }
            if (thumbLayer_ == ThumbLayer::Zoom) {
                const float next = std::clamp(thumbZoomStart_ * std::exp(dx * .007f),
                                               kMinZoom, kMaxZoom);
                if (std::fabs(next - zoomTarget_) > .0001f) {
                    setZoomFocus(thumbDown_.pos.x, thumbDown_.pos.y);
                    captureZoomAnchor();
                    zoomMode_ = ZoomMode::Free;
                    zoomTarget_ = next;
                    zoomAnim_.reset(next);
                    relayout(e.timestamp);
                    applyZoomAnchor();
                    anchorValid_ = false;
                }
                return true;
            }
            // Keep the selected tool arc on screen while its inner option
            // row is revealed. The old menu replaced the tools only AFTER a
            // down-then-up detour, so the second layer was undiscoverable.
            if (thumbLayer_ == ThumbLayer::Tools) {
                if (thumbTool_ > 0) {
                    for (int i = 0; i < 4; ++i) {
                        if (thumbOptionAvailable(i) &&
                            thumbItem(i, ThumbLayer::Options).contains(e.pos.x, e.pos.y)) {
                            thumbOption_ = i;
                            thumbHover_ = -1;
                            thumbLayer_ = ThumbLayer::Options;
                            thumbGrow_.reset(.55f);
                            thumbGrow_.animateTo(1.f, tokens::dur::hoverFast,
                                                 e.timestamp, Curve::OutBack);
                            thumbValuesReveal_.reset(0.f);
                            thumbValuesReveal_.animateTo(
                                1.f, tokens::dur::popoverReveal, e.timestamp,
                                Curve::OutBack);
                            return true;
                        }
                    }
                }
                int tool = -1;
                for (int i = 0; i < 4; ++i) {
                    if (thumbItem(i, ThumbLayer::Tools).contains(e.pos.x, e.pos.y)) {
                        tool = i;
                        break;
                    }
                }
                if (tool != thumbTool_) {
                    thumbTool_ = tool < 0 ? thumbTool_ : tool;
                    thumbHover_ = tool;
                    if (tool >= 0) {
                        thumbGrow_.reset(.55f); // immediate finger feedback
                        thumbGrow_.animateTo(1.f, tokens::dur::hoverFast,
                                             e.timestamp, Curve::OutBack);
                        thumbOptionsReveal_.reset(0.f);
                        if (tool != 0) thumbOptionsReveal_.animateTo(
                            1.f, tokens::dur::popoverReveal, e.timestamp,
                            Curve::OutBack);
                    }
                }
                return true;
            }
            // The option row remains visible WITH the value palette. Sliding
            // back down changes category without dismissing the gesture.
            for (int i = 0; i < 4; ++i) {
                if (thumbOptionAvailable(i) &&
                    thumbItem(i, ThumbLayer::Options).contains(e.pos.x, e.pos.y)) {
                    if (thumbOption_ != i) {
                        thumbOption_ = i;
                        thumbGrow_.reset(.55f);
                        thumbGrow_.animateTo(1.f, tokens::dur::hoverFast,
                                             e.timestamp, Curve::OutBack);
                        thumbValuesReveal_.reset(0.f);
                        thumbValuesReveal_.animateTo(
                            1.f, tokens::dur::popoverReveal, e.timestamp,
                            Curve::OutBack);
                    }
                    thumbLayer_ = ThumbLayer::Options;
                    thumbHover_ = -1;
                    return true;
                }
            }
            if (thumbOption_ < 0) return true;
            if (thumbLayer_ == ThumbLayer::Options && dy >= -82.f) return true;
            thumbLayer_ = ThumbLayer::Values;
            const int count = thumbOption_ == 0 ? 24 : thumbOption_ == 1 ? 6 :
                              thumbOption_ == 2 ? 4 : 5;
            int hit = -1;
            for (int i = 0; i < count; ++i) {
                const Rect r = thumbItem(i, ThumbLayer::Values);
                // 44dp targets touch without overlapping a neighbouring ink.
                if (r.inset(-2.f).contains(e.pos.x, e.pos.y)) {
                    hit = i;
                    break;
                }
            }
            if (hit != thumbHover_) {
                thumbHover_ = hit;
                if (hit >= 0) {
                    thumbGrow_.reset(.55f);
                    thumbGrow_.animateTo(1.f, tokens::dur::hoverFast,
                                         e.timestamp, Curve::OutBack);
                }
            }
            return true;
        }
        if (e.phase == PointerPhase::Up || e.phase == PointerPhase::Cancel) {
            const bool commit = e.phase == PointerPhase::Up && thumbTool_ >= 0 &&
                                thumbLayer_ != ThumbLayer::Zoom;
            const int tool = thumbTool_;
            const int option = thumbLayer_ == ThumbLayer::Values && thumbHover_ >= 0
                                   ? thumbOption_ : -1;
            const int value = option >= 0 ? thumbHover_ : -1;
            if (!thumbDeferred_ && touch_.count() > 0) touch_.onPointer(e);
            thumbCandidate_ = thumbDeferred_ = false;
            touchPanning_ = touchDrawing_ = false;
            touchAxis_ = TouchAxis::Undecided;
            if (e.phase == PointerPhase::Cancel || thumbLayer_ == ThumbLayer::Zoom) {
                closeThumb(e.timestamp);
                return true;
            }
            if (tool >= 0 && !thumbToolApplied_) {
                if (onThumbChoice) onThumbChoice(tool, option, value);
                thumbToolApplied_ = true;
            } else if (commit && option >= 0 && onThumbChoice) {
                onThumbChoice(tool, option, value);
            }
            // No next tier follows Hand. Likewise an uninterrupted sweep
            // that finishes at a value is already complete: reverse-reveal
            // on release. A separate tap into a previously pinned popup is
            // incremental editing, so keep property + value rows available.
            if (commit && (tool == 0 ||
                           (option >= 0 && !thumbGestureFromPinned_))) {
                closeThumb(e.timestamp);
                return true;
            }
            thumbAwaitingTap_ = true;
            if (option >= 0) thumbLayer_ = ThumbLayer::Values;
            else thumbLayer_ = tool > 0 ? ThumbLayer::Options : ThumbLayer::Tools;
            return true;
        }
        return true;
    }
    if (thumbCandidate_ && e.phase == PointerPhase::Down &&
        e.pointerId != thumbDown_.pointerId) {
        thumbCandidate_ = false;
        if (thumbDeferred_) {
            thumbDeferred_ = false;
            thumbReplay_ = true;
            onPointer(thumbDown_);
            thumbReplay_ = false;
        }
        return false; // normal two-finger pinch wins
    }
    if (thumbCandidate_ && e.pointerId == thumbDown_.pointerId) {
        if (e.phase == PointerPhase::Move) {
            thumbLast_ = e.pos;
            if (std::hypot(e.pos.x - thumbDown_.pos.x,
                           e.pos.y - thumbDown_.pos.y) > gesture::kSlop) {
                thumbCandidate_ = false;
                if (thumbDeferred_) {
                    thumbDeferred_ = false;
                    thumbReplay_ = true;
                    onPointer(thumbDown_);
                    onPointer(e);
                    thumbReplay_ = false;
                    return true;
                }
            } else if (thumbDeferred_) return true;
        } else if (e.phase == PointerPhase::Up || e.phase == PointerPhase::Cancel) {
            thumbCandidate_ = false;
            if (thumbDeferred_) {
                thumbDeferred_ = false;
                if (e.phase == PointerPhase::Up && thumbOnText_) {
                    // Text wins over a stationary ink dot, even when an
                    // annotation tool is armed; the popup is released only
                    // after the finger has left the page.
                    selectWordAt(thumbTextPage_, thumbTextGlyph_);
                    if (hasSelection() && onSelectionCommitted)
                        onSelectionCommitted(e.windowPos);
                } else if (e.phase == PointerPhase::Up &&
                           e.timestamp - thumbDown_.timestamp < kThumbHoldSeconds) {
                    thumbReplay_ = true;
                    onPointer(thumbDown_);
                    onPointer(e);
                    thumbReplay_ = false;
                }
                return true;
            }
        } else if (e.phase == PointerPhase::Down && e.pointerId != thumbDown_.pointerId) {
            thumbCandidate_ = false;
            if (thumbDeferred_) {
                thumbDeferred_ = false;
                thumbReplay_ = true;
                onPointer(thumbDown_);
                thumbReplay_ = false;
            }
        }
    }
    if (e.phase != PointerPhase::Down || thumbCandidate_ || touch_.count() != 0 ||
        !source_ || layout_ == LayoutMode::Grid)
        return false;
    int pg = -1;
    Point norm{};
    if (!hitPage(e.pos, &pg, &norm)) return false;
    // Never guess that not-yet-extracted text is blank: its word-selection
    // gesture must win when PDFium eventually supplies the glyphs.
    if (!source_->glyphs(pg) ||
        (store_ && store_->hitTest(pg, norm, .025f)) ||
        hitAnnotGrip(e.pos, true) >= 0 ||
        (hasSelection() && hitSelectionHandle(e.pos, 22.f) != SelectionHandle::None))
        return false;
    const int glyph = glyphAt(pg, norm, true);
    if (glyph >= 0 && editTool_ == EditTool::None) return false;
    // An upward-only palette needs room; text selection does not.
    if (glyph < 0 && frame.w < 285.f) return false;
    thumbDown_ = e;
    thumbLast_ = e.pos;
    thumbCandidate_ = true;
    thumbOnText_ = glyph >= 0;
    thumbTextPage_ = pg;
    thumbTextGlyph_ = glyph;
    thumbDeferred_ = editTool_ != EditTool::None &&
                     (thumbOnText_ || editTool_ != EditTool::Highlight);
    return thumbDeferred_;
}

void PageStage::paintThumb(IPaintTarget& t, const PaintCtx& ctx) {
    if (thumbLayer_ == ThumbLayer::Closed) return;
    if (thumbLayer_ == ThumbLayer::Zoom) {
        t.drawTextInBox(std::to_string(static_cast<int>(zoomTarget_ * 100.f)) + "%",
                        {thumbMenuX_ - 38.f, thumbMenuY_ - 90.f, 76.f, 34.f},
                        styleFor(tokens::type_label, ctx.pal.text),
                        folio::render::TextAlign::Centre);
        return;
    }

    const Point anchor{thumbDown_.pos.x, thumbMenuY_};
    const float closing = std::clamp(thumbExit_.value(ctx.now), 0.f, 1.f);
    // Folio's selection and colour Popovers reveal a stationary card/content
    // through ONE expanding clip from the anchor, fading as they open. Do
    // exactly that per tier: no per-icon saveLayers, scale transforms or 24
    // independently animated swatches. The old GPU path did all three on
    // every PDF frame and was visibly choppy on a phone.
    const auto begin = [&](const AnimatedF& reveal, float exit, float radius) {
        const float p = std::clamp(reveal.value(ctx.now), 0.f, 1.f) *
                        closing * std::clamp(exit, 0.f, 1.f);
        if (p <= .004f) return false;
        t.save();
        const float r = radius * p;
        t.clipRoundedRect({anchor.x - r, anchor.y - r, 2.f * r, 2.f * r},
                          Corners::all(r), true);
        t.saveLayerAlpha(std::min(1.f, p * 2.2f));
        return true;
    };
    const auto end = [&] { t.restore(); t.restore(); };
    const auto inkIcon = [&](const char* icon, Rect r, bool active) {
        // Keep a full 44dp hit target but paint a restrained 24dp glyph,
        // matching the selection popup's compact ghost buttons.
        const float grow = active ? 1.f + .10f * thumbGrow_.value(ctx.now) : 1.f;
        if (grow > 1.f) r = r.inset(-(grow - 1.f) * r.w * .5f);
        drawIcon(t, icon, r.inset(10.f),
                 active ? ctx.pal.accent : ctx.pal.text, tokens::icon::strokeWidth);
    };

    // The tall colour grid occupies the upper slot: tools remain pinned
    // after release, but yield that slot while the third tier is open.
    const bool showTools = thumbOption_ < 0;
    static constexpr const char* tools[] = {"hand", "highlighter", "pen", "eraser"};
    if (showTools && begin(thumbReveal_, thumbToolsExit_.value(ctx.now), 210.f)) {
        // Single arched card follows the tool centres; no outlined disk
        // around any item. Opaque card colour is the same as Popover::onPaint.
        using V = folio::render::PathVerb;
        static const std::vector<V> arc = {
            {V::Op::MoveTo, -99.f, -101.f},
            {V::Op::CubicTo, -99.f, -122.f, -87.f, -134.f, -66.f, -134.f},
            {V::Op::CubicTo, -45.f, -134.f, -45.f, -150.f, -22.f, -150.f},
            {V::Op::CubicTo, 0.f, -150.f, 0.f, -150.f, 22.f, -150.f},
            {V::Op::CubicTo, 45.f, -150.f, 45.f, -134.f, 66.f, -134.f},
            {V::Op::CubicTo, 87.f, -134.f, 99.f, -122.f, 99.f, -101.f},
            {V::Op::CubicTo, 99.f, -80.f, 88.f, -67.f, 66.f, -67.f},
            {V::Op::CubicTo, 45.f, -67.f, 46.f, -79.f, 22.f, -79.f},
            {V::Op::CubicTo, 0.f, -80.f, 0.f, -80.f, -22.f, -79.f},
            {V::Op::CubicTo, -46.f, -79.f, -45.f, -67.f, -66.f, -67.f},
            {V::Op::CubicTo, -88.f, -67.f, -99.f, -80.f, -99.f, -101.f},
            {V::Op::Close},
        };
        t.save();
        t.translate(thumbMenuX_, anchor.y);
        t.fillPath(arc, ctx.pal.card);
        t.restore();
        for (int i = 0; i < 4; ++i)
            inkIcon(tools[i], thumbItem(i, ThumbLayer::Tools), thumbTool_ == i);
        end();
    }

    if (thumbTool_ > 0 && begin(thumbOptionsReveal_, 1.f, 180.f)) {
        // The two cards have a real gap; the settings row no longer covers
        // the lower edge of the first-tier tool arc.
        const Rect panel{thumbMenuX_ - 104.f, anchor.y - 62.f, 208.f, 58.f};
        drawCard(t, panel, Corners::all(tokens::radius::popover),
                 ctx.pal.card, tokens::elevation::card(ctx.pal));
        static constexpr const char* options[] = {
            "highlighter", "stroke-weight", "opacity", "shapes"};
        for (int i = 0; i < 4; ++i) {
            if (!thumbOptionAvailable(i)) continue;
            const Rect r = thumbItem(i, ThumbLayer::Options);
            const bool active = thumbOption_ == i;
            if (i == 0) {
                const Point centre{r.x + r.w * .5f, r.y + r.h * .5f};
                for (int dot = 0; dot < 3; ++dot) {
                    const Color c = thumbInk ? thumbInk(dot) : ctx.pal.accent;
                    t.fillCircle({centre.x + (static_cast<float>(dot) - 1.f) * 9.f,
                                  centre.y}, active ? 5.5f : 4.5f, c);
                }
            } else {
                inkIcon(options[i], r, active);
            }
        }
        end();
    }

    if (thumbOption_ < 0 ||
        !begin(thumbValuesReveal_, 1.f, thumbOption_ == 0 ? 330.f : 210.f))
        return;
    const int count = thumbOption_ == 0 ? 24 : thumbOption_ == 1 ? 6 :
                      thumbOption_ == 2 ? 4 : 5;
    const bool inks = thumbOption_ == 0;
    const float valueWidth = inks ? 270.f : static_cast<float>(count) * 46.f + 18.f;
    const Rect panel{thumbMenuX_ - valueWidth * .5f,
                     anchor.y - (inks ? 249.f : 126.f), valueWidth,
                     inks ? 183.f : 60.f};
    drawCard(t, panel, Corners::all(tokens::radius::popover),
             ctx.pal.card, tokens::elevation::card(ctx.pal));
    static constexpr const char* shapes[] = {
        "pen", "shape-rect", "shape-oval", "shape-line", "shape-arrow"};
    for (int i = 0; i < count; ++i) {
        const Rect r = thumbItem(i, ThumbLayer::Values);
        const bool active = thumbLayer_ == ThumbLayer::Values && thumbHover_ == i;
        const Point centre{r.x + r.w * .5f, r.y + r.h * .5f};
        if (thumbOption_ == 0) {
            const Color c = thumbInk ? thumbInk(i) : ctx.pal.accent;
            if (c.r > 225 && c.g > 225 && c.b > 225)
                t.fillCircle(centre, active ? 19.f : 17.f, ctx.pal.border);
            t.fillCircle(centre, active ? 17.f : 15.f, c);
        } else if (thumbOption_ == 1) {
            const Color c = active ? ctx.pal.accent : ctx.pal.text;
            t.drawLine({r.x + 7.f, centre.y - 3.f},
                       {r.right() - 7.f, centre.y - 3.f}, c,
                       1.f + static_cast<float>(i) * 1.25f);
            t.drawTextInBox(std::to_string(i + 1),
                            {r.x, centre.y + 4.f, r.w, 12.f},
                            styleFor(tokens::type_small, c),
                            folio::render::TextAlign::Centre);
        } else if (thumbOption_ == 2) {
            t.drawTextInBox(std::to_string((i + 1) * 25) + "%", r,
                            styleFor(tokens::type_small,
                                     active ? ctx.pal.accent : ctx.pal.text),
                            folio::render::TextAlign::Centre);
        } else {
            inkIcon(shapes[i], r, active);
        }
    }
    end();
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------

bool PageStage::handleTouch(const PointerEvent& e) {
    // A mouse keeps every behaviour it already had, bit for bit. Touch is
    // additive here, not a rewrite of a working input path.
    if (e.pointerType == PointerType::Mouse) return false;

    // A pen is a precision instrument and falls through to the ordinary
    // paths so it can draw and select exactly like a mouse. All we take
    // from it is nib pressure.
    if (e.pointerType == PointerType::Pen) {
        if (e.phase == PointerPhase::Down) { penPressSum_ = 0.f; penPressN_ = 0; }
        if (e.phase == PointerPhase::Down || e.phase == PointerPhase::Move) {
            penPressSum_ += e.pressure;
            ++penPressN_;
        }
        return false;
    }

    touch_.onPointer(e);
    const std::size_t n = touch_.count();

    switch (e.phase) {
        case PointerPhase::Down:
            // Landing a finger stops a glide dead, the way grabbing a
            // spinning wheel does. Nothing is more annoying than a page
            // that keeps sliding under the finger trying to stop it.
            smoothing_ = false;
            velocity_ = 0.f;
            overscrollVelocity_ = 0.f;
            if (n == 1) {
                pinchWasActive_ = false;
                touchAxis_ = TouchAxis::Undecided;
            }
            if (n >= 2 && touchAnnotId_) {
                PointerEvent cancel = e;
                cancel.phase = PointerPhase::Cancel;
                handleAnnotSelection(cancel);
                touchAnnotId_ = 0;
            }
            if (n >= 2) {
                overscrollY_ = 0.f;
                touchAxis_ = TouchAxis::Undecided;
                pinchWasActive_ = true;
                touchDrawing_ = false;
                // Finish a button/double-tap zoom at the *visible* scale
                // before a finger-driven zoom takes over its timeline.
                pinchZoom_ = effectiveScale(e.timestamp);
                zoomAnim_.reset(pinchZoom_);
                relayout(e.timestamp);
            }
            // In Select/Read mode an existing mark takes priority over page
            // panning; armed Ink/Shape/Eraser tools must retain their own
            // one-finger authoring gesture.
            if (n == 1 && store_ && editTool_ == EditTool::None) {
                if (handleAnnotSelection(e)) {
                    touchAnnotId_ = selAnnot_;
                    touchAnnotPointer_ = e.pointerId;
                    touchPanning_ = false;
                    return true;
                }
            }
            // One finger with a tool active is a drawing stroke: let the
            // authoring path have it.
            if (n == 1 && editTool_ != EditTool::None) {
                touchDrawing_ = true;
                touchPanning_ = false;
                // A stroke can start only on paper; an off-page touch with a
                // tool armed must not fall through to the mouse pan path.
                return hitPage(e.pos, nullptr, nullptr) ? false : true;
            }
            // A second finger landing mid-stroke means the user wanted to
            // zoom, not to draw. Throw the half-drawn stroke away instead
            // of committing a stray mark when that finger lifts.
            if (n >= 2 && authoring_) {
                store_->cancelDraft();
                authoring_ = false;
                highlightTouchControlled_ = highlightStrokeLocked_ = false;
            }
            touchPanning_ = (n >= 1);
            return true;

        case PointerPhase::Move: {
            if (touchAnnotId_) {
                if (e.pointerId == touchAnnotPointer_ && n == 1)
                    handleAnnotSelection(e);
                return true;
            }
            if (n >= 2) {
                // Two fingers: scale about the midpoint and carry the page
                // along with it, so the content under the fingers stays
                // under the fingers.
                const PinchState p = touch_.pinch();
                const Point before{p.centroid.x - p.centroidDelta.x,
                                   p.centroid.y - p.centroidDelta.y};
                const float next = std::clamp(pinchZoom_ * p.scale,
                                              kMinZoom, kMaxZoom);
                if (std::fabs(next - pinchZoom_) > 1e-5f) {
                    // Capture the document point UNDER the *old* midpoint,
                    // relayout at the new scale, then put that same point
                    // under the *new* midpoint. Applying an anchor from a
                    // prior wheel gesture (or before relayout) was the
                    // source of pinch zooming toward an unrelated location.
                    setZoomFocus(before.x, before.y);
                    captureZoomAnchor();
                    pinchZoom_ = zoomTarget_ = next;
                    zoomMode_ = ZoomMode::Free;
                    zoomAnim_.reset(next);
                    relayout(e.timestamp);
                    setZoomFocus(p.centroid.x, p.centroid.y);
                    applyZoomAnchor();
                    anchorValid_ = false;
                } else {
                    // Two-finger translation without scale is just a pan;
                    // don't rebuild every page's geometry for it.
                    // A vertical two-finger scroll must not jitter sideways.
                    // Deliberate horizontal two-finger travel is still
                    // available to read a magnified wide page.
                    if (canScrollX() &&
                        std::fabs(p.centroidDelta.x) >
                            1.35f * std::fabs(p.centroidDelta.y))
                        scrollX_ -= p.centroidDelta.x;
                    scrollY_ -= p.centroidDelta.y;
                    clampScroll();
                }
                updateCurrentFromScroll();
                return true;
            }
            if (touchDrawing_ && n == 1)
                return authoring_ ? false : true; // never turn a stroke into a page pan
            if (!touchPanning_) return false;
            if (!touch_.pointers().empty() && !touch_.pointers().front().moved)
                return true; // Android-style touch slop before starting a drag
            const Point d = touch_.panDelta();
            if (touchAxis_ == TouchAxis::Undecided && !touch_.pointers().empty()) {
                const auto& finger = touch_.pointers().front();
                const float dx = finger.pos.x - finger.start.x;
                const float dy = finger.pos.y - finger.start.y;
                // Lock a scroll vertically; an unmistakably sideways drag on
                // a zoomed/wide page must still move it horizontally.
                touchAxis_ = canScrollX() && std::fabs(dx) > 1.4f * std::fabs(dy)
                    ? TouchAxis::Horizontal : TouchAxis::Vertical;
            }
            if (touchAxis_ == TouchAxis::Horizontal) {
                scrollX_ -= d.x;
                clampScroll();
            } else {
                if (overscrollY_ != 0.f) {
                    const float before = overscrollY_;
                    overscrollY_ = std::clamp(before + d.y * 0.42f,
                                              -kOverscrollLimit, kOverscrollLimit);
                    if (before * overscrollY_ < 0.f) overscrollY_ = 0.f;
                } else {
                    const float wanted = scrollY_ - d.y;
                    scrollY_ = wanted;
                    clampScroll();
                    overscrollY_ = std::clamp((scrollY_ - wanted) * 0.42f,
                                              -kOverscrollLimit, kOverscrollLimit);
                }
                updateCurrentFromScroll();
            }
            return true;
        }

        case PointerPhase::Up:
        case PointerPhase::Cancel: {
            if (touchAnnotId_) {
                if (e.pointerId == touchAnnotPointer_) {
                    handleAnnotSelection(e);
                    touchAnnotId_ = 0;
                    touchPanning_ = false;
                    touchAxis_ = TouchAxis::Undecided;
                }
                return true;
            }
            const bool wasPanning = touchPanning_;
            const bool wasPinch = pinchWasActive_;
            if (touch_.count() == 0) {
                touchPanning_ = false;
                pinchWasActive_ = false;
            } else if (wasPinch) {
                touchAxis_ = TouchAxis::Undecided;
            }
            if (touchDrawing_) {
                touchDrawing_ = false;
                if (e.phase == PointerPhase::Cancel) {
                    if (store_) store_->cancelDraft();
                    authoring_ = false;
                    highlightTouchControlled_ = highlightStrokeLocked_ = false;
                    return true;
                }
                return authoring_ ? false : true; // commit through handleAuthoring
            }

            // A tap that never left slop is a press as far as the page is
            // concerned. The early return above used to swallow it, so the
            // steps every mouse press takes never ran: the selection and its
            // markup popup stayed up, and annotations could not be selected.
            // A long press has already made its own selection and must not be
            // undone by the release, hence the `selecting_` guard.
            if (e.phase == PointerPhase::Up && wasPanning && !wasPinch &&
                touch_.count() == 0 && touch_.wasTap() && !selecting_) {
                PointerEvent press = e;
                press.pos = press.windowPos = touch_.tapPos();
                press.phase = PointerPhase::Down;
                if (handleAnnotSelection(press)) {
                    press.phase = PointerPhase::Up;
                    handleAnnotSelection(press);
                }
            }

            // Only a released, unmoved outside tap deselects. A hold and
            // page pan leaves both the text range and selected mark intact.
            if (e.phase == PointerPhase::Up && !wasPinch && touch_.count() == 0 &&
                touch_.wasTap() && !selecting_) {
                const Point tap = touch_.tapPos();
                if (pendingAnnotClear_ && selAnnot_) {
                    selAnnot_ = 0;
                    if (onAnnotSelectionChanged) onAnnotSelectionChanged(0, true);
                }
                if (hasSelection() && !selectionContains(tap)) {
                    clearSelection();
                    if (onSelectionCleared) onSelectionCleared();
                }
            }
            pendingAnnotClear_ = false;

            // Double tap toggles between fitting the page and a readable
            // 2x at the point tapped - the gesture every mobile reader has.
            // ...but not while a tool is armed: there, two quick taps are
            // two deliberate dots, and zooming instead would destroy work.
            if (!wasPinch && touch_.wasTap() && touch_.tapCount() >= 2 &&
                editTool_ == EditTool::None) {
                const Point tp = touch_.tapPos();
                // In the page grid a double tap means "open this page", as a
                // double click does with a mouse. Zooming a thumbnail is not
                // a useful result.
                if (layout_ == LayoutMode::Grid) {
                    int gp = 0;
                    Point gn{0.f, 0.f};
                    if (hitPage(tp, &gp, &gn) && onPageActivated) {
                        onPageActivated(gp);
                        return true;
                    }
                }
                const float fit = fitScale(true);
                // Strictly two states, so a second double tap always undoes
                // the first. Ratcheting (x2 each time) leaves the reader
                // somewhere they did not ask to be.
                if (zoomTarget_ > fit * 1.2f) {
                    setZoomMode(ZoomMode::FitWidth, e.timestamp);
                } else {
                    setZoomFocus(tp.x, tp.y);
                    setZoom(fit * 2.f, e.timestamp);
                }
                return true;
            }

            // A single plain tap toggles immersive reading chrome. Run only
            // after the double-tap branch so a quick zoom still zooms; never
            // trigger from a long-press selection or an armed drawing tool.
            if (e.phase == PointerPhase::Up && !wasPinch && !selecting_ &&
                editTool_ == EditTool::None && touch_.count() == 0 &&
                touch_.wasTap() && touch_.tapCount() == 1 &&
                hitPage(touch_.tapPos(), nullptr, nullptr) && onCanvasTap)
                onCanvasTap();

            const Point v = touch_.flingVelocity();
            if (!wasPinch && touchAxis_ == TouchAxis::Vertical &&
                std::fabs(overscrollY_) < 0.5f && std::fabs(v.y) > 1.f) {
                velocity_ = std::clamp(-v.y,
                                       -gesture::kFlingMax, gesture::kFlingMax);
                smoothing_ = false;
            }
            if (touch_.count() == 0) touchAxis_ = TouchAxis::Undecided;
            return wasPanning;
        }

        default:
            return false;
    }
}

bool PageStage::onPointer(const PointerEvent& e) {
    // Give a finger on the wider touch target exclusive ownership of the
    // bar until it lifts; don't feed its motion to the pan/selection tracker.
    if (e.isTouch() && highlightMagnifierActive_ &&
        ((e.phase == PointerPhase::Up || e.phase == PointerPhase::Cancel) &&
         (e.pointerId == highlightPointer_ ||
          (e.phase == PointerPhase::Cancel && e.pointerId == 0)) ||
         (e.phase == PointerPhase::Down && e.pointerId != highlightPointer_)))
        highlightMagnifierActive_ = false;
    if (e.isTouch() && e.phase == PointerPhase::Cancel &&
        (e.pointerId == 0 || e.pointerId == magnifierPointer_)) {
        magnifierActive_ = false;
        selecting_ = false;
    }
    // A desktop right-click opens the same pinned, multi-tier tool menu as
    // a stationary touch hold. No mobile breakpoint or Android #ifdef: XI2,
    // Windows touchscreens, pens and mice share the PDF stage behaviour.
    if (e.phase == PointerPhase::Down && e.button == MouseButton::Right &&
        !e.isTouch() && source_ && layout_ != LayoutMode::Grid) {
        int page = -1; Point norm{};
        if (hitPage(e.pos, &page, &norm)) {
            if (thumbLayer_ != ThumbLayer::Closed) {
                thumbLayer_ = ThumbLayer::Closed;
                thumbClosing_ = false;
            }
            thumbDown_ = e;
            thumbLast_ = e.pos;
            beginThumb(e.timestamp, true);
            return true;
        }
    }
    if (thumbLayer_ != ThumbLayer::Closed && !e.isTouch() &&
        e.button == MouseButton::Right && e.phase == PointerPhase::Up)
        return true; // right-button release does not select or pan the page
    if ((e.isTouch() || e.isPen() || thumbLayer_ != ThumbLayer::Closed) &&
        handleThumb(e)) return true;
    if (e.isTouch()) {
        if (barTouchHeld_) {
            if (e.pointerId != barTouchId_) return true;
            const bool done = e.phase == PointerPhase::Up ||
                              e.phase == PointerPhase::Cancel;
            handleScrollbar(e);
            if (done) barTouchHeld_ = false;
            return true;
        }
        if (e.phase == PointerPhase::Down && handleScrollbar(e)) {
            barTouchHeld_ = true;
            barTouchId_ = e.pointerId;
            return true;
        }
    }

    if (handleThumb(e)) return true;

    // Selection grips own their pointer before the page-pan, annotation and
    // long-press paths. The drawn dots remain ~12dp, but a finger has a 44dp
    // invisible target. A second finger cancels an unfinished adjustment and
    // passes into the ordinary anchored pinch rather than moving text.
    bool passingToPinch = false;
    if (activeSelectionHandle_ != SelectionHandle::None) {
        if (e.phase == PointerPhase::Cancel && e.pointerId == 0) {
            // Window deactivation can cancel every pointer with one event,
            // not one Cancel per finger. Do not leave a captured grip alive.
            selA_ = selectionBeforeA_;
            selB_ = selectionBeforeB_;
            activeSelectionHandle_ = SelectionHandle::None;
            touch_.clear();
            touchPanning_ = false;
            magnifierActive_ = false;
            return true;
        }
        if (e.phase == PointerPhase::Down &&
            e.pointerId != selectionPointer_) {
            selA_ = selectionBeforeA_;
            selB_ = selectionBeforeB_;
            activeSelectionHandle_ = SelectionHandle::None;
            magnifierActive_ = false;
            passingToPinch = true;
            // The original touch is already in touch_, so handleTouch now
            // sees both contacts and starts its normal two-finger gesture.
        } else if (e.pointerId == selectionPointer_) {
            if (e.isTouch()) touch_.onPointer(e);
            if (e.isTouch() && e.phase == PointerPhase::Move && magnifierActive_)
                magnifierFinger_ = e.pos;
            if (e.phase == PointerPhase::Move) {
                int page = -1;
                Point norm{};
                if (hitPage(e.pos, &page, &norm)) {
                    const int glyph = glyphAt(page, norm);
                    if (glyph >= 0) {
                        const TextPos moved{page, glyph};
                        if (activeSelectionHandle_ == SelectionHandle::Start) {
                            if (selB_ < moved) {
                                selA_ = selB_; selB_ = moved;
                                activeSelectionHandle_ = SelectionHandle::End;
                            } else {
                                selA_ = moved;
                            }
                        } else if (moved < selA_) {
                            selB_ = selA_; selA_ = moved;
                            activeSelectionHandle_ = SelectionHandle::Start;
                        } else {
                            selB_ = moved;
                        }
                    }
                }
            } else if (e.phase == PointerPhase::Cancel) {
                selA_ = selectionBeforeA_;
                selB_ = selectionBeforeB_;
                activeSelectionHandle_ = SelectionHandle::None;
                magnifierActive_ = false;
            } else if (e.phase == PointerPhase::Up) {
                activeSelectionHandle_ = SelectionHandle::None;
                magnifierActive_ = false;
                if (hasSelection() && onSelectionCommitted)
                    onSelectionCommitted(e.windowPos);
            }
            return true;
        }
    }
    if (!passingToPinch && e.phase == PointerPhase::Down && hasSelection() &&
        editTool_ == EditTool::None) {
        const SelectionHandle hit = hitSelectionHandle(
            e.pos, e.isTouch() ? 22.f : 10.f);
        if (hit != SelectionHandle::None) {
            TextPos first = selA_, last = selB_;
            if (last < first) std::swap(first, last);
            selectionBeforeA_ = selA_;
            selectionBeforeB_ = selB_;
            selA_ = first;
            selB_ = last;
            activeSelectionHandle_ = hit;
            selectionPointer_ = e.pointerId;
            selecting_ = false;
            touchPanning_ = false;
            if (e.isTouch()) {
                touch_.onPointer(e);
                if (magnifierEnabled_) {
                    magnifierActive_ = true;
                    magnifierPointer_ = e.pointerId;
                    magnifierFinger_ = e.pos;
                }
            }
            if (onSelectionCleared) onSelectionCleared(); // popup only; keep text
            return true;
        }
    }
    if (e.isTouch() && magnifierActive_ && e.pointerId == magnifierPointer_) {
        if (e.phase == PointerPhase::Move) magnifierFinger_ = e.pos;
        if (e.phase == PointerPhase::Up || e.phase == PointerPhase::Cancel)
            magnifierActive_ = false;
    }
    if (e.isTouch() && e.phase == PointerPhase::Down && magnifierActive_ &&
        e.pointerId != magnifierPointer_) {
        magnifierActive_ = false; // a pinch never inherits a one-finger loupe
        selecting_ = false;
    }
    if (handleTouch(e)) return true;

    // Mouse scrollbars still precede page link/annotation hit testing.
    if (!e.isTouch() && handleScrollbar(e)) return true;
    if (handleAuthoring(e)) return true;
    if (handleAnnotSelection(e)) return true;

    switch (e.phase) {
        case PointerPhase::Wheel: {
            if (e.modifiers & ModCtrl) {
                // Ctrl+wheel zooms about the pointer, the universal gesture.
                // This used to claim it did so while anchoring to the fixed
                // centre of the viewport, which on a zoomed-in page sends
                // whatever you were pointing at off the edge.
                setZoomFocus(e.pos.x, e.pos.y);
                const float factor = std::exp(e.wheelY * 0.12f);
                setZoom(zoomTarget_ * factor, e.timestamp);
            } else if ((e.modifiers & ModShift) || std::fabs(e.wheelX) > 0.01f) {
                // Shift+wheel is the universal horizontal scroll, and a
                // trackpad or tilt wheel sends wheelX directly. Only useful
                // once the page is wider than the viewport.
                const float dx = std::fabs(e.wheelX) > 0.01f ? e.wheelX : e.wheelY;
                scrollX_ -= dx * 64.f;
                clampScroll();
            } else {
                // Ease towards the target rather than teleporting 64px per
                // notch. Consecutive notches extend the same glide, which is
                // what makes a wheel feel continuous instead of steppy; a
                // trackpad sends many small deltas and gets the same path.
                constexpr int kGlideMs = 190;
                const float base = smoothing_ ? smoothTarget_ : scrollY_;
                const float maxS = std::max(0.f, contentH_ - frame.h);
                smoothTarget_ = std::clamp(base - e.wheelY * 64.f, 0.f, maxS);
                smoothScroll_.reset(scrollY_);
                smoothScroll_.animateTo(smoothTarget_, kGlideMs, e.timestamp);
                smoothing_ = true;
                velocity_ = 0.f;
                updateCurrentFromScroll();
            }
            return true;
        }
        case PointerPhase::Down: {
            // Grid is a page picker, not a reading view: the pages are
            // thumbnails and there is nothing there to select or pan. A
            // double click means "open this one", which is what every file
            // browser does with a tile.
            if (layout_ == LayoutMode::Grid && e.clickCount >= 2) {
                int gp = 0;
                Point gn{0.f, 0.f};
                if (hitPage(e.pos, &gp, &gn) && onPageActivated) {
                    onPageActivated(gp);
                    return true;
                }
            }

            // ANY press discards the current selection, so the chrome that
            // belongs to it comes down first.
            //
            // This used to live further down, in the "pressed empty space"
            // path only. But pressing on *more text* takes the branch below,
            // which collapses the selection to a caret without ever reaching
            // it - so the markup popup stayed on screen pointing at text
            // that was no longer selected. Clicking elsewhere in the
            // document is the ordinary way to deselect, so that was the
            // common case, not the edge case.
            const bool insideSelection = selectionContains(e.pos);
            pendingTextClear_ = hasSelection() && !insideSelection;
            blankDown_ = e.pos;
            if (insideSelection && editTool_ == EditTool::None) {
                // A press inside the existing range is a page grab; the
                // explicit handles change the range. Do not collapse it to
                // one glyph simply because the user began a pan on text.
                dragging_ = true;
                dragLast_ = e.pos;
                return true;
            }

            // With the select tool (the default in Read mode), a press on
            // text starts a selection instead of a pan. A press on empty
            // space still pans, which is what every other reader does.
            int pg = 0;
            Point n{0.f, 0.f};
            if (editTool_ == EditTool::None && hitPage(e.pos, &pg, &n)) {
                const int g = glyphAt(pg, n, true);
                if (g >= 0) {
                    if (hasSelection() && onSelectionCleared) onSelectionCleared();
                    selA_ = selB_ = TextPos{pg, g};
                    selecting_ = true;
                    velocity_ = 0.f;
                    smoothing_ = false;
                    return true;
                }
            }
            // A blank press can become a pan; keep the text range until a
            // short Up confirms the user meant a deselect.
            dragging_ = true;
            dragLast_ = e.pos;
            velocity_ = 0.f;
            smoothing_ = false;
            return true;
        }
        case PointerPhase::Move:
            if (selecting_ && pressed) {
                int pg = 0;
                Point n{0.f, 0.f};
                if (hitPage(e.pos, &pg, &n)) {
                    const int g = glyphAt(pg, n);
                    if (g >= 0) selB_ = TextPos{pg, g};
                }
                return true;
            }
            if (dragging_ && pressed) {
                if (std::fabs(e.pos.x - blankDown_.x) > 7.f ||
                    std::fabs(e.pos.y - blankDown_.y) > 7.f) {
                    pendingTextClear_ = false;
                    pendingAnnotClear_ = false;
                }
                const float dy = e.pos.y - dragLast_.y;
                scrollY_ -= dy;
                if (canScrollX()) scrollX_ -= e.pos.x - dragLast_.x;
                clampScroll();
                dragLast_ = e.pos;
                updateCurrentFromScroll();
                return true;
            }
            return false; // hovering over stationary paper changes no pixels
        case PointerPhase::Up: {
            // A finished selection drag is what summons the markup popup.
            // Fire on release rather than on every move, so the popup does
            // not chase the cursor while the user is still choosing words.
            const bool wasSelecting = selecting_;
            selecting_ = false;
            dragging_ = false;
            if (pendingTextClear_ && hasSelection()) {
                clearSelection();
                if (onSelectionCleared) onSelectionCleared();
            }
            if (pendingAnnotClear_ && selAnnot_) {
                selAnnot_ = 0;
                if (onAnnotSelectionChanged) onAnnotSelectionChanged(0, false);
            }
            pendingTextClear_ = pendingAnnotClear_ = false;
            if (wasSelecting && hasSelection() && onSelectionCommitted)
                onSelectionCommitted(e.windowPos);
            return true;
        }
        default:
            break;
    }
    return false;
}

bool PageStage::onKey(const KeyEvent& e) {
    if (!e.pressed) return false;
    switch (e.key) {
        case Key::PageDown: case Key::Space: nextPage(0.0); return true;
        case Key::PageUp:                    prevPage(0.0); return true;
        case Key::Down:  scrollY_ += 60.f; clampScroll(); updateCurrentFromScroll(); return true;
        case Key::Up:    scrollY_ -= 60.f; clampScroll(); updateCurrentFromScroll(); return true;
        case Key::Left:
            if (canScrollX()) { scrollX_ -= 60.f; clampScroll(); return true; }
            prevPage(now());
            return true;
        case Key::Right:
            if (canScrollX()) { scrollX_ += 60.f; clampScroll(); return true; }
            nextPage(now());
            return true;
        case Key::Home:  goToPage(1, now()); return true;
        case Key::End:   goToPage(source_ ? source_->pageCount() : 1, now()); return true;
        default: break;
    }
    return false;
}

bool PageStage::thumbOverlayOnly(double now) const noexcept {
    if (thumbLayer_ == ThumbLayer::Zoom ||
        (thumbLayer_ == ThumbLayer::Closed && (!thumbCandidate_ || thumbOnText_)) ||
        (thumbClosing_ && (authoring_ || touchPanning_ || touchDrawing_ ||
                           touch_.count() != 0 || barDrag_ != Bar::None)))
        return false;
    if (!source_ || source_->pending() || selecting_ || dragging_ ||
        movingAnnot_ || highlightMagnifierActive_ || magnifierActive_ ||
        zoomAnim_.active(now) || rotationAnim_.active(now) || smoothing_ ||
        std::fabs(velocity_) > kFlingCutoff ||
        std::fabs(overscrollY_) > .1f ||
        std::fabs(overscrollVelocity_) > 1.f)
        return false;
    return std::none_of(entrance_.begin(), entrance_.end(),
                        [now](const Timeline& tl) { return tl.active(now); });
}

Rect PageStage::damageBounds() const {
    if (!thumbOverlayOnly(Widget::now())) return localBounds();
    const float x = thumbCandidate_ ? thumbDown_.pos.x : thumbMenuX_;
    // Include the colour grid, popover elevation and both animation origins.
    // Previous + current damage are joined by FolioApp on each retained frame.
    const float y = thumbCandidate_ ? thumbDown_.pos.y : thumbMenuY_;
    return {x - 150.f, y - 275.f, 300.f, 293.f};
}

bool PageStage::animating(double now) const {
    // A completed async result needs a frame; work still in flight does not
    // need to redraw the same pixels while PDFium is busy.
    if (source_ && source_->pending()) return true;
    if (zoomAnim_.active(now) || rotationAnim_.active(now) || smoothing_) return true;
    // Wait for a possible long press while a finger is still; once it has
    // fired (or the finger moved / an annotation tool owns it), stop the
    // 45 Hz idle repaint. Move events request their own frames.
    if (thumbCandidate_ || (thumbLayer_ != ThumbLayer::Closed &&
        (thumbGrow_.active(now) || thumbReveal_.active(now) ||
         thumbOptionsReveal_.active(now) || thumbValuesReveal_.active(now) ||
         thumbToolsExit_.active(now) || thumbExit_.active(now))))
        return true;
    if (editTool_ == EditTool::None && touch_.longPressPending()) return true;
    if (std::fabs(velocity_) > kFlingCutoff ||
        std::fabs(overscrollY_) > 0.1f ||
        std::fabs(overscrollVelocity_) > 1.f) return true;
    return std::any_of(entrance_.begin(), entrance_.end(),
                       [now](const Timeline& tl) { return tl.active(now); });
}

void PageStage::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    // Armed drawing tools defer their Down on text. A 500ms stationary
    // hold selects the word exactly as Read mode does; moving first replays
    // the original Down and continues the ordinary stroke instead.
    if (thumbCandidate_ && thumbOnText_ && thumbDeferred_ &&
        ctx.now - thumbDown_.timestamp >= gesture::kLongPress) {
        thumbCandidate_ = thumbDeferred_ = false;
        thumbTextSelecting_ = selecting_ = true;
        touchDrawing_ = touchPanning_ = false;
        touch_.onPointer(thumbDown_);
        selectWordAt(thumbTextPage_, thumbTextGlyph_);
        if (magnifierEnabled_) {
            magnifierActive_ = true;
            magnifierPointer_ = thumbDown_.pointerId;
            magnifierFinger_ = thumbDown_.pos;
        }
    }
    // A longer stationary hold on known blank paper owns the radial menu.
    // Text's existing 500ms long-press runs first and cannot be stolen.
    if (thumbCandidate_ && !thumbOnText_ &&
        ctx.now - thumbDown_.timestamp >= kThumbHoldSeconds &&
        std::hypot(thumbLast_.x - thumbDown_.pos.x,
                   thumbLast_.y - thumbDown_.pos.y) <= gesture::kSlop &&
        (thumbDeferred_ || !thumbDown_.isTouch() ||
         (touch_.count() == 1 &&
                            touch_.pointers().front().id == thumbDown_.pointerId))) {
        beginThumb(ctx.now);
        if (authoring_ && highlightTouchControlled_) {
            if (store_) store_->cancelDraft();
            authoring_ = touchDrawing_ = false;
            highlightTouchControlled_ = highlightStrokeLocked_ = false;
            highlightMagnifierActive_ = false;
        }
    }

    // Long press: a finger has no hover, so there is no way to say "select
    // this text" by pointing at it. Holding still is the gesture every
    // mobile reader uses, and it has to be polled because it is defined by
    // the *absence* of events.
    if (editTool_ == EditTool::None && touch_.pollLongPress(ctx.now) &&
        !touch_.pointers().empty()) {
        const Point p = touch_.pointers().front().pos;
        int pg = 0;
        Point n{0.f, 0.f};
        if (hitPage(p, &pg, &n)) {
            const int g = glyphAt(pg, n, true);
            if (g >= 0) {
                // Select the word under the finger and hand over to the
                // ordinary drag-selection path, so the handles and the
                // markup popup behave exactly as they do with a mouse.
                selectWordAt(pg, g);
                selecting_ = true;
                touchPanning_ = false;
                if (magnifierEnabled_) {
                    magnifierActive_ = true;
                    magnifierPointer_ = touch_.pointers().front().id;
                    magnifierFinger_ = p;
                }
                // Commit the popup on release: it must not sit over the
                // loupe or intercept the finger extending this word.
            }
        }
    }

    // Install anything the engine finished since the last frame, before we
    // decide what to draw.
    if (source_) {
        source_->tick();
        source_->setFocusPage(currentPage() - 1);
        if (source_->geometryRevision() != lastGeometryRevision_) {
            auto* mutableStage = const_cast<PageStage*>(this);
            mutableStage->lastGeometryRevision_ = source_->geometryRevision();
            mutableStage->relayout(ctx.now);
        }
    }
    const Rect b = localBounds();
    if (b.empty() || !source_) return;

    auto* self = const_cast<PageStage*>(this);
    const double dt = (lastFrame_ > 0.0) ? std::min(ctx.now - lastFrame_, 0.1) : 0.0;
    self->lastFrame_ = ctx.now;

    // Backdrop: a subtle vertical gradient, which is what stops a large
    // mid-grey area from looking flat and dead.
    t.fillRectVerticalGradient(
        b, ctx.pal.bg,
        over(ctx.pal.bg,
             Color{0, 0, 0,
                   static_cast<std::uint8_t>(std::lround(
                       lerp(12.f, 20.f, tokens::darkness(ctx.pal))))}));

    // A fit mode is a standing promise about the page/viewport relationship,
    // not a one-off calculation. When the viewport changes width - a side
    // panel collapsing, the window resizing, focus mode engaging - the
    // promise has to be re-honoured, or the space the panel gave back just
    // becomes dead grey margin beside a page that stayed the same size.
    if (std::fabs(b.w - lastFitW_) > 0.5f) {
        self->lastFitW_ = b.w;
        if (zoomMode_ != ZoomMode::Free) {
            const float fs = fitScale(zoomMode_ == ZoomMode::FitWidth);
            if (std::fabs(fs - zoomTarget_) > 1e-4f) {
                self->captureZoomAnchor();
                self->zoomTarget_ = fs;
                self->zoomAnim_.animateTo(fs, tokens::dur::pageWidth, ctx.now);
            }
        }
    }

    // Relayout when anything that affects geometry has changed. Comparing
    // against the last values avoids rebuilding the page list every frame of
    // a zoom animation - it rebuilds only while the scale is actually moving.
    const float scale = effectiveScale(ctx.now);

    // Hold the raster scale steady for the duration of a zoom or rotation,
    // then snap it to the real scale when the animation stops. This is the
    // "idle render": expensive work happens when nothing is moving.
    const bool scaleMoving = zoomAnim_.active(ctx.now) || rotationAnim_.active(ctx.now);
    if (!scaleMoving || rasterScale_ <= 0.f) self->rasterScale_ = scale;

    const bool scaleChanged = std::fabs(scale - lastLayoutScale_) > 1e-4f;
    if (b.w != lastLayoutW_ || scaleChanged || placed_.empty()) {
        self->relayout(ctx.now);
        // Put the anchored page back under the viewport centre before
        // clamping, so a zoom step does not slide you onto another page.
        if (scaleChanged) self->applyZoomAnchor();
        self->clampScroll();
    }
    // Once the zoom settles, the anchor has done its job; drop it so a later
    // scroll is not undone by a stale anchor.
    if (!zoomAnim_.active(ctx.now) && anchorValid_) self->anchorValid_ = false;

    if (smoothing_) {
        self->scrollY_ = smoothScroll_.value(ctx.now);
        if (!smoothScroll_.active(ctx.now)) self->smoothing_ = false;
    } else if (std::fabs(velocity_) > kFlingCutoff && dt > 0.0) {
        const float target = scrollY_ + static_cast<float>(velocity_ * dt);
        self->scrollY_ = target;
        self->clampScroll();
        if (std::fabs(target - scrollY_) > 0.01f) {
            // A fling stops at the document edge; convert only a small part
            // of its remaining momentum into a bounded rubber-band bounce.
            self->overscrollVelocity_ = std::clamp(-velocity_ * 0.20f,
                                                   -500.f, 500.f);
            self->velocity_ = 0.f;
        } else {
            self->velocity_ = velocity_ * std::exp(-kFlingFriction * static_cast<float>(dt));
        }
    }
    if (touch_.count() == 0 && dt > 0.0 &&
        (std::fabs(overscrollY_) > 0.01f || std::fabs(overscrollVelocity_) > 0.1f)) {
        // Critically damped spring: position and velocity are integrated in
        // logical pixels; a resting screen costs no extra frames.
        const float step = static_cast<float>(std::min(dt, 0.032));
        self->overscrollVelocity_ += (-144.f * overscrollY_ -
                                      24.f * overscrollVelocity_) * step;
        self->overscrollY_ = std::clamp(overscrollY_ + overscrollVelocity_ * step,
                                        -kOverscrollLimit, kOverscrollLimit);
        if (std::fabs(overscrollY_) < 0.3f &&
            std::fabs(overscrollVelocity_) < 4.f)
            self->overscrollY_ = self->overscrollVelocity_ = 0.f;
    }

    const float rot = rotationAnim_.value(ctx.now);
    const Color paper = paperOf(pageTheme_);

    int loupePage = -1;
    std::vector<Rect> loupeLines;
    if (magnifierEnabled_ && magnifierActive_ && hasSelection()) {
        Point norm{};
        if (!hitPage(magnifierFinger_, &loupePage, &norm)) {
            TextPos first = selA_, last = selB_;
            if (last < first) std::swap(first, last);
            loupePage = activeSelectionHandle_ == SelectionHandle::Start
                ? first.page : last.page;
        }
    }

    t.save();
    t.translate(-scrollX_, -scrollY_ + overscrollY_);

    for (std::size_t i = 0; i < placed_.size(); ++i) {
        const Placed& p = placed_[i];
        // Paint only pages actually on screen; queued work is likewise
        // restricted to the stage viewport, never speculative offscreen pages.
        if (p.rect.bottom() <= scrollY_ || p.rect.y >= scrollY_ + b.h) {
            continue;
        }

        float enterPhase = 1.f;
        if (static_cast<std::size_t>(p.index) < entrance_.size()) {
            enterPhase = entrance_[static_cast<std::size_t>(p.index)].phase(ctx.now);
        }
        if (enterPhase <= 0.004f) continue;

        t.save();
        // Entrance: rise and fade.
        if (enterPhase < 0.999f) {
            t.translate(0.f, lerp(18.f, 0.f, enterPhase));
            t.saveLayerAlpha(enterPhase);
        }

        // Page layout is the *rotated* bounding box. Paint the unrotated
        // PDF (and Folio overlays) in an aspect-correct source rectangle,
        // then rotate the entire composition. The former subtraction of
        // rotation_ * 90 undid the final turn and stretched portrait pages.
        Rect r = p.rect;
        if (std::fabs(std::fmod(rot, 360.f)) > 0.01f) {
            t.translate(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
            t.rotateDegrees(std::fmod(rot, 360.f));
            t.translate(-(r.x + r.w * 0.5f), -(r.y + r.h * 0.5f));
        }

        const bool small = layout_ == LayoutMode::Grid;
        const Corners rr = Corners::all(small ? 4.f : 2.f);
        const auto elev = small ? tokens::elevation::pageSmall(ctx.pal)
                                : tokens::elevation::page(ctx.pal);

        Rect pageRect = sourceRect(r);
        if (small) pageRect.h = pageRect.w * (792.f / 612.f);  // label sits below

        drawElevation(t, pageRect, rr, elev, 1.f, /*interiorOpaque=*/true);

        // Paper + content come out of the raster cache as one bitmap. While
        // the zoom is moving we keep rasterising at the scale we already have
        // and let the blit stretch it: the page grows smoothly with no reflow
        // and no thousand-glyph re-run per frame. The moment the gesture
        // settles, rasterScale_ snaps to the real scale and the next frame
        // re-rasterises once at full crispness.
        const float rs = (rasterScale_ > 0.f) ? rasterScale_ : scale;
        const float ratio = rs / std::max(scale, 0.0001f);
        const int pxW = static_cast<int>(std::lround(pageRect.w * ratio * ctx.dpr));
        const int pxH = static_cast<int>(std::lround(pageRect.h * ratio * ctx.dpr));

        std::uint64_t key = 1469598103934665603ull;
        const auto mix = [&key](std::uint64_t v) {
            for (int i = 0; i < 8; ++i) {
                key ^= (v >> (i * 8)) & 0xFF;
                key *= 1099511628211ull;
            }
        };
        mix(static_cast<std::uint64_t>(p.index));
        mix(static_cast<std::uint64_t>(std::lround(rs * 1000.f)));
        mix(static_cast<std::uint64_t>(pageTheme_));
        mix(static_cast<std::uint64_t>(small ? 1 : 0));
        // NOT keyed on the app theme. A page's pixels depend on the page
        // theme and the zoom, never on whether the chrome is light or dark -
        // paperOf() reads pageTheme_ alone. Keying on isDark meant that
        // halfway through a theme cross-fade every visible page changed key,
        // missed, and re-rasterised, which is the stall that made the
        // transition look broken.

        auto* src = source_.get();
        const int idx = p.index;
        const PageTheme th = pageTheme_;
        const Color pc = paper;
        const float devScale = rs * ctx.dpr;
        // NOTE: annotations are deliberately NOT inside the cached bitmap.
        // Baking them in would invalidate the page raster on every stroke -
        // the one moment the renderer must stay responsive. They are drawn
        // over the blit instead, which costs a handful of paths per page.
        if (src->cachesOwnRasters()) {
            // The source hands us a ready bitmap. Going through drawCached
            // here would allocate an offscreen surface and composite the
            // whole page into it every time the zoom changes, purely to
            // avoid one blit - measured at 34ms/frame during a zoom burst
            // against 15ms drawing direct.
            t.fillRoundedRect(pageRect, rr, pc);
            t.save();
            t.clipRoundedRect(pageRect, rr, true);
            // Popover/selection HUD animations repaint only their bounded
            // damage on a retained surface. Tell PDFium about that subset
            // too; merely clipping Skia after requesting the full viewport
            // still made it composite every tile on each animation frame.
            Rect view = b;
            if (!ctx.damage.empty()) {
                const Point origin = toWindow({0.f, 0.f});
                const Rect dirty{ctx.damage.x - origin.x, ctx.damage.y - origin.y,
                                 ctx.damage.w, ctx.damage.h};
                const float left = std::max(view.x, dirty.x);
                const float top = std::max(view.y, dirty.y);
                const float right = std::min(view.right(), dirty.right());
                const float bottom = std::min(view.bottom(), dirty.bottom());
                view = right > left && bottom > top
                    ? Rect{left, top, right - left, bottom - top} : Rect{};
            }
            // Inverse-transform the *visible* viewport into source space.
            // Requesting the full unrotated page here defeats visible-tile
            // eviction on long/high-DPI pages after a quarter turn.
            const float theta = -rot * (3.14159265358979323846f / 180.f);
            const float cs = std::cos(theta), sn = std::sin(theta);
            const float cx = r.x + r.w * .5f, cy = r.y + r.h * .5f;
            const auto inverse = [&](float x, float y) {
                x += scrollX_ - cx;
                y += scrollY_ - overscrollY_ - cy;
                return Point{cx + x * cs - y * sn, cy + x * sn + y * cs};
            };
            const Point corners[] = {inverse(view.x, view.y),
                                     inverse(view.right(), view.y),
                                     inverse(view.x, view.bottom()),
                                     inverse(view.right(), view.bottom())};
            float left = corners[0].x, right = left;
            float top = corners[0].y, bottom = top;
            for (const auto& q : corners) {
                left = std::min(left, q.x); right = std::max(right, q.x);
                top = std::min(top, q.y); bottom = std::max(bottom, q.y);
            }
            const float vx0 = std::max(pageRect.x, left);
            const float vy0 = std::max(pageRect.y, top);
            const float vx1 = std::min(pageRect.right(), right);
            const float vy1 = std::min(pageRect.bottom(), bottom);
            if (vx1 > vx0 && vy1 > vy0)
                src->drawVisiblePage(t, idx, pageRect,
                                     Rect{vx0, vy0, vx1 - vx0, vy1 - vy0},
                                     devScale, th);
            t.restore();
        } else {
            t.drawCached(key, pageRect, pxW, pxH,
                         [src, idx, th, pc, rr, devScale, pxW, pxH](IPaintTarget& g) {
                             const Rect full{0.f, 0.f, static_cast<float>(pxW),
                                             static_cast<float>(pxH)};
                             g.fillRoundedRect(full, rr, pc);
                             g.save();
                             g.clipRoundedRect(full, rr, true);
                             src->drawPage(g, idx, full, devScale, th);
                             g.restore();
                         });
        }

        // Selection and find highlights sit over the page blit, under the
        // annotations, and are never baked into a cached raster - they
        // change far too often for that to pay.
        if (hasSelection() || !searchHits_.empty()) {
            std::vector<Rect> rects;
            TextPos a = selA_, bb = selB_;
            if (bb < a) std::swap(a, bb);
            if (hasSelection() && idx >= a.page && idx <= bb.page) {
                const auto* gs = source_->glyphs(idx);
                const int from = idx == a.page ? a.glyph : 0;
                const int to = idx == bb.page
                                   ? bb.glyph
                                   : (gs ? static_cast<int>(gs->size()) - 1 : 0);
                selectionRects(idx, from, to, rects);
            }
            // Live selection is composited from the same PDF glyph-line
            // geometry as the magnifier, outside the immutable page cache.
            paintSelectionBands(t, pageRect, rects);
            if (idx == loupePage) loupeLines = rects;

            for (std::size_t h = 0; h < searchHits_.size(); ++h) {
                const auto& hit = searchHits_[h];
                if (hit.page != idx) continue;
                std::vector<Rect> hr;
                selectionRects(idx, hit.start, hit.start + hit.count - 1, hr);
                const bool cur = static_cast<int>(h) == searchCurrent_;
                Color c = cur ? Color{0xFF, 0x9F, 0x1A, 0xC8}
                              : Color{0xFF, 0xD6, 0x6B, 0x8C};
                for (const Rect& hrr : hr) {
                    t.setBlendMode(pageTheme_ == PageTheme::Dark
                                       ? folio::render::BlendMode::Screen
                                       : folio::render::BlendMode::Multiply);
                    t.fillRect(hrr, c);
                    t.setBlendMode(folio::render::BlendMode::SrcOver);
                    if (cur)
                        t.strokeRoundedRect(hrr, Corners{1.5f, 1.5f, 1.5f, 1.5f},
                                            Color{0xB4, 0x6A, 0x00, 0xFF}, 1.5f);
                }
            }
        }

        if (store_) {
            t.save();
            t.clipRoundedRect(pageRect, rr, true);
            for (const Annotation* a : store_->forPage(p.index))
                drawAnnotation(t, *a, pageRect, enterPhase, pageTheme_ == PageTheme::Dark);
            if (const Annotation* d = store_->draft();
                d && d->page == p.index && !d->empty()) {
                drawAnnotation(t, *d, pageRect, enterPhase, pageTheme_ == PageTheme::Dark);
            }
            t.restore();
        }

        if (small) {
            const bool isCur = p.index == current_;
            if (isCur) {
                t.strokeRoundedRect(pageRect.inset(-2.f), Corners::all(6.f),
                                    ctx.pal.accent, 2.f);
            }
            t.drawTextInBox(std::to_string(p.index + 1),
                            Rect{r.x, pageRect.bottom() + 2.f, r.w, 18.f},
                            styleFor(tokens::type_small,
                                     isCur ? ctx.pal.accentText : ctx.pal.muted),
                            folio::render::TextAlign::Centre);
        }

        if (enterPhase < 0.999f) t.restore();
        t.restore();
    }

    paintAnnotSelection(t, ctx);
    paintSelectionHandles(t, ctx);
    paintTextCaret(t, ctx);

    t.restore();

    paintScrollbars(t, ctx);
    paintSelectionMagnifier(t, ctx, rasterScale_ * ctx.dpr, loupePage, loupeLines);
    paintThumb(t, ctx);
    if (thumbClosing_ && !thumbExit_.active(ctx.now) &&
        thumbExit_.value(ctx.now) <= .004f) {
        thumbLayer_ = ThumbLayer::Closed;
        thumbClosing_ = false;
    }

    // Queue visible text *after* drawPage queued its rasters: a PDF with a
    // huge text layer must not keep its first visible page blank while text
    // extraction monopolises the document's single PDFium worker.
    const int cur = currentPage() - 1;
    for (int p = std::max(0, cur - 1); p <= cur + 1; ++p) {
        if (p >= source_->pageCount()) break;
        (void)source_->glyphs(p);
    }

    source_->endVisibleFrame();

    // Empty state.
    if (placed_.empty()) {
        t.drawTextInBox("No document open", Rect{0.f, b.h * 0.5f - 10.f, b.w, 20.f},
                        styleFor(tokens::type_label, ctx.pal.muted),
                        folio::render::TextAlign::Centre);
    }
}

}  // namespace folio::ui
