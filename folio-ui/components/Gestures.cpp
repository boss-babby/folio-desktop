#include "components/Gestures.h"

#include <algorithm>

namespace folio::ui {

namespace {
float dist(Point a, Point b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}
}  // namespace

ActivePointer* GestureTracker::find(PointerId id) {
    for (auto& p : pointers_)
        if (p.id == id) return &p;
    return nullptr;
}

bool GestureTracker::onPointer(const PointerEvent& e) {
    panDelta_ = Point{0.f, 0.f};
    pinch_ = PinchState{};
    wasTap_ = false;

    switch (e.phase) {
        case PointerPhase::Down: {
            // A platform can repeat a down for an id we already hold if an
            // up was lost (window deactivation mid-gesture is the usual
            // way). Treat it as a fresh contact rather than growing the
            // list forever.
            ActivePointer* slot = find(e.pointerId);
            if (!slot) {
                pointers_.push_back(ActivePointer{});
                slot = &pointers_.back();
            }
            ActivePointer& p = *slot;
            p.id = e.pointerId;
            p.type = e.pointerType;
            p.start = p.pos = p.prev = e.pos;
            p.startTime = p.lastTime = e.timestamp;
            p.velocity = Point{0.f, 0.f};
            p.pressure = e.pressure;
            p.moved = false;
            longPressFired_ = false;
            classify();
            if (pointers_.size() == 2) {
                startSpan_ = lastSpan_ = dist(pointers_[0].pos, pointers_[1].pos);
            }
            return true;
        }

        case PointerPhase::Move: {
            ActivePointer* p = find(e.pointerId);
            if (!p) return false;
            const double dt = std::max(1e-4, e.timestamp - p->lastTime);
            const Point d{e.pos.x - p->pos.x, e.pos.y - p->pos.y};

            p->prev = p->pos;
            p->pos = e.pos;
            p->lastTime = e.timestamp;
            p->pressure = e.pressure;
            if (dist(p->start, p->pos) > gesture::kSlop) p->moved = true;

            // Smoothed, so one stuttered frame at release does not fling
            // the page across the screen.
            const float a = gesture::kVelocityAlpha;
            const Point inst{static_cast<float>(d.x / dt),
                             static_cast<float>(d.y / dt)};
            p->velocity = Point{p->velocity.x * (1.f - a) + inst.x * a,
                                p->velocity.y * (1.f - a) + inst.y * a};

            if (pointers_.size() >= 2) {
                const Point a0 = pointers_[0].pos, b0 = pointers_[1].pos;
                const Point ap = pointers_[0].prev, bp = pointers_[1].prev;
                const float span = dist(a0, b0);
                const Point c{(a0.x + b0.x) * 0.5f, (a0.y + b0.y) * 0.5f};
                const Point cp{(ap.x + bp.x) * 0.5f, (ap.y + bp.y) * 0.5f};
                pinch_.centroid = c;
                pinch_.centroidDelta = Point{c.x - cp.x, c.y - cp.y};
                // Guard the degenerate case: two fingers landing on the
                // same pixel gives span 0 and an infinite scale.
                pinch_.scale = (lastSpan_ > 1.f && span > 1.f) ? span / lastSpan_ : 1.f;
                pinch_.totalScale =
                    (startSpan_ > 1.f && span > 1.f) ? span / startSpan_ : 1.f;
                lastSpan_ = span;
            } else {
                panDelta_ = d;
            }
            return true;
        }

        case PointerPhase::Up:
        case PointerPhase::Cancel: {
            ActivePointer* p = find(e.pointerId);
            if (!p) {
                if (e.phase == PointerPhase::Cancel) clear();
                return false;
            }
            const bool tap = !p->moved &&
                             (e.timestamp - p->startTime) < gesture::kLongPress;
            if (tap && e.phase == PointerPhase::Up) {
                wasTap_ = true;
                // Against the *previous* tap, not itself - a double tap is
                // two taps in the same place, and comparing a point with
                // itself makes every tap a double one.
                const bool near = lastTapTime_ > 0.0 &&
                                  dist(p->pos, lastTapPos_) < gesture::kSlop * 2.f;
                tapCount_ = (near && e.timestamp - lastTapTime_ < gesture::kDoubleTap)
                                ? tapCount_ + 1
                                : 1;
                tapPos_ = p->pos;
                lastTapPos_ = p->pos;
                lastTapTime_ = e.timestamp;
            }
            // Fling only from the last pointer up, and only if it was
            // actually moving. Releasing one finger of a pinch must not
            // launch the page.
            const float speed = std::sqrt(p->velocity.x * p->velocity.x +
                                          p->velocity.y * p->velocity.y);
            fling_ = (pointers_.size() == 1 && p->moved &&
                      speed > gesture::kFlingMin && e.phase == PointerPhase::Up)
                         ? p->velocity
                         : Point{0.f, 0.f};

            pointers_.erase(std::remove_if(pointers_.begin(), pointers_.end(),
                                           [&](const ActivePointer& q) {
                                               return q.id == e.pointerId;
                                           }),
                            pointers_.end());
            // Dropping from two fingers to one must not make the remaining
            // finger jump the page by its whole offset: restart its frame
            // of reference where it currently is.
            if (pointers_.size() == 1) {
                pointers_[0].prev = pointers_[0].pos;
                pointers_[0].start = pointers_[0].pos;
            }
            classify();
            return true;
        }

        default:
            return false;
    }
}

void GestureTracker::classify() {
    if (pointers_.size() >= 2) kind_ = GestureKind::Pinch;
    else if (pointers_.size() == 1) kind_ = GestureKind::Pan;
    else kind_ = GestureKind::None;
}

bool GestureTracker::pollLongPress(double now) {
    if (longPressFired_ || pointers_.size() != 1) return false;
    const ActivePointer& p = pointers_.front();
    if (p.moved || p.type == PointerType::Mouse) return false;
    if (now - p.startTime < gesture::kLongPress) return false;
    longPressFired_ = true;
    return true;
}

}  // namespace folio::ui
