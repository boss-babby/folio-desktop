#include "folio/ui/Widget.h"

#include <algorithm>

namespace folio::ui {

// ---------------------------------------------------------------------------
// Widget
// ---------------------------------------------------------------------------

Widget* Widget::adopt(std::unique_ptr<Widget> child) {
    if (!child) return nullptr;
    child->parent_ = this;
    children_.push_back(std::move(child));
    // A child may already carry a layer (Popover sets one in its ctor), so
    // the list cannot be assumed sorted just because it was before.
    childOrderDirty_ = true;
    return children_.back().get();
}

void Widget::setZOrder(int z) {
    if (z == zOrder_) return;
    zOrder_ = z;
    if (parent_) parent_->childOrderDirty_ = true;
}

void Widget::ensureChildOrder() {
    if (!childOrderDirty_) return;
    childOrderDirty_ = false;
    // Stable, so widgets sharing a layer keep insertion order - which is
    // what every existing caller relies on, raiseToFront() included.
    std::stable_sort(children_.begin(), children_.end(),
                     [](const std::unique_ptr<Widget>& a,
                        const std::unique_ptr<Widget>& b) {
                         return a->zOrder_ < b->zOrder_;
                     });
}

void Widget::raiseToFront() {
    if (!parent_) return;
    auto& v = parent_->children_;
    const auto it = std::find_if(
        v.begin(), v.end(),
        [this](const std::unique_ptr<Widget>& c) { return c.get() == this; });
    if (it == v.end() || std::next(it) == v.end()) return;
    auto owned = std::move(*it);
    v.erase(it);
    v.push_back(std::move(owned));
}

Point Widget::toWindow(Point local) const noexcept {
    const Widget* w = this;
    while (w) {
        local.x += w->frame.x;
        local.y += w->frame.y;
        w = w->parent_;
    }
    return local;
}

bool Widget::hitLocal(Point local) const {
    return local.x >= 0.f && local.y >= 0.f && local.x < frame.w && local.y < frame.h;
}

void Widget::layoutTree() {
    onLayout();
    ensureChildOrder();
    for (auto& c : children_) c->layoutTree();
}

void Widget::applyContentClip(IPaintTarget& t, const PaintCtx&) {
    t.clipRect(localBounds(), true);
}

void Widget::paintTree(IPaintTarget& t, const PaintCtx& ctx) {
    if (!visible || opacity <= 0.003f) return;

    setClock(ctx.now);

    t.save();
    t.translate(frame.x, frame.y);

    const bool layered = opacity < 0.997f;
    if (layered) t.saveLayerAlpha(opacity);

    // The clip has to cover onPaint too, not just the children: list rows
    // and pages are drawn by the container itself.
    if (clipsContent) {
        t.save();
        applyContentClip(t, ctx);
    }

    onPaint(t, ctx);
    ensureChildOrder();
    for (auto& c : children_) c->paintTree(t, ctx);
    onPaintOverlay(t, ctx);

    if (clipsContent) t.restore();

    if (layered) t.restore();
    t.restore();
}

Widget* Widget::hitTest(Point parentPoint) {
    if (!visible || !enabled) return nullptr;
    const Point local{parentPoint.x - frame.x, parentPoint.y - frame.y};

    // A clipping container hides anything outside itself, so stop early -
    // otherwise a scrolled-out list row would still catch clicks.
    if (clipsContent && !hitLocal(local)) return nullptr;

    // Later children paint on top, so they are tested first. Same order
    // the painter used, or the topmost widget would not be the one clicked.
    ensureChildOrder();
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if (Widget* hit = (*it)->hitTest(local)) return hit;
    }
    if (acceptsPointer && hitLocal(local)) return this;
    return nullptr;
}

bool Widget::treeAnimating(double now) const {
    if (!visible) return false;
    if (animating(now)) return true;
    return std::any_of(children_.begin(), children_.end(),
                       [now](const auto& c) { return c->treeAnimating(now); });
}

void Widget::forEach(const std::function<void(Widget&)>& fn) {
    fn(*this);
    for (auto& c : children_) c->forEach(fn);
}

// ---------------------------------------------------------------------------
// EventRouter
// ---------------------------------------------------------------------------

PointerEvent EventRouter::localise(const PointerEvent& e, Widget& target) {
    PointerEvent out = e;
    const Point origin = target.toWindow(Point{0.f, 0.f});
    out.pos = Point{e.windowPos.x - origin.x, e.windowPos.y - origin.y};
    return out;
}

void EventRouter::forget(const Widget* w) {
    if (hover_ == w) hover_ = nullptr;
    if (capture_ == w) capture_ = nullptr;
    if (focus_ == w) focus_ = nullptr;
}

void EventRouter::setFocus(Widget* w) {
    if (focus_ == w) return;
    Widget* prev = focus_;
    focus_ = w;
    // Flags first, then notify, so a handler that inspects `focused` on
    // either widget sees the settled state rather than a half-applied one.
    if (prev) prev->focused = false;
    if (focus_) focus_->focused = true;
    if (prev) prev->onFocusChanged(false);
    if (focus_) focus_->onFocusChanged(true);
}

bool EventRouter::dispatchPointer(Widget& root, const PointerEvent& windowEvent) {
    PointerEvent e = windowEvent;
    e.pos = e.windowPos;

    // The pointer leaving the window must clear hover everywhere, or a button
    // stays lit after the user alt-tabs away.
    if (e.phase == PointerPhase::Cancel) {
        if (hover_) {
            PointerEvent leave = e;
            leave.phase = PointerPhase::Leave;
            hover_->hovered = false;
            hover_->onPointer(leave);
            hover_ = nullptr;
        }
        if (capture_) {
            capture_->pressed = false;
            capture_ = nullptr;
        }
        return true;
    }

    // --- captured drag ----------------------------------------------------
    if (capture_) {
        Widget* target = capture_;
        PointerEvent le = localise(e, *target);
        const bool consumed = target->onPointer(le);
        if (e.phase == PointerPhase::Up) {
            target->pressed = false;
            capture_ = nullptr;
            // Re-evaluate hover: the pointer may have been released somewhere
            // else entirely.
            Widget* under = root.hitTest(e.windowPos);
            if (under != hover_) {
                if (hover_) {
                    PointerEvent lv = e;
                    lv.phase = PointerPhase::Leave;
                    hover_->hovered = false;
                    hover_->onPointer(lv);
                }
                hover_ = under;
                if (hover_) {
                    PointerEvent en = localise(e, *hover_);
                    en.phase = PointerPhase::Enter;
                    hover_->hovered = true;
                    hover_->onPointer(en);
                }
            }
        }
        return consumed;
    }

    Widget* under = root.hitTest(e.windowPos);

    // --- hover transitions --------------------------------------------------
    if (under != hover_) {
        if (hover_) {
            PointerEvent lv = localise(e, *hover_);
            lv.phase = PointerPhase::Leave;
            hover_->hovered = false;
            hover_->onPointer(lv);
        }
        hover_ = under;
        if (hover_) {
            PointerEvent en = localise(e, *hover_);
            en.phase = PointerPhase::Enter;
            hover_->hovered = true;
            hover_->onPointer(en);
        }
    }

    if (!under) return false;

    PointerEvent le = localise(e, *under);
    if (e.phase == PointerPhase::Down) {
        under->pressed = true;
        capture_ = under;
        setFocus(under);
    }

    // Wheel events bubble: the hovered widget may be a button inside a
    // scrollable list, and the list is what should scroll.
    if (e.phase == PointerPhase::Wheel) {
        Widget* w = under;
        while (w) {
            PointerEvent we = localise(e, *w);
            if (w->onPointer(we)) return true;
            w = w->parent();
        }
        return false;
    }

    return under->onPointer(le);
}

bool EventRouter::dispatchKey(Widget& root, const KeyEvent& e) {
    // Focused widget first, then bubble to ancestors, then the root as a
    // last resort for global shortcuts.
    for (Widget* w = focus_; w; w = w->parent()) {
        if (w->onKey(e)) return true;
    }
    return focus_ != &root && root.onKey(e);
}

}  // namespace folio::ui
