// folio-ui/components/Gestures.h
//
// Multi-touch gesture recognition.
//
// WHY A RECOGNISER AND NOT just reading PointerEvents
// ---------------------------------------------------
// A widget can handle a mouse by looking at one event at a time: down,
// move, up. Touch is not like that. "Pinch" is a statement about two
// pointers over time, "fling" is a statement about velocity at the moment
// of release, and "long press" is a statement about a pointer that did
// *not* move. None of those can be decided from the event in your hand.
//
// So the tracker owns the small amount of state those questions need -
// which pointers are down, where they started, how fast they were going -
// and answers them. Widgets ask questions instead of accumulating state,
// which keeps the state in one tested place rather than smeared across
// every component that wants to be touchable.
//
// Deliberately framework-free: no Skia, no platform headers, just the
// project's own PointerEvent. The same recogniser serves the X11 backend,
// the streaming shell's browser transport, and eventually Android.

#pragma once

#include "folio/ui/Event.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace folio::ui {

/// One pointer currently in contact.
struct ActivePointer {
    PointerId id = 0;
    PointerType type = PointerType::Touch;
    Point start{};
    Point pos{};
    Point prev{};
    double startTime = 0.0;
    double lastTime = 0.0;
    /// Exponentially smoothed velocity in points/second. Raw frame deltas
    /// are far too noisy to fling from: one stuttered frame at the moment
    /// of release otherwise throws the page across the screen.
    Point velocity{};
    float pressure = 1.f;
    bool moved = false;
};

/// What the tracker decided this gesture is.
enum class GestureKind : std::uint8_t {
    None,
    /// One pointer, past the slop radius: drag/pan.
    Pan,
    /// Two pointers: scale about a centroid, which also pans.
    Pinch,
};

struct PinchState {
    Point centroid{};      ///< midpoint, in the widget's local coordinates
    Point centroidDelta{}; ///< movement of that midpoint since the last frame
    float scale = 1.f;     ///< multiplicative change since the last frame
    float totalScale = 1.f;///< multiplicative change since the gesture began
};

/// Tunables. Named rather than inlined because every one of them is a
/// judgement call that someone will want to argue with on real hardware.
namespace gesture {
/// How far a pointer may wander before it stops being a tap. 10pt is the
/// usual figure; below ~8 a thumb on a bumpy train registers as a drag.
inline constexpr float kSlop = 10.f;
/// How long a stationary pointer must stay down to be a long press.
inline constexpr double kLongPress = 0.5;
/// Two taps closer together than this, within slop, are a double tap.
inline constexpr double kDoubleTap = 0.3;
/// Below this speed at release there is no fling, just a stop.
inline constexpr float kFlingMin = 160.f;
/// A short Android finger flick should advance a viewport, not dozens of
/// pages. Clamp the input velocity as well as the stage's final fling.
inline constexpr float kFlingMax = 2600.f;
/// Velocity smoothing. Lower trusts history more.
inline constexpr float kVelocityAlpha = 0.35f;
}  // namespace gesture

/// Tracks the pointers in contact with one widget and classifies them.
///
/// Feed it every PointerEvent the widget receives. Mouse events are
/// tracked too - a mouse drag is a one-pointer pan - so a caller that
/// wants uniform treatment gets it, while a caller that wants to keep
/// mouse behaviour exactly as it was can test `isTouchGesture()`.
class GestureTracker {
public:
    /// Returns true if this event changed the pointer set.
    bool onPointer(const PointerEvent& e);

    void clear() { pointers_.clear(); kind_ = GestureKind::None; }

    [[nodiscard]] std::size_t count() const noexcept { return pointers_.size(); }
    [[nodiscard]] bool longPressPending() const noexcept {
        return !longPressFired_ && pointers_.size() == 1 &&
               !pointers_.front().moved &&
               pointers_.front().type == PointerType::Touch;
    }
    [[nodiscard]] GestureKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::vector<ActivePointer>& pointers() const noexcept {
        return pointers_;
    }

    /// True once at least one tracked pointer is a finger. Pens and mice
    /// are excluded: a pen is a precision device and must not inherit
    /// touch's pan-instead-of-select behaviour.
    [[nodiscard]] bool isTouchGesture() const noexcept {
        for (const auto& p : pointers_)
            if (p.type == PointerType::Touch) return true;
        return false;
    }

    /// Pinch geometry for this frame. Only meaningful while kind() is
    /// Pinch; returns an identity state otherwise.
    [[nodiscard]] PinchState pinch() const noexcept { return pinch_; }

    /// Single-pointer movement since the previous event.
    [[nodiscard]] Point panDelta() const noexcept { return panDelta_; }

    /// Velocity of the pointer that just lifted, points/second. Zero when
    /// it was too slow to count as a fling.
    [[nodiscard]] Point flingVelocity() const noexcept { return fling_; }

    /// True when the pointer that just lifted was a tap (never left slop).
    [[nodiscard]] bool wasTap() const noexcept { return wasTap_; }
    [[nodiscard]] int tapCount() const noexcept { return tapCount_; }
    [[nodiscard]] Point tapPos() const noexcept { return tapPos_; }

    /// Poll from the frame loop: true exactly once, when a pointer has been
    /// held still past the long-press threshold.
    bool pollLongPress(double now);

private:
    ActivePointer* find(PointerId id);
    void classify();

    std::vector<ActivePointer> pointers_;
    GestureKind kind_ = GestureKind::None;
    PinchState pinch_{};
    Point panDelta_{};
    Point fling_{};
    float lastSpan_ = 0.f;
    float startSpan_ = 0.f;
    bool wasTap_ = false;
    bool longPressFired_ = false;
    int tapCount_ = 0;
    Point tapPos_{};
    Point lastTapPos_{};
    double lastTapTime_ = -1.0;
};

}  // namespace folio::ui
