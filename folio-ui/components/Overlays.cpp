#include <cstdio>
#include <cstdlib>
#include "components/Overlays.h"

#include <algorithm>
#include <cmath>

namespace folio::ui {
namespace {

OverlayHost* g_current = nullptr;

/// Used when no host is installed, so a widget drawn in isolation (a unit
/// test, an icon sheet) still works.
OverlayHost& nullHost() {
    static OverlayHost h;
    return h;
}

constexpr float kTipPadX = 9.f;
constexpr float kTipPadY = 6.f;
constexpr float kTipGap = 10.f;
constexpr float kToastPadX = 16.f;
constexpr float kToastH = 38.f;
constexpr float kProgressH = 3.f;

}  // namespace

OverlayHost::OverlayHost() {
    acceptsPointer = false;  // never steals input
    debugName = "OverlayHost";
}

OverlayHost& OverlayHost::current() { return g_current ? *g_current : nullHost(); }
void OverlayHost::setCurrent(OverlayHost* h) { g_current = h; }

// ---------------------------------------------------------------------------
// Tooltip
// ---------------------------------------------------------------------------

void OverlayHost::requestTooltip(Widget* owner, std::string text, double now) {
    if (tipOwner_ == owner) return;
    tipOwner_ = owner;
    tipText_ = std::move(text);
    tipArmed_ = now;
    // Anchor under the owner's bottom centre, captured now because the owner
    // may move (a toolbar can reflow) before the tooltip appears.
    if (owner) {
        tipAnchor_ = owner->toWindow(Point{owner->frame.w * 0.5f, owner->frame.h});
    }
    tipAlpha_.reset(0.f);
}

void OverlayHost::cancelTooltip(const Widget* owner) {
    if (tipOwner_ != owner) return;
    tipOwner_ = nullptr;
    tipArmed_ = 0.0;
    // Fade out rather than vanish; the prototype does the same, and it stops
    // the tooltip flickering when the pointer crosses a 1 px gap between two
    // adjacent toolbar buttons.
    tipAlpha_.animateTo(0.f, tokens::dur::tooltip, now());
}

void OverlayHost::dismissTooltip() {
    tipOwner_ = nullptr;
    tipText_.clear();
    tipAlpha_.reset(0.f);
}

namespace {
void growTo(Rect& acc, const Rect& r) {
    if (r.w <= 0.f) return;
    if (acc.w <= 0.f) { acc = r; return; }
    const float x0 = std::min(acc.x, r.x), y0 = std::min(acc.y, r.y);
    const float x1 = std::max(acc.right(), r.right());
    const float y1 = std::max(acc.bottom(), r.bottom());
    acc = Rect{x0, y0, x1 - x0, y1 - y0};
}
}  // namespace

void OverlayHost::paintTooltip(IPaintTarget& t, const PaintCtx& ctx) {
    // A hovered widget can be hidden without a Leave event (screen change,
    // rotation, a closed sheet). Also time out labels even if no pointer
    // movement follows; an old sort tooltip must never float over a PDF.
    if (tipOwner_) {
        for (const Widget* w = tipOwner_; w; w = w->parent()) {
            if (!w->visible || w->opacity <= .003f ||
                ctx.now - tipArmed_ > 2.5) {
                dismissTooltip();
                break;
            }
        }
    }
    if (tipOwner_ != nullptr) {
        const double waited = (ctx.now - tipArmed_) * 1000.0;
        if (waited >= tokens::delay::tooltipHover) {
            tipAlpha_.animateTo(1.f, tokens::dur::tooltip, ctx.now);
        }
    } else {
        tipAlpha_.animateTo(0.f, tokens::dur::tooltip, ctx.now);
    }
    const float a = tipAlpha_.value(ctx.now);
    if (a <= 0.004f || tipText_.empty()) return;

    const auto ts = styleFor(tokens::type_small, fade(tokens::fixed::tooltipText, a));
    const float tw = t.measureText(tipText_, ts);
    const auto fm = t.fontMetrics(ts);
    const float h = (-fm.ascent + fm.descent) + kTipPadY * 2.f;
    const float w = tw + kTipPadX * 2.f;

    // Rise 4 px as it fades in.
    float x = tipAnchor_.x - w * 0.5f;
    float y = tipAnchor_.y + kTipGap + lerp(4.f, 0.f, a);
    x = std::clamp(x, 6.f, std::max(6.f, frame.w - w - 6.f));
    if (y + h > frame.h - 6.f) y = tipAnchor_.y - kTipGap - h;

    const Rect box{x, y, w, h};
    growTo(painted_, box.inset(-28.f));  // shadow bleed
    const Corners rr = Corners::all(tokens::radius::sm);
    t.drawShadow(box, rr, folio::render::Shadow{0.f, 4.f, 14.f, 0.f,
                                                fade(ctx.pal.shadow, a)});
    t.fillRoundedRect(box, rr, fade(tokens::fixed::tooltipBg, a));
    t.drawTextInBox(tipText_, box, ts, folio::render::TextAlign::Centre);
}

// ---------------------------------------------------------------------------
// Toast
// ---------------------------------------------------------------------------

void OverlayHost::showToast(std::string text, double now) {
    toastText_ = std::move(text);
    toastShown_ = now;
    toastT_.reset(0.f);
    toastT_.animateTo(1.f, tokens::dur::toast, now, Curve::OutBack);
}

void OverlayHost::paintToast(IPaintTarget& t, const PaintCtx& ctx) {
    if (toastText_.empty()) return;
    const double age = (ctx.now - toastShown_) * 1000.0;
    if (age > tokens::delay::autoDismiss_toast) {
        toastT_.animateTo(0.f, tokens::dur::toast, ctx.now);
    }
    const float p = toastT_.value(ctx.now);
    if (p <= 0.004f) return;

    const auto ts = styleFor(tokens::type_body, fade(tokens::fixed::tooltipText, p));
    const float tw = t.measureText(toastText_, ts);
    const float w = tw + kToastPadX * 2.f;
    const float x = (frame.w - w) * 0.5f;
    // Slides up from below the bottom edge.
    const float y = lerp(frame.h + 10.f, frame.h - kToastH - 28.f, p);

    const Rect box{x, y, w, kToastH};
    growTo(painted_, box.inset(-28.f));  // shadow bleed
    const Corners rr = Corners::all(tokens::radius::pill);
    t.drawShadow(box, rr,
                 folio::render::Shadow{0.f, 8.f, 24.f, 0.f, fade(ctx.pal.shadow, p)});
    t.fillRoundedRect(box, rr, fade(tokens::fixed::tooltipBg, p));
    t.drawTextInBox(toastText_, box, ts, folio::render::TextAlign::Centre);
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------

void OverlayHost::setProgress(float v, double now) {
    progressVisible_ = true;
    progress_ = v;
    progressAlpha_.animateTo(1.f, tokens::dur::hoverFast, now);
    if (v >= 0.f) progressValue_.animateTo(v, tokens::dur::progressBar, now);
}

void OverlayHost::hideProgress(double now) {
    progressVisible_ = false;
    progressAlpha_.animateTo(0.f, tokens::dur::hover, now);
}

void OverlayHost::paintProgress(IPaintTarget& t, const PaintCtx& ctx) {
    growTo(painted_, Rect{0.f, 0.f, frame.w, 6.f});
    const float a = progressAlpha_.value(ctx.now);
    if (a <= 0.004f) return;

    const Rect track{0.f, 0.f, frame.w, kProgressH};
    t.fillRect(track, fade(ctx.pal.accentSoft, a));

    if (progress_ >= 0.f) {
        const float v = progressValue_.value(ctx.now);
        t.fillRect(Rect{0.f, 0.f, frame.w * clamp01(v), kProgressH},
                   fade(ctx.pal.accent, a));
    } else {
        // Indeterminate: a 28%-wide bar sweeping across, easing at both ends
        // so it does not look like a constant-velocity scanner.
        const float ph = loopPhase(ctx.now, 1200);
        const float barW = frame.w * 0.28f;
        const float x = lerp(-barW, frame.w, easeStandard(ph));
        t.fillRect(Rect{x, 0.f, barW, kProgressH}, fade(ctx.pal.accent, a));
    }
}

Rect OverlayHost::damageBounds() const {
    // The overlay layer is window-sized but only ever draws a tooltip, a
    // toast or the progress bar. Reporting the whole window here would make
    // every hover mark the window damaged.
    return painted_.w > 0.f ? painted_ : Rect{0.f, 0.f, 0.f, 0.f};
}

void OverlayHost::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    // Recomputed by the paint helpers below.
    painted_ = Rect{0.f, 0.f, 0.f, 0.f};
    paintProgress(t, ctx);
    paintTooltip(t, ctx);
    paintToast(t, ctx);
}

bool OverlayHost::animating(double now) const {
    if (tipAlpha_.active(now) || toastT_.active(now) || progressAlpha_.active(now) ||
        progressValue_.active(now)) {
        return true;
    }
    // An armed-but-not-yet-shown tooltip needs frames so the delay can expire.
    if (tipOwner_ && (now - tipArmed_) < 2.55) return true;
    // An indeterminate bar animates forever while visible.
    if (progressVisible_ && progress_ < 0.f) return true;
    // A visible toast needs frames until its auto-dismiss fires.
    if (!toastText_.empty() && toastT_.target() > 0.f &&
        (now - toastShown_) * 1000.0 <= tokens::delay::autoDismiss_toast + 50) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Popover
// ---------------------------------------------------------------------------

Popover::Popover() {
    acceptsPointer = true;  // swallow clicks so they do not fall through
    visible = false;
    debugName = "Popover";
    // A menu is above the chrome it was opened from, always. Declaring it
    // here means no caller can forget, and an overlaying side panel can no
    // longer paint over an open dropdown.
    setZOrder(zlayer::popover);
}

void Popover::open(Point anchorWindow, double now) {
    anchor_ = anchorWindow;
    open_ = true;
    visible = true;
    reveal_.animateTo(1.f, tokens::dur::popoverReveal, now, Curve::OutBack);

    // Place below the anchor, flipping above if there is no room. Clamp
    // horizontally so a popover anchored to a right-edge button stays
    // on-screen.
    float x = anchor_.x - frame.w * 0.5f;
    x = std::clamp(x, 8.f, std::max(8.f, windowSize.w - frame.w - 8.f));
    float y;
    if (preferAbove) {
        y = anchor_.y - frame.h - 10.f;
        if (y < 8.f) y = anchor_.y + 14.f;       // no room above: drop below
    } else {
        y = anchor_.y + 8.f;
        if (y + frame.h > windowSize.h - 8.f) y = anchor_.y - frame.h - 8.f;
    }
    frame.x = x;
    frame.y = std::clamp(y, 8.f,
                         std::max(8.f, windowSize.h - frame.h - 8.f));

    // The reveal grows from the anchor itself, projected onto the card.
    //
    // Previously the vertical origin was picked from a `flipped` flag,
    // which only told the truth when the caller let open() do the
    // flipping. A caller that pre-offset its anchor to place the popover
    // above a button looked un-flipped, so the reveal grew from the top
    // edge - the far side from the control that was just clicked.
    // Clamping the anchor into local bounds needs no flag and is right for
    // every placement, including one that was clamped sideways to stay on
    // screen.
    revealFrom_ = Point{std::clamp(anchor_.x - frame.x, 0.f, frame.w),
                        std::clamp(anchor_.y - frame.y, 0.f, frame.h)};
    clipsContent = true;

    // Placement diagnostic. Popovers are white cards on white pages, which
    // defeats pixel-sampling entirely, so geometry questions get answered from
    // numbers rather than screenshots. getenv is resolved once.
    static const bool kDebugPop = std::getenv("FOLIO_DEBUG_POP") != nullptr;
    if (kDebugPop) {
        std::fprintf(stderr,
                     "[pop] win=%.0fx%.0f anchor=(%.0f,%.0f) frame=(%.0f,%.0f "
                     "%.0fx%.0f) reveal=(%.0f,%.0f) above=%d  right=%.0f\n",
                     windowSize.w, windowSize.h, anchor_.x, anchor_.y, frame.x,
                     frame.y, frame.w, frame.h, revealFrom_.x, revealFrom_.y,
                     preferAbove ? 1 : 0, frame.x + frame.w);
    }
}

void Popover::constrainToWindow() {
    if (!visible) return;
    frame.x = std::clamp(frame.x, 8.f,
                         std::max(8.f, windowSize.w - frame.w - 8.f));
    frame.y = std::clamp(frame.y, 8.f,
                         std::max(8.f, windowSize.h - frame.h - 8.f));
    revealFrom_ = Point{std::clamp(anchor_.x - frame.x, 0.f, frame.w),
                        std::clamp(anchor_.y - frame.y, 0.f, frame.h)};
}

void Popover::close(double now) {
    if (!open_) return;
    open_ = false;
    reveal_.animateTo(0.f, tokens::dur::hoverFast, now);
    if (onClosed) onClosed();
}

Rect Popover::contentRect() const {
    return localBounds().inset(tokens::space::pad);
}

void Popover::dismissIfOutside(Point windowPoint, double now) {
    if (!open_) return;
    const Point o = toWindow(Point{0.f, 0.f});
    const bool inside = windowPoint.x >= o.x && windowPoint.y >= o.y &&
                        windowPoint.x < o.x + frame.w && windowPoint.y < o.y + frame.h;
    if (inside) return;

    // A press on the control that opened this popover is not an outside
    // click. Without this the press closed the popover and the button's
    // own click reopened it on release, so clicking the button to dismiss
    // did nothing - it took a second click somewhere else first.
    if (owner_ && owner_->visible) {
        const Point w0 = owner_->toWindow(Point{0.f, 0.f});
        if (windowPoint.x >= w0.x && windowPoint.y >= w0.y &&
            windowPoint.x < w0.x + owner_->frame.w &&
            windowPoint.y < w0.y + owner_->frame.h) {
            return;
        }
    }
    close(now);
}

void Popover::applyContentClip(IPaintTarget& t, const PaintCtx& ctx) {
    const float p = std::clamp(reveal_.value(ctx.now), 0.f, 1.f);
    const Rect b = localBounds();
    // Radius that just covers the farthest corner, plus slack so the card's
    // drop shadow is not clipped once the reveal has finished.
    float maxD = 0.f;
    const float cx = revealFrom_.x, cy = revealFrom_.y;
    for (const Point& c : {Point{0.f, 0.f}, Point{b.w, 0.f}, Point{0.f, b.h},
                           Point{b.w, b.h}}) {
        const float dx = c.x - cx, dy = c.y - cy;
        maxD = std::max(maxD, std::sqrt(dx * dx + dy * dy));
    }
    maxD += 44.f;
    const float r = maxD * p;
    t.clipRoundedRect(Rect{cx - r, cy - r, r * 2.f, r * 2.f}, Corners::all(r), true);
}

bool Popover::onPointer(const PointerEvent&) { return true; }

bool Popover::onKey(const KeyEvent& e) {
    if (e.pressed && e.key == Key::Escape && open_) {
        close(0.0);
        return true;
    }
    return false;
}

bool Popover::animating(double now) const { return reveal_.active(now); }

void Popover::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    const float p = reveal_.value(ctx.now);
    if (p <= 0.004f) {
        if (!open_) const_cast<Popover*>(this)->visible = false;
        return;
    }
    const Rect b = localBounds();
    const Corners rr = Corners::all(tokens::radius::popover);

    // Scale from 96% about the top edge nearest the anchor, which reads as
    // "growing out of the button" rather than "zooming from the middle".
    t.save();
    t.saveLayerAlpha(std::min(1.f, p * 2.2f));

    drawCard(t, b, rr, ctx.pal.card, tokens::elevation::card(ctx.pal));

    t.restore();
    t.restore();
}

}  // namespace folio::ui
