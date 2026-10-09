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
constexpr float kFlingFriction = 5.5f;
constexpr float kFlingCutoff = 10.f;

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
    source_ = std::move(src);
    current_ = 0;
    scrollY_ = 0.f;
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
            anchorFrac_ = (probe - p.rect.y) / std::max(1.f, p.rect.h);
            // Same trick across: the fraction is of the page, so it
            // survives the page being re-laid out at the new scale.
            anchorFracX_ = (probeX - p.rect.x) / std::max(1.f, p.rect.w);
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
        // Only steer x once the page is actually wider than the viewport.
        // Below that the page is centred and there is nothing to hold.
        if (p.rect.w > frame.w + 1.f) {
            scrollX_ = p.rect.x + anchorFracX_ * p.rect.w -
                       frame.w * anchorViewFracX_;
        }
        clampScroll();
        return;
    }
}

void PageStage::setZoom(float z, double now) {
    captureZoomAnchor();
    zoomMode_ = ZoomMode::Free;
    zoomTarget_ = std::clamp(z, kMinZoom, kMaxZoom);
    zoomAnim_.animateTo(zoomTarget_, tokens::dur::pageWidth, now);
}

void PageStage::setZoomMode(ZoomMode m, double now) {
    captureZoomAnchor();
    zoomMode_ = m;
    if (m == ZoomMode::Free) return;
    zoomTarget_ = fitScale(m == ZoomMode::FitWidth);
    zoomAnim_.animateTo(zoomTarget_, tokens::dur::pageWidth, now);
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

    // Content width drives the horizontal scrollbar and the pan limit. A
    // page zoomed past the viewport is laid out at kMargin rather than
    // centred (see sizeOf callers), so the overflow is all on the right.
    contentW_ = 0.f;
    for (const Placed& p : placed_)
        contentW_ = std::max(contentW_, p.rect.right() + kMargin);
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
    const bool overV = canScrollY() && vBarTrack().contains(e.pos.x, e.pos.y);
    const bool overH = canScrollX() && hBarTrack().contains(e.pos.x, e.pos.y);

    if (e.phase == PointerPhase::Move && barDrag_ == Bar::None) {
        const Bar was = barHover_;
        barHover_ = overV ? Bar::Vertical : (overH ? Bar::Horizontal : Bar::None);
        return barHover_ != Bar::None || was != Bar::None ? barHover_ != Bar::None
                                                          : false;
    }

    if (e.phase == PointerPhase::Down && (overV || overH)) {
        if (overV) {
            const Rect th = vBarThumb();
            barDrag_ = Bar::Vertical;
            // Grabbing the thumb keeps the point under the cursor; clicking
            // the track jumps so the thumb centres there.
            barGrabOffset_ = th.contains(e.pos.x, e.pos.y) ? e.pos.y - th.y
                                                           : th.h * 0.5f;
        } else {
            const Rect th = hBarThumb();
            barDrag_ = Bar::Horizontal;
            barGrabOffset_ = th.contains(e.pos.x, e.pos.y) ? e.pos.x - th.x
                                                           : th.w * 0.5f;
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

    if (e.phase == PointerPhase::Up) {
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

bool PageStage::hitPage(Point local, int* pageOut, Point* normOut) const {
    const float sy = scrollY_, sx = scrollX_;
    for (const Placed& p : placed_) {
        const Rect r{p.rect.x - sx, p.rect.y - sy, p.rect.w, p.rect.h};
        if (!r.contains(local.x, local.y)) continue;
        if (pageOut) *pageOut = p.index;
        if (normOut)
            *normOut = Point{(local.x - r.x) / std::max(1.f, r.w),
                             (local.y - r.y) / std::max(1.f, r.h)};
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
    if (onAnnotSelectionChanged) onAnnotSelectionChanged(0);
    if (onAnnotationDeleted)
        for (const Annotation& c : copies) onAnnotationDeleted(c);
    if (onAnnotationsChanged) onAnnotationsChanged();
    return true;
}

/// Pick up, drag and drop an existing annotation. Runs only for the Select
/// tool, and only before text selection gets a look at the event - clicking
/// a highlight should grab the highlight, not start selecting the words
/// underneath it.
bool PageStage::handleAnnotSelection(const PointerEvent& e) {
    if (!store_ || editTool_ != EditTool::None) return false;

    switch (e.phase) {
        case PointerPhase::Down: {
            int page = 0;
            Point n{0.f, 0.f};
            if (!hitPage(e.pos, &page, &n)) return false;
            const std::uint64_t hit = store_->hitTest(page, n, 0.008f);
            if (!hit) {
                // A press on bare page clears the selection but does not
                // consume the event: text selection still wants it.
                if (selAnnot_) {
                    selAnnot_ = 0;
                    if (onAnnotSelectionChanged) onAnnotSelectionChanged(0);
                }
                return false;
            }
            selAnnot_ = hit;
            if (const Annotation* a = store_->get(hit)) moveBefore_ = *a;
            movingAnnot_ = true;
            moveLast_ = n;
            if (onAnnotSelectionChanged) onAnnotSelectionChanged(hit);
            return true;
        }
        case PointerPhase::Move: {
            // `pressed` is the router's own press state for this widget;
            // e.buttonsDown is not populated by every host.
            if (!movingAnnot_ || !pressed) return false;
            int page = 0;
            Point n{0.f, 0.f};
            if (!hitPage(e.pos, &page, &n)) return true;
            Annotation* a = store_->get(selAnnot_);
            if (!a) return true;
            // Normalised coordinates are per page, so a drag that crosses a
            // page boundary would teleport. Stay on the page it started on.
            if (page != a->page) return true;
            const float dx = n.x - moveLast_.x, dy = n.y - moveLast_.y;
            moveLast_ = n;
            a->box.x += dx;
            a->box.y += dy;
            for (Point& p : a->points) { p.x += dx; p.y += dy; }
            store_->touch();
            return true;
        }
        case PointerPhase::Up: {
            if (!movingAnnot_) return false;
            movingAnnot_ = false;
            const Annotation* a = store_->get(selAnnot_);
            // Only record a move if it actually moved; a plain click to
            // select must not litter the undo stack.
            if (a && (std::fabs(a->box.x - moveBefore_.box.x) > 1e-5f ||
                      std::fabs(a->box.y - moveBefore_.box.y) > 1e-5f)) {
                if (onAnnotationMoved) onAnnotationMoved(moveBefore_, *a);
                if (onAnnotationsChanged) onAnnotationsChanged();
            }
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
        const Rect ms{mpr->x + b.x * mpr->w, mpr->y + b.y * mpr->h,
                      b.w * mpr->w, b.h * mpr->h};
        const Rect mo = ms.inset(-3.f);
        if (ids.size() > 1) {
            // Lighter per-line rule; the union carries the full-strength one.
            t.strokeRoundedRect(mo, Corners::all(3.f),
                                fade(ctx.pal.accent, 0.55f), 1.f);
        }
        if (!haveUni) { uni = mo; haveUni = true; }
        else {
            const float l = std::min(uni.x, mo.x), tp = std::min(uni.y, mo.y);
            const float r = std::max(uni.right(), mo.right());
            const float bt = std::max(uni.bottom(), mo.bottom());
            uni = Rect{l, tp, r - l, bt - tp};
        }
    }
    if (!haveUni) return;
    const Rect o = ids.size() > 1 ? uni.inset(-3.f) : uni.inset(-2.f);
    t.strokeRoundedRect(o, Corners::all(4.f), ctx.pal.accent, 1.5f);
    // Corner handles. They are not draggable yet - resizing is a separate
    // gesture - but they are what makes "this thing is selected and movable"
    // legible at a glance.
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
    textCaret_  = a->text.size();
    textIsNew_  = false;
    caretBlink_ = now();
    selAnnot_   = 0;
}

void PageStage::commitText(bool cancel) {
    if (!editText_ || !store_) { editText_ = 0; return; }
    const std::uint64_t id = editText_;
    editText_ = 0;
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

    const Rect box{pr->x + a->box.x * pr->w, pr->y + a->box.y * pr->h,
                   a->box.w * pr->w, a->box.h * pr->h};
    // A dashed-looking frame while editing, so it is obvious the thing is
    // live and will keep swallowing keys until Enter or Escape.
    t.fillRoundedRect(box.inset(-4.f), Corners::all(4.f),
                      Color{ctx.pal.accent.r, ctx.pal.accent.g, ctx.pal.accent.b, 18});
    t.strokeRoundedRect(box.inset(-4.f), Corners::all(4.f), ctx.pal.accent, 1.5f);

    // Caret: 530 ms on, 530 ms off, restarted on every keystroke so it is
    // always solid while the user is actually typing.
    const double since = now() - caretBlink_;
    if (std::fmod(since, 1.06) < 0.53) {
        const float em = (a->fontSize > 0.f ? a->fontSize : a->box.h * 0.74f) * pr->h;
        const float adv = em * 0.56f;   // matches the glyph advance
        const float cx = box.x + 2.f + adv * static_cast<float>(textCaret_);
        t.fillRect(Rect{cx, box.y + 2.f, 1.6f, std::max(10.f, box.h - 4.f)},
                   ctx.pal.accent);
    }
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
                case EditTool::Text:      a.kind = AnnotKind::Text;      break;
                case EditTool::Stamp:     a.kind = AnnotKind::Stamp;     break;
                case EditTool::Signature: a.kind = AnnotKind::Signature; break;
                default: return false;
            }
            if (a.kind == AnnotKind::Ink || a.kind == AnnotKind::Signature) {
                a.points.push_back(n);
            } else {
                a.box = Rect{n.x, n.y, 0.f, 0.f};
            }
            if (a.kind == AnnotKind::Note) {
                // A note is a fixed-size pin, placed on press.
                a.box = Rect{n.x, n.y, 0.03f, 0.03f};
                store_->add(a);
                if (onAnnotationsChanged) onAnnotationsChanged();
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
            // Clamp to the page the gesture started on: dragging off the edge
            // should stretch to the margin, not jump to the next page.
            const Point c{std::clamp(n.x, 0.f, 1.f), std::clamp(n.y, 0.f, 1.f)};
            if (d->kind == AnnotKind::Ink || d->kind == AnnotKind::Signature) {
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
                // A highlight tracks the line it was started on rather than
                // becoming a tall rectangle, which is what a text selection
                // would do once there is a text layer to snap to.
                d->box = Rect{std::min(authorStart_.x, c.x), authorStart_.y - 0.012f,
                              std::fabs(c.x - authorStart_.x), 0.026f};
            } else {
                d->box = boxBetween(authorStart_, c);
            }
            return true;
        }
        case PointerPhase::Up: {
            if (!authoring_) return false;
            authoring_ = false;
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

    const Cls hit = classOf((*gs)[static_cast<std::size_t>(glyph)].code);
    if (hit == Sep) {
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

int PageStage::glyphAt(int page, Point n) {
    if (!source_) return -1;
    const auto* gs = source_->glyphs(page);
    if (!gs || gs->empty()) return -1;

    // A direct hit wins outright.
    for (std::size_t i = 0; i < gs->size(); ++i) {
        const auto& g = (*gs)[i];
        if (n.x >= g.l && n.x <= g.r && n.y >= g.t && n.y <= g.b)
            return static_cast<int>(i);
    }
    // Otherwise the closest glyph, weighting vertical distance heavily so a
    // drag that wanders past the end of a line stays on that line rather
    // than jumping to whatever is horizontally nearest two lines down.
    int best = -1;
    float bestD = 1e9f;
    for (std::size_t i = 0; i < gs->size(); ++i) {
        const auto& g = (*gs)[i];
        const float cy = (g.t + g.b) * 0.5f;
        const float dy = std::fabs(n.y - cy);
        const float dx = n.x < g.l ? g.l - n.x : (n.x > g.r ? n.x - g.r : 0.f);
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

    // Merge consecutive glyphs that share a line into one rectangle. PDFium
    // reports glyphs in reading order, so a line break is simply a glyph
    // whose vertical centre has moved. Output is page-normalised, which is
    // the space both the selection overlay and annotations want.
    bool open = false;
    float l = 0, t = 0, r = 0, b = 0;
    auto flush = [&] {
        if (!open) return;
        out.push_back(Rect{l, t, r - l, b - t});
        open = false;
    };
    for (int i = from; i <= to; ++i) {
        const auto& g = (*gs)[static_cast<std::size_t>(i)];
        if (g.r <= g.l || g.b <= g.t) continue;      // zero-area: spaces
        const float cy = (g.t + g.b) * 0.5f;
        const bool sameLine = open && cy >= t && cy <= b && g.l >= l - 0.02f;
        if (!sameLine) {
            flush();
            l = g.l; t = g.t; r = g.r; b = g.b;
            open = true;
        } else {
            r = std::max(r, g.r);
            t = std::min(t, g.t);
            b = std::max(b, g.b);
        }
    }
    flush();
}

void PageStage::selectionRects(int page, int from, int to,
                               std::vector<Rect>& out) {
    const Rect* pr = nullptr;
    for (const auto& p : placed_)
        if (p.index == page) { pr = &p.rect; break; }
    if (!pr) return;
    std::vector<Rect> norm;
    selectionLines(page, from, to, norm);
    for (const Rect& n : norm)
        out.push_back(Rect{pr->x + n.x * pr->w, pr->y + n.y * pr->h,
                           n.w * pr->w, n.h * pr->h});
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
            pinchZoom_ = zoomTarget_;
            // One finger with a tool active is a drawing stroke: let the
            // authoring path have it.
            if (n == 1 && editTool_ != EditTool::None) return false;
            // A second finger landing mid-stroke means the user wanted to
            // zoom, not to draw. Throw the half-drawn stroke away instead
            // of committing a stray mark when that finger lifts.
            if (n >= 2 && authoring_) {
                store_->cancelDraft();
                authoring_ = false;
            }
            touchPanning_ = (n >= 1);
            return true;

        case PointerPhase::Move: {
            if (n >= 2) {
                // Two fingers: scale about the midpoint and carry the page
                // along with it, so the content under the fingers stays
                // under the fingers.
                const PinchState p = touch_.pinch();
                setZoomFocus(p.centroid.x, p.centroid.y);
                pinchZoom_ = std::clamp(pinchZoom_ * p.scale, kMinZoom, kMaxZoom);
                zoomMode_ = ZoomMode::Free;
                zoomTarget_ = pinchZoom_;
                // Snap, not animate: the zoom must track the fingers
                // frame for frame or the gesture feels like it is lagging.
                zoomAnim_.reset(pinchZoom_);
                applyZoomAnchor();
                scrollX_ -= p.centroidDelta.x;
                scrollY_ -= p.centroidDelta.y;
                clampScroll();
                updateCurrentFromScroll();
                return true;
            }
            if (!touchPanning_) return false;
            const Point d = touch_.panDelta();
            scrollX_ -= d.x;
            scrollY_ -= d.y;
            clampScroll();
            updateCurrentFromScroll();
            return true;
        }

        case PointerPhase::Up:
        case PointerPhase::Cancel: {
            const bool wasPanning = touchPanning_;
            if (touch_.count() == 0) touchPanning_ = false;

            // Double tap toggles between fitting the page and a readable
            // 2x at the point tapped - the gesture every mobile reader has.
            // ...but not while a tool is armed: there, two quick taps are
            // two deliberate dots, and zooming instead would destroy work.
            if (touch_.wasTap() && touch_.tapCount() >= 2 &&
                editTool_ == EditTool::None) {
                const Point tp = touch_.tapPos();
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

            const Point v = touch_.flingVelocity();
            if (std::fabs(v.y) > 1.f) {
                // Hand off to the existing inertial scroller rather than
                // inventing a second one: one decay curve, one cutoff.
                velocity_ = -v.y;
                smoothing_ = false;
            }
            return wasPanning;
        }

        default:
            return false;
    }
}

bool PageStage::onPointer(const PointerEvent& e) {
    // Touch first: it decides whether this is a gesture or a
    // single-pointer interaction the mouse paths below understand.
    if (handleTouch(e)) return true;

    // Scrollbars first: a press on one must never fall through to the page
    // and start a selection or a pan.
    if (handleScrollbar(e)) return true;
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
            if (hasSelection() && onSelectionCleared) onSelectionCleared();

            // With the select tool (the default in Read mode), a press on
            // text starts a selection instead of a pan. A press on empty
            // space still pans, which is what every other reader does.
            int pg = 0;
            Point n{0.f, 0.f};
            if (editTool_ == EditTool::None && hitPage(e.pos, &pg, &n)) {
                const int g = glyphAt(pg, n);
                if (g >= 0) {
                    selA_ = selB_ = TextPos{pg, g};
                    selecting_ = true;
                    velocity_ = 0.f;
                    smoothing_ = false;
                    return true;
                }
            }
            clearSelection();
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
                const float dy = e.pos.y - dragLast_.y;
                scrollY_ -= dy;
                if (canScrollX()) scrollX_ -= e.pos.x - dragLast_.x;
                clampScroll();
                dragLast_ = e.pos;
                updateCurrentFromScroll();
            }
            return true;
        case PointerPhase::Up: {
            // A finished selection drag is what summons the markup popup.
            // Fire on release rather than on every move, so the popup does
            // not chase the cursor while the user is still choosing words.
            const bool wasSelecting = selecting_;
            selecting_ = false;
            dragging_ = false;
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

bool PageStage::animating(double now) const {
    // An async source keeps the loop alive on its own account: a raster that
    // lands while nothing else is moving still has to be painted.
    if (source_ && source_->pending()) return true;
    if (zoomAnim_.active(now) || rotationAnim_.active(now) || smoothing_) return true;
    // A finger resting on the page must keep frames coming, or the
    // long-press timer has nothing to fire from.
    if (touch_.count() > 0) return true;
    if (std::fabs(velocity_) > kFlingCutoff) return true;
    return std::any_of(entrance_.begin(), entrance_.end(),
                       [now](const Timeline& tl) { return tl.active(now); });
}

void PageStage::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    // Long press: a finger has no hover, so there is no way to say "select
    // this text" by pointing at it. Holding still is the gesture every
    // mobile reader uses, and it has to be polled because it is defined by
    // the *absence* of events.
    if (touch_.pollLongPress(ctx.now) && !touch_.pointers().empty()) {
        const Point p = touch_.pointers().front().pos;
        int pg = 0;
        Point n{0.f, 0.f};
        if (hitPage(p, &pg, &n)) {
            const int g = glyphAt(pg, n);
            if (g >= 0) {
                // Select the word under the finger and hand over to the
                // ordinary drag-selection path, so the handles and the
                // markup popup behave exactly as they do with a mouse.
                selectWordAt(pg, g);
                selecting_ = true;
                touchPanning_ = false;
                // Same hand-off a mouse drag makes on release, so the
                // markup popup appears over the word just selected.
                if (onSelectionCommitted)
                    onSelectionCommitted(toWindow(p));
            }
        }
    }

    // Install anything the engine finished since the last frame, before we
    // decide what to draw.
    if (source_) {
        source_->tick();
        source_->setFocusPage(currentPage() - 1);
        // Warm the text layer for the pages around the viewport. Asking is
        // what queues extraction, and a selection drag cannot wait a frame
        // for it - the press has to land on a glyph immediately or it
        // becomes a pan.
        const int cur = currentPage() - 1;
        for (int p = std::max(0, cur - 1); p <= cur + 1; ++p) {
            if (p >= source_->pageCount()) break;
            (void)source_->glyphs(p);
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
        self->scrollY_ += static_cast<float>(velocity_ * dt);
        self->velocity_ *= std::exp(-kFlingFriction * static_cast<float>(dt));
        self->clampScroll();
    }

    const float rot = rotationAnim_.value(ctx.now);
    const Color paper = paperOf(pageTheme_);

    t.save();
    t.translate(-scrollX_, -scrollY_);

    for (std::size_t i = 0; i < placed_.size(); ++i) {
        const Placed& p = placed_[i];
        // Cull: only pages intersecting the viewport, plus one screen of
        // slack so a fast fling does not reveal unpainted gaps.
        if (p.rect.bottom() < scrollY_ - b.h || p.rect.y > scrollY_ + b.h * 2.f) {
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

        // Rotation about the page centre.
        Rect r = p.rect;
        if (std::fabs(std::fmod(rot, 360.f)) > 0.01f) {
            t.translate(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
            t.rotateDegrees(std::fmod(rot, 360.f) - static_cast<float>(rotation_) * 90.f);
            t.translate(-(r.x + r.w * 0.5f), -(r.y + r.h * 0.5f));
        }

        const bool small = layout_ == LayoutMode::Grid;
        const Corners rr = Corners::all(small ? 4.f : 2.f);
        const auto elev = small ? tokens::elevation::pageSmall(ctx.pal)
                                : tokens::elevation::page(ctx.pal);

        Rect pageRect = r;
        if (small) pageRect.h = r.w * (792.f / 612.f);  // label sits below

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
            src->drawPage(t, idx, pageRect, devScale, th);
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
            Color sel = ctx.pal.accent;
            sel.a = 72;
            for (const Rect& sr : rects) t.fillRect(sr, sel);

            for (std::size_t h = 0; h < searchHits_.size(); ++h) {
                const auto& hit = searchHits_[h];
                if (hit.page != idx) continue;
                std::vector<Rect> hr;
                selectionRects(idx, hit.start, hit.start + hit.count - 1, hr);
                const bool cur = static_cast<int>(h) == searchCurrent_;
                Color c = cur ? Color{0xFF, 0x9F, 0x1A, 0xC8}
                              : Color{0xFF, 0xD6, 0x6B, 0x8C};
                for (const Rect& hrr : hr) {
                    t.fillRect(hrr, c);
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
                drawAnnotation(t, *a, pageRect, enterPhase);
            if (const Annotation* d = store_->draft();
                d && d->page == p.index && !d->empty()) {
                drawAnnotation(t, *d, pageRect, enterPhase);
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
    paintTextCaret(t, ctx);

    t.restore();

    paintScrollbars(t, ctx);

    // Empty state.
    if (placed_.empty()) {
        t.drawTextInBox("No document open", Rect{0.f, b.h * 0.5f - 10.f, b.w, 20.f},
                        styleFor(tokens::type_label, ctx.pal.muted),
                        folio::render::TextAlign::Centre);
    }
}

}  // namespace folio::ui
