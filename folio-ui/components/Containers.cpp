#include "components/Containers.h"

#include "folio/ui/Icons.h"

#include <algorithm>
#include <cmath>

namespace folio::ui {
namespace {

constexpr float kTabClose = 18.f;
constexpr float kFlingFriction = 4.6f;   // momentum decay; still controllable by touch
constexpr float kFlingCutoff = 8.f;      // px/s below which we stop

}  // namespace

// ---------------------------------------------------------------------------
// Stack
// ---------------------------------------------------------------------------

void Stack::onLayout() {
    const bool row = dir == Dir::Row;
    const float innerW = frame.w - padL - padR;
    const float innerH = frame.h - padT - padB;
    const float avail = row ? innerW : innerH;

    float used = 0.f;
    float totalFlex = 0.f;
    int visibleCount = 0;
    for (const auto& c : children()) {
        if (!c->visible || c->lp.skip) continue;
        ++visibleCount;
        if (c->lp.flex > 0.f) {
            totalFlex += c->lp.flex;
        } else {
            const float basis = c->lp.basis >= 0.f ? c->lp.basis
                                                   : (row ? c->frame.w : c->frame.h);
            used += basis;
        }
    }
    if (visibleCount > 1) used += spacing * static_cast<float>(visibleCount - 1);

    const float leftover = std::max(0.f, avail - used);
    float cursor = row ? padL : padT;

    for (const auto& c : children()) {
        if (!c->visible || c->lp.skip) continue;
        float main = c->lp.flex > 0.f
                         ? (totalFlex > 0.f ? leftover * (c->lp.flex / totalFlex) : 0.f)
                         : (c->lp.basis >= 0.f ? c->lp.basis
                                               : (row ? c->frame.w : c->frame.h));
        const float crossSize = row ? c->frame.h : c->frame.w;
        const float crossAvail = row ? innerH : innerW;

        float crossPos = row ? padT : padL;
        float crossLen = crossSize;
        switch (cross) {
            case Cross::Stretch: crossLen = crossAvail; break;
            case Cross::Centre:  crossPos += (crossAvail - crossSize) * 0.5f; break;
            case Cross::End:     crossPos += crossAvail - crossSize; break;
            case Cross::Start:   break;
        }

        if (row) {
            c->setFrame(cursor, crossPos, main, crossLen);
        } else {
            c->setFrame(crossPos, cursor, crossLen, main);
        }
        cursor += main + spacing;
    }
}

// ---------------------------------------------------------------------------
// CardView
// ---------------------------------------------------------------------------

void CardView::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    const Rect b = localBounds();
    if (b.empty()) return;
    drawCard(t, b, cornersOrRadius(), fill(ctx.pal), elevation(ctx.pal));
}

// ---------------------------------------------------------------------------
// ScrollView
// ---------------------------------------------------------------------------

ScrollView::ScrollView() {
    clipsContent = true;
    acceptsPointer = true;
    touchScrollable = true;
    debugName = "ScrollView";
}

float ScrollView::maxScroll() const {
    return std::max(0.f, contentHeight - frame.h);
}

void ScrollView::setScrollY(float y) {
    scrollY_ = std::clamp(y, 0.f, maxScroll());
}

void ScrollView::scrollBy(float dy) {
    setScrollY(scrollY_ + dy);
}

void ScrollView::revealRange(float top, float height, double now, int durationMs) {
    const float visibleTop = scrollY_;
    const float visibleBottom = scrollY_ + frame.h;
    float target = scrollY_;
    if (top < visibleTop) {
        target = top - 12.f;
    } else if (top + height > visibleBottom) {
        target = top + height - frame.h + 12.f;
    } else {
        return;
    }
    target = std::clamp(target, 0.f, maxScroll());
    smooth_.reset(scrollY_);
    smooth_.animateTo(target, durationMs, now);
    smoothActive_ = true;
    velocity_ = 0.f;
}

bool ScrollView::onPointer(const PointerEvent& e) {
    if (barTrackPress_ && e.pointerId == barId_) {
        if (e.phase == PointerPhase::Up || e.phase == PointerPhase::Cancel)
            barTrackPress_ = false;
        return true; // rail taps never jump or start content gestures
    }
    if (barHeld_ && e.pointerId == barId_) {
        if (e.phase == PointerPhase::Up || e.phase == PointerPhase::Cancel) {
            barHeld_ = false;
            barAlpha_.animateTo(0.f, tokens::dur::hover, e.timestamp);
            return true;
        }
        if (e.phase == PointerPhase::Move) {
            const float trackH = std::max(1.f, frame.h - 8.f);
            const float thumbH = std::min(trackH, std::max(32.f,
                trackH * (frame.h / std::max(1.f, contentHeight))));
            const float span = trackH - thumbH;
            if (span > 0.f)
                setScrollY(std::clamp((e.pos.y - 4.f - barGrabOffset_) / span,
                                      0.f, 1.f) * maxScroll());
            return true;
        }
        return true;
    }
    if (barHeld_ && e.isTouch()) return true;
    if (e.phase == PointerPhase::Down && showScrollbar && maxScroll() > 0.f &&
        e.pos.x >= frame.w - (e.isTouch() ? 28.f : 12.f) &&
        e.pos.x < frame.w && e.pos.y >= 0.f && e.pos.y < frame.h) {
        const float trackH = std::max(1.f, frame.h - 8.f);
        const float thumbH = std::min(trackH, std::max(32.f,
            trackH * (frame.h / std::max(1.f, contentHeight))));
        const float thumbY = 4.f + (trackH - thumbH) * scrollY_ / maxScroll();
        barId_ = e.pointerId;
        if (e.pos.y < thumbY || e.pos.y >= thumbY + thumbH) {
            barTrackPress_ = true;
            return true; // clicking the empty rail is deliberately inert
        }
        barGrabOffset_ = e.pos.y - thumbY;
        barHeld_ = true;
        velocity_ = 0.f;
        smoothActive_ = false;
        barAlpha_.reset(1.f);
        return true;
    }
    if (e.phase == PointerPhase::Wheel) {
        if (maxScroll() <= 0.f) return false;
        smoothActive_ = false;
        // A wheel notch is ~120 units; the prototype scrolls about 3 lines.
        scrollBy(-e.wheelY * 48.f);
        velocity_ = 0.f;
        barAlpha_.animateTo(1.f, tokens::dur::hoverFast, e.timestamp);
        return true;
    }
    if (!e.isTouch()) return false;
    if (e.phase == PointerPhase::Down) {
        if (touchHeld_) return true;
        touchHeld_ = true;
        touchMoved_ = false;
        touchId_ = e.pointerId;
        touchStartY_ = touchLastY_ = e.pos.y;
        touchLastTime_ = e.timestamp;
        smoothActive_ = false;
        velocity_ = overscrollVelocity_ = 0.f;
        return false; // a stationary tap still belongs to the list row
    }
    if (e.phase == PointerPhase::Cancel) {
        touchHeld_ = false;
        touchMoved_ = false;
        velocity_ = 0.f;
        return true;
    }
    if (!touchHeld_ || e.pointerId != touchId_) return false;
    if (e.phase == PointerPhase::Move) {
        const float dy = e.pos.y - touchLastY_;
        if (!touchMoved_ && std::fabs(e.pos.y - touchStartY_) > 10.f)
            touchMoved_ = true;
        if (touchMoved_) {
            const float before = scrollY_;
            const float wanted = scrollY_ - dy;
            const float bound = std::clamp(wanted, 0.f, maxScroll());
            // Resistance beyond either edge, capped so the panel cannot be
            // dragged away indefinitely. The release spring settles it.
            scrollY_ = bound + std::clamp((wanted - bound) * 0.42f,
                                           -60.f, 60.f);
            const double dt = e.timestamp - touchLastTime_;
            if (dt >= 0.002 && scrollY_ != before) {
                const float speed = std::clamp(-dy / static_cast<float>(dt),
                                               -2200.f, 2200.f);
                velocity_ = velocity_ * 0.65f + speed * 0.35f;
            }
            barAlpha_.animateTo(1.f, tokens::dur::hoverFast, e.timestamp);
        }
        touchLastY_ = e.pos.y;
        touchLastTime_ = e.timestamp;
        return touchMoved_;
    }
    if (e.phase == PointerPhase::Up) {
        const bool dragged = touchMoved_;
        touchHeld_ = touchMoved_ = false;
        if (!dragged || e.timestamp - touchLastTime_ > 0.08 ||
            std::fabs(velocity_) < 120.f) velocity_ = 0.f;
        if (scrollY_ < 0.f || scrollY_ > maxScroll()) {
            overscrollVelocity_ = std::clamp(velocity_ * 0.15f, -320.f, 320.f);
            velocity_ = 0.f;
        }
        return dragged; // a tap may select a thumbnail/outline row
    }
    return false;
}

bool ScrollView::animating(double now) const {
    return smoothActive_ || std::fabs(velocity_) > kFlingCutoff ||
           scrollY_ < -0.1f || scrollY_ > maxScroll() + 0.1f ||
           std::fabs(overscrollVelocity_) > 1.f || barAlpha_.active(now);
}

void ScrollView::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    // Integrate the fling here because it is the one place guaranteed to run
    // exactly once per frame with a known timestamp.
    auto* self = const_cast<ScrollView*>(this);
    const double dt = (lastFrame_ > 0.0) ? std::min(ctx.now - lastFrame_, 0.1) : 0.0;
    self->lastFrame_ = ctx.now;

    if (smoothActive_) {
        self->scrollY_ = smooth_.value(ctx.now);
        if (!smooth_.active(ctx.now)) self->smoothActive_ = false;
    } else if (std::fabs(velocity_) > kFlingCutoff && dt > 0.0) {
        const float wanted = scrollY_ + static_cast<float>(velocity_ * dt);
        const float bound = std::clamp(wanted, 0.f, maxScroll());
        if (std::fabs(wanted - bound) > 0.01f) {
            self->scrollY_ = bound + std::clamp((wanted - bound) * 0.30f,
                                                  -60.f, 60.f);
            self->overscrollVelocity_ = std::clamp(velocity_ * 0.15f, -320.f, 320.f);
            self->velocity_ = 0.f;
        } else {
            self->scrollY_ = wanted;
            self->velocity_ *= std::exp(-kFlingFriction * static_cast<float>(dt));
        }
    }
    if (!touchHeld_ && !barHeld_ && dt > 0.0 &&
        (scrollY_ < 0.f || scrollY_ > maxScroll() ||
         std::fabs(overscrollVelocity_) > 0.1f)) {
        const float goal = std::clamp(scrollY_, 0.f, maxScroll());
        const float step = static_cast<float>(std::min(dt, 0.032));
        self->overscrollVelocity_ += (-144.f * (scrollY_ - goal) -
                                      24.f * overscrollVelocity_) * step;
        self->scrollY_ += overscrollVelocity_ * step;
        if (std::fabs(scrollY_ - goal) < 0.3f &&
            std::fabs(overscrollVelocity_) < 4.f)
            { self->scrollY_ = goal; self->overscrollVelocity_ = 0.f; }
    }

    if (!hovered && barAlpha_.target() > 0.f && !smoothActive_ &&
        std::fabs(velocity_) <= kFlingCutoff) {
        self->barAlpha_.animateTo(0.f, tokens::dur::hover, ctx.now);
    }
    (void)t;
}

void ScrollView::onPaintOverlay(IPaintTarget& t, const PaintCtx& ctx) {
    if (!showScrollbar) return;
    const float maxS = maxScroll();
    if (maxS <= 0.f) return;
    const float a = std::max(barAlpha_.value(ctx.now), hovered ? 0.6f : 0.f);
    if (a <= 0.01f) return;

    const Rect b = localBounds();
    const float trackH = b.h - 8.f;
    const float thumbH = std::max(32.f, trackH * (b.h / contentHeight));
    const float y = 4.f + (trackH - thumbH) * std::clamp(scrollY_ / maxS, 0.f, 1.f);
    t.fillRoundedRect(Rect{b.w - 7.f, y, 4.f, thumbH}, Corners::all(2.f),
                      fade(ctx.pal.muted, a * 0.55f));
}

// ---------------------------------------------------------------------------
// SidePanel
// ---------------------------------------------------------------------------

SidePanel::SidePanel(Side s)
    : side_(s),
      width_(s == Side::Left ? tokens::size::panelLeft : tokens::size::panelRight),
      expanded_(s == Side::Left ? tokens::size::panelLeft
                                : tokens::size::panelRight) {
    debugName = s == Side::Left ? "SidePanel(L)" : "SidePanel(R)";
    // Children are laid out at the final expanded width so opening doesn't
    // reflow text/thumbnail rows. Clip those children to the *animated*
    // frame: otherwise pages paint before the panel arrives and linger in
    // the canvas after its edge has passed them on collapse.
    clipsContent = true;
    clipSelfPaint = false; // preserve CardView's outside elevation/shadow
}

void SidePanel::setCollapsedImmediately(bool c) noexcept {
    collapsed_ = c;
    width_.reset(c ? tokens::size::panelRailCollapsed : expanded_);
    collapseT_.reset(c ? 1.f : 0.f);
    sheetT_.reset(c ? 0.f : 1.f);
}

void SidePanel::setCollapsed(bool c, double now) {
    if (c == collapsed_) return;
    collapsed_ = c;
    const float expanded = expanded_;
    const int d = c ? tokens::dur::panelCollapse : tokens::dur::panelReveal;
    width_.animateTo(c ? tokens::size::panelRailCollapsed : expanded, d, now);
    collapseT_.animateTo(c ? 1.f : 0.f, d, now);
    // A sheet travels instead of narrowing. OutCubic on the way up gives it
    // the slight settle a surface pushed by a thumb should have; closing is
    // quicker and plain, because a dismissal that lingers feels unresponsive.
    sheetT_.animateTo(c ? 0.f : 1.f, d, now,
                      c ? Curve::Standard : Curve::OutCubic);
}

float SidePanel::sheetProgress(double now) const { return sheetT_.value(now); }

void SidePanel::setExpandedWidth(float w, double now) {
    if (std::fabs(w - expanded_) < 0.5f) return;
    expanded_ = w;
    // Re-target the animation so a breakpoint change slides rather than
    // snaps, and so a panel resized while collapsed stays collapsed.
    if (!collapsed_) width_.animateTo(w, tokens::dur::panelReveal, now);
}

float SidePanel::currentHeight(double now, float expandedH) const {
    const float target = (side_ == Side::Left) ? tokens::size::panelLeftCollapsedHeight
                                               : tokens::size::panelRightCollapsedHeight;
    return lerp(expandedH, std::min(target, expandedH), collapseT_.value(now));
}

float SidePanel::railAlpha(double now) const {
    // The rail appears only once the panel is nearly closed, matching the
    // prototype's delayed .col .rail fade. Otherwise the icons swim about
    // inside a panel that is still wide.
    return smoothstep(0.72f, 1.f, collapseT_.value(now));
}

float SidePanel::contentAlpha(double now) const {
    if (present_ == Presentation::Sheet) {
        // A sheet travels; it does not dissolve. Fading its content across
        // the whole slide would read as two animations fighting, so this
        // only takes the edge off the first moment of the move.
        return smoothstep(0.f, 0.25f, sheetT_.value(now));
    }
    const float expanded = expanded_;
    const float w = width_.value(now);
    const float t = std::clamp(
        (w - tokens::size::panelRailCollapsed) /
            std::max(1.f, expanded - tokens::size::panelRailCollapsed),
        0.f, 1.f);
    // Phase-locked to the panel's own geometry: full opacity exactly when
    // the panel reaches full width, gone exactly when it is a rail again.
    //
    // This used to be driven by `1 - railAlpha()`, and railAlpha is
    // deliberately delayed (smoothstep 0.72..1) because the *rail* should
    // only appear once the panel is nearly shut. Taking its complement for
    // the *body* inherited that delay backwards: the content reached full
    // opacity 28% into the opening, and on the way out stayed solid until
    // the last 28% - so it arrived before the panel and outstayed it.
    return smoothstep(0.f, 1.f, t);
}

bool SidePanel::animating(double now) const {
    return width_.active(now) || collapseT_.active(now) || sheetT_.active(now);
}

// ---------------------------------------------------------------------------
// TabBar
// ---------------------------------------------------------------------------

TabBar::TabBar() {
    acceptsPointer = true;
    clipsContent = true; // an arbitrary number of tabs must never cover window controls
    debugName = "TabBar";
}

void TabBar::addTab(std::string title, double now, bool select) {
    Tab t;
    t.title = std::move(title);
    t.open.start(now, tokens::dur::tabOpen);
    tabs_.push_back(std::move(t));
    if (select) {
        active_ = static_cast<int>(tabs_.size()) - 1;
        revealActive(now);
    }
}

void TabBar::closeTab(int index, double now) {
    if (index < 0 || static_cast<std::size_t>(index) >= tabs_.size()) return;
    Tab& t = tabs_[static_cast<std::size_t>(index)];
    if (t.isClosing) return;
    t.isClosing = true;
    t.closing.animateTo(0.f, tokens::dur::tabClose, now);
    if (onClose) onClose(index);
}

void TabBar::setTitle(int index, std::string title) {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return;
    tabs_[static_cast<std::size_t>(index)].title = std::move(title);
}

void TabBar::selectTab(int index, double now) {
    if (index < 0 || static_cast<std::size_t>(index) >= tabs_.size()) return;
    if (tabs_[static_cast<std::size_t>(index)].isClosing) return;
    active_ = index;
    revealActive(now);
    if (onSelect) onSelect(index);
}

void TabBar::setDirty(int index, bool dirty) {
    if (index < 0 || static_cast<std::size_t>(index) >= tabs_.size()) return;
    tabs_[static_cast<std::size_t>(index)].dirty = dirty;
}

float TabBar::tabWidth(const Tab& t, double now) const {
    // Width is what animates on open and close, so the neighbours slide
    // across rather than teleporting.
    const float full = tokens::size::tab::width;
    if (t.isClosing) return full * t.closing.value(now);
    return full * t.open.phase(now);
}

float TabBar::intrinsicWidth(double now) const {
    float w = 0.f;
    for (const Tab& t : tabs_) w += tabWidth(t, now) + 4.f;
    return w + 30.f;  // the "+" button
}

float TabBar::maxScroll(double now) const {
    return std::max(0.f, intrinsicWidth(now) - frame.w);
}

void TabBar::revealActive(double now) {
    float left = 0.f;
    for (int i = 0; i < active_ && i < static_cast<int>(tabs_.size()); ++i)
        left += tabWidth(tabs_[i], now) + 4.f;
    const float right = active_ < static_cast<int>(tabs_.size())
        ? left + tabWidth(tabs_[static_cast<std::size_t>(active_)], now) : left;
    if (left < scrollX_) scrollX_ = left;
    if (right + 8.f > scrollX_ + frame.w) scrollX_ = right + 8.f - frame.w;
    scrollX_ = std::clamp(scrollX_, 0.f, maxScroll(now));
}

int TabBar::hitTab(Point local, double now, bool* closeHit) const {
    float x = -scrollX_;
    if (closeHit) *closeHit = false;
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        const float w = tabWidth(tabs_[i], now);
        if (local.x >= x && local.x < x + w) {
            if (closeHit) *closeHit = local.x > x + w - kTabClose - 6.f;
            return static_cast<int>(i);
        }
        x += w + 4.f;
    }
    return -1;
}

void TabBar::reapClosed(double now) {
    std::erase_if(tabs_, [&](const Tab& t) {
        return t.isClosing && !t.closing.active(now) && t.closing.value(now) <= 0.01f;
    });
    active_ = std::clamp(active_, 0, static_cast<int>(tabs_.size()) - 1);
    scrollX_ = std::clamp(scrollX_, 0.f, maxScroll(now));
}

Rect TabBar::plusRect() const {
    float x = -scrollX_;
    for (const Tab& tab : tabs_) x += tabWidth(tab, lastNow_) + 4.f;
    const float th = tokens::size::tab::height;
    const float y = (frame.h - th) * 0.5f;
    return Rect{x + 2.f, y + (th - 26.f) * 0.5f, 26.f, 26.f};
}

bool TabBar::onPointer(const PointerEvent& e) {
    if (e.phase == PointerPhase::Wheel) {
        if (maxScroll(e.timestamp) <= 0.f) return false;
        scrollX_ = std::clamp(scrollX_ - (e.wheelX != 0.f ? e.wheelX : e.wheelY) * 64.f,
                              0.f, maxScroll(e.timestamp));
        return true;
    }
    const bool oldPlus = hoverTab_ < 0 &&
        plusRect().contains(hoverPos_.x, hoverPos_.y);
    hoverPos_ = e.pos;
    switch (e.phase) {
        case PointerPhase::Move:
        case PointerPhase::Enter: {
            bool onX = false;
            const int nextTab = hitTab(e.pos, e.timestamp, &onX);
            const bool nextPlus = nextTab < 0 &&
                plusRect().contains(e.pos.x, e.pos.y);
            const bool changed = nextTab != hoverTab_ || onX != hoverClose_ ||
                                 oldPlus != nextPlus;
            hoverTab_ = nextTab;
            hoverClose_ = onX;
            return changed;
        }
        case PointerPhase::Leave:
            hoverTab_ = -1;
            hoverClose_ = false;
            hoverPos_ = Point{-1.f, -1.f};
            return true;
        case PointerPhase::Up: {
            bool onX = false;
            const int idx = hitTab(e.pos, e.timestamp, &onX);
            if (idx < 0) {
                // Past the last tab. Only the "+" glyph itself opens a new
                // tab - treating the whole empty strip as a button meant any
                // stray click in the title bar spawned one.
                if (plusRect().contains(e.pos.x, e.pos.y) && onNew) onNew();
            } else if (onX) {
                closeTab(idx, e.timestamp);
            } else {
                selectTab(idx, e.timestamp);
            }
            return true;
        }
        case PointerPhase::Down:
            return true;
        default:
            break;
    }
    return false;
}

bool TabBar::animating(double now) const {
    return std::any_of(tabs_.begin(), tabs_.end(), [now](const Tab& t) {
        return t.open.active(now) || t.closing.active(now) ||
               (t.isClosing && t.closing.value(now) > 0.01f);
    });
}

void TabBar::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    const_cast<TabBar*>(this)->reapClosed(ctx.now);

    lastNow_ = ctx.now;
    auto* self = const_cast<TabBar*>(this);
    self->scrollX_ = std::clamp(scrollX_, 0.f, maxScroll(ctx.now));
    // Width animation may reveal a newly added tab outside the visible strip.
    if (animating(ctx.now)) self->revealActive(ctx.now);
    const float h = tokens::size::tab::height;
    const float y = (frame.h - h) * 0.5f;
    float x = -scrollX_;

    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        const Tab& tab = tabs_[i];
        const float w = tabWidth(tab, ctx.now);
        if (w < 2.f) { x += w + 4.f; continue; }

        const bool isActive = static_cast<int>(i) == active_ && !tab.isClosing;
        const bool isHover = static_cast<int>(i) == hoverTab_;
        const Rect r{x, y, w, h};
        const Corners rr{tokens::radius::sm, tokens::radius::sm, 0.f, 0.f};
        const float alpha = tab.isClosing ? tab.closing.value(ctx.now)
                                          : tab.open.phase(ctx.now);

        if (isActive) {
            drawCard(t, r, rr, ctx.pal.card, tokens::elevation::tabActive(ctx.pal),
                     alpha);
        } else if (isHover) {
            t.fillRoundedRect(r, rr, fade(ctx.pal.hover, alpha));
        }

        // Content fades in behind the width animation so a 20 px-wide
        // opening tab does not show a squashed glyph.
        const float contentA = alpha * smoothstep(0.35f, 1.f, alpha);
        if (contentA > 0.01f && w > 40.f) {
            const Color fg = fade(isActive ? ctx.pal.text : ctx.pal.muted, contentA);
            const float textX = x + 10.f;
            const float textW = w - 10.f - kTabClose - 8.f;
            t.drawTextInBox(tab.title, Rect{textX, y, std::max(0.f, textW), h},
                            styleFor(isActive ? tokens::type_bodyStrong
                                              : tokens::type_body,
                                     fg),
                            folio::render::TextAlign::Left);

            if (tab.dirty) {
                t.fillCircle(Point{x + w - kTabClose - 12.f, y + h * 0.5f}, 3.f,
                             fade(tokens::fixed::dirtyDot, contentA));
            }
            const Rect closeBox{x + w - kTabClose - 5.f, y + (h - kTabClose) * 0.5f,
                                kTabClose, kTabClose};
            if (isHover && hoverClose_) {
                t.fillRoundedRect(closeBox, Corners::all(5.f),
                                  fade(ctx.pal.hover, contentA));
            }
            if (isHover || isActive) {
                drawIcon(t, "close", closeBox.inset(4.f), fade(fg, contentA * 0.9f),
                         1.7f);
            }
        }
        x += w + 4.f;
    }

    // New-tab button.
    const Rect plus{x + 2.f, y + (h - 26.f) * 0.5f, 26.f, 26.f};
    const bool hoverPlus = hoverTab_ < 0 && hovered && plusRect().contains(hoverPos_.x, hoverPos_.y);
    if (hoverPlus) t.fillRoundedRect(plus, Corners::all(tokens::radius::xs), ctx.pal.hover);
    // A new-tab plus sits on its own with no label to lean on, so it is
    // drawn a touch larger and heavier than a toolbar glyph. At the stock
    // 16px/1.6pt it softened into something closer to a dagger once the
    // frame had been through JPEG.
    drawIcon(t, "plus", plus.inset(4.f), ctx.pal.text,
             tokens::icon::strokeWidth + 0.5f);
}

// ---------------------------------------------------------------------------
// RecentCard
// ---------------------------------------------------------------------------

RecentCard::RecentCard(std::string t, std::string m, int accent)
    : title(std::move(t)), meta(std::move(m)), accentIndex(accent) {
    acceptsPointer = true;
    debugName = "RecentCard(" + title + ")";
}

void RecentCard::enter(double now, int indexInGrid) {
    // Bound the stagger: the last card in a full library should never take
    // a second to appear after a sort. Preserve a little row-by-row motion.
    const int delay = tokens::delay::recentCardBase +
                      tokens::delay::recentCardStep * std::min(indexInGrid, 8);
    enter_.start(now, tokens::dur::pageEnter, delay, Curve::OutCubic);
    // The progress bar fills after the card has landed.
    barDelay_ = now + (delay + tokens::dur::pageEnter) / 1000.0;
}

bool RecentCard::onPointer(const PointerEvent& e) {
    if (e.phase == PointerPhase::Down) {
        if (e.button == MouseButton::Right) {
            pressStart_ = -1.0;
            menuOpened_ = true;
            if (onMenu) onMenu(toWindow(e.pos));
            return true;
        }
        menuOpened_ = false;
        pressStart_ = e.isTouch() ? e.timestamp : -1.0;
        pressPos_ = e.pos;
        return true;
    }
    if (e.phase == PointerPhase::Move && pressStart_ >= 0.0) {
        const float dx = e.pos.x - pressPos_.x, dy = e.pos.y - pressPos_.y;
        if (dx * dx + dy * dy > 100.f) pressStart_ = -1.0;
    }
    if (e.phase == PointerPhase::Cancel || e.phase == PointerPhase::Leave) {
        pressStart_ = -1.0;
        return true;
    }
    if (e.phase == PointerPhase::Up) {
        if (pressStart_ >= 0.0 && e.timestamp - pressStart_ >= .5 &&
            hitLocal(e.pos) && !menuOpened_) {
            menuOpened_ = true;
            if (onMenu) onMenu(toWindow(e.pos));
        }
        pressStart_ = -1.0;
        if (!menuOpened_ && e.button != MouseButton::Right && hitLocal(e.pos) && onOpen)
            onOpen();
        menuOpened_ = false;
        return true;
    }
    return e.phase == PointerPhase::Enter || e.phase == PointerPhase::Move;
}

bool RecentCard::animating(double now) const {
    return (pressStart_ >= 0.0 && !menuOpened_) ||
           hoverT_.active(now) || enter_.active(now) ||
           (progress > 0.f &&
            (now - barDelay_) * 1000.0 < tokens::dur::heroBarFill);
}

void RecentCard::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    if (pressStart_ >= 0.0 && !menuOpened_ && ctx.now - pressStart_ >= .5) {
        menuOpened_ = true;
        if (onMenu) onMenu(toWindow(pressPos_));
    }
    const Rect full = localBounds();
    if (full.empty()) return;

    auto* self = const_cast<RecentCard*>(this);
    self->hoverT_.animateTo(hovered ? 1.f : 0.f, tokens::dur::hover, ctx.now);
    const float hv = hoverT_.value(ctx.now);
    const float ph = enter_.phase(ctx.now);

    // Entrance: rise 14 px and fade.
    t.save();
    t.translate(0.f, lerp(14.f, 0.f, ph));
    t.saveLayerAlpha(ph);

    // Hover lift: 3 px up, with the stronger `cardHover` elevation.
    const float lift = lerp(0.f, -3.f, hv);
    const Rect b{full.x, full.y + lift, full.w, full.h};
    const Corners rr = Corners::all(tokens::radius::cardLarge);

    drawElevation(t, b, rr, tokens::elevation::card(ctx.pal), 1.f - hv);
    drawElevation(t, b, rr, tokens::elevation::cardHover(ctx.pal), hv);
    t.fillRoundedRect(b, rr, ctx.pal.card);

    const float thumbH = b.h * 0.56f;
    const Rect thumb{b.x + 10.f, b.y + 10.f, b.w - 20.f, thumbH};
    t.fillRoundedRect(thumb, Corners::all(tokens::radius::sm),
                      lerpColor(Color{254, 254, 254, 255}, ctx.pal.surfaceSunken,
                                tokens::darkness(ctx.pal)));
    const Color accent =
        tokens::docAccent[static_cast<std::size_t>(accentIndex) % tokens::docAccent.size()];
    if (cover && cover->width > 0 && cover->height > 0 && !cover->pixels.empty()) {
        t.save();
        t.clipRoundedRect(thumb, Corners::all(tokens::radius::sm));
        const Rect image = recentCoverDestination(thumb, cover->width, cover->height);
        t.drawImageBGRA(cover->pixels.data(), cover->width, cover->height,
                        static_cast<std::size_t>(cover->width) * 4u, image,
                        cover->imageKey, IPaintTarget::ImageFit::Transient);
        t.restore();
    } else {
        // Cheap placeholder only until this file's REAL first page arrives.
        t.fillRoundedRect(Rect{thumb.x, thumb.y, 4.f, thumb.h},
                          Corners{tokens::radius::sm, 0.f, 0.f, tokens::radius::sm}, accent);
        for (int i = 0; i < 5; ++i) {
            const float ly = thumb.y + 16.f + static_cast<float>(i) * 11.f;
            if (ly > thumb.bottom() - 10.f) break;
            const float lw = (thumb.w - 34.f) * (i == 4 ? 0.5f : (i == 0 ? 0.75f : 0.92f));
            t.fillRoundedRect(Rect{thumb.x + 16.f, ly, lw, 4.f}, Corners::all(2.f),
                              fade(ctx.pal.muted, 0.3f));
        }
    }
    t.strokeRoundedRect(thumb, Corners::all(tokens::radius::sm), ctx.pal.border, 1.f);

    const float textY = thumb.bottom() + 10.f;
    t.drawTextInBox(title, Rect{b.x + 12.f, textY, b.w - 24.f, 20.f},
                    styleFor(tokens::type_bodyStrong, ctx.pal.text),
                    folio::render::TextAlign::Left);
    t.drawTextInBox(meta, Rect{b.x + 12.f, textY + 18.f, b.w - 24.f, 16.f},
                    styleFor(tokens::type_small, ctx.pal.muted),
                    folio::render::TextAlign::Left);

    // Reading-progress bar fills briefly after the card has landed.
    if (progress > 0.f) {
        const float fill =
            clamp01(static_cast<float>((ctx.now - barDelay_) * 1000.0 /
                                       tokens::dur::heroBarFill));
        const Rect track{b.x + 12.f, b.bottom() - 12.f, b.w - 24.f, 3.f};
        t.fillRoundedRect(track, Corners::all(1.5f), ctx.pal.surfaceSunken);
        t.fillRoundedRect(Rect{track.x, track.y, track.w * progress * easeStandard(fill),
                               track.h},
                          Corners::all(1.5f), accent);
    }

    t.restore();
    t.restore();
}

// ---------------------------------------------------------------------------
// Dropzone
// ---------------------------------------------------------------------------

Dropzone::Dropzone() {
    acceptsPointer = true;
    debugName = "Dropzone";
}

void Dropzone::setDragOver(bool v, double now) {
    if (v == dragOver_) return;
    dragOver_ = v;
    glow_.animateTo(v ? 1.f : 0.f, tokens::dur::hoverFast, now);
}

bool Dropzone::onPointer(const PointerEvent& e) {
    if (e.phase == PointerPhase::Up && hitLocal(e.pos) && onActivate) onActivate();
    return e.phase != PointerPhase::Move;
}

bool Dropzone::animating(double now) const { return glow_.active(now); }

void Dropzone::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    const Rect b = localBounds();
    if (b.empty()) return;
    auto* self = const_cast<Dropzone*>(this);
    self->glow_.animateTo((dragOver_ || hovered) ? 1.f : 0.f, tokens::dur::hover, ctx.now);
    const float g = glow_.value(ctx.now);

    const Corners rr = Corners::all(tokens::radius::cardLarge);
    t.fillRoundedRect(b, rr, lerpColor(fade(ctx.pal.surfaceSunken, 0.6f),
                                       ctx.pal.accentSoft, g));

    // Dashed border, drawn as segments because the paint interface has no
    // dash pattern - and adding one for a single call site would be the
    // wrong trade.
    const Color stroke = lerpColor(ctx.pal.border, ctx.pal.accent, g);
    constexpr float dash = 7.f, gap = 5.f;
    const float r = rr.tl;
    auto dashRun = [&](float x0, float y0, float x1, float y1) {
        const float dx = x1 - x0, dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.f) return;
        const float ux = dx / len, uy = dy / len;
        for (float s = 0.f; s < len; s += dash + gap) {
            const float e2 = std::min(s + dash, len);
            t.drawLine(Point{x0 + ux * s, y0 + uy * s}, Point{x0 + ux * e2, y0 + uy * e2},
                       stroke, 1.6f);
        }
    };
    dashRun(b.x + r, b.y, b.right() - r, b.y);
    dashRun(b.x + r, b.bottom(), b.right() - r, b.bottom());
    dashRun(b.x, b.y + r, b.x, b.bottom() - r);
    dashRun(b.right(), b.y + r, b.right(), b.bottom() - r);

    const float iconSz = 28.f;
    const float cy = b.h * 0.5f;
    drawIcon(t, "upload",
             Rect{(b.w - iconSz) * 0.5f, cy - 30.f + lerp(0.f, -3.f, g), iconSz, iconSz},
             lerpColor(ctx.pal.muted, ctx.pal.accent, g), 1.8f);
    t.drawTextInBox(title, Rect{0.f, cy + 2.f, b.w, 20.f},
                    styleFor(tokens::type_bodyStrong,
                             lerpColor(ctx.pal.text, ctx.pal.accentText, g)),
                    folio::render::TextAlign::Centre);
    t.drawTextInBox(subtitle, Rect{0.f, cy + 22.f, b.w, 18.f},
                    styleFor(tokens::type_small, ctx.pal.muted),
                    folio::render::TextAlign::Centre);
}

}  // namespace folio::ui
