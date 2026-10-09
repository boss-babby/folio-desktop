// folio-ui/include/folio/ui/Animation.h
//
// The animation system. This is the part that has to reproduce the
// prototype's 65 CSS transitions and keyframe animations at 60 FPS.
//
// DESIGN
// ------
// The prototype animates with CSS `transition`, which has a very specific
// semantics that is easy to get subtly wrong when reimplementing:
//
//   1. A transition interpolates from the CURRENT rendered value, not from
//      the previous target. Hovering out halfway through a hover-in must
//      reverse from the middle, not snap to the end and then run backwards.
//   2. Retargeting restarts the clock with the full duration. (CSS does not
//      scale the remaining time; neither do we, because matching the
//      prototype exactly is the acceptance criterion.)
//   3. An element with no transition in flight costs nothing.
//
// `Animated<T>` implements exactly that in 40 lines, and because it stores
// only a start time and endpoints it is 24 bytes and trivially copyable - a
// widget can hold a dozen without anyone noticing.
//
// Everything is driven by an explicit `now` passed down from the frame loop,
// never by reading a clock inside the getter. That makes a frame internally
// consistent (two widgets in the same frame always agree what time it is) and
// makes the golden-image tests reproducible by passing a fixed time.

#pragma once

#include "folio/ui/Tokens.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace folio::ui {

// ---------------------------------------------------------------------------
// Easing
// ---------------------------------------------------------------------------

/// Evaluate a CSS cubic-bezier(x1,y1,x2,y2) easing curve at progress `t`.
///
/// CSS defines the curve parametrically with control points (0,0), (x1,y1),
/// (x2,y2), (1,1), and `t` is the *x* coordinate, not the curve parameter.
/// So we must invert x(s) = t before evaluating y(s). Newton-Raphson
/// converges in 3-4 iterations over the whole useful domain; the bisection
/// fallback guards the near-vertical curves where the derivative vanishes.
[[nodiscard]] float cubicBezier(float x1, float y1, float x2, float y2, float t) noexcept;

/// The single easing curve the prototype uses everywhere:
/// cubic-bezier(0.42, 0, 0.58, 1), i.e. CSS `ease-in-out`.
[[nodiscard]] inline float easeStandard(float t) noexcept {
    return cubicBezier(tokens::kEaseStandard[0], tokens::kEaseStandard[1],
                       tokens::kEaseStandard[2], tokens::kEaseStandard[3], t);
}

/// Overshoot curve for the few places the prototype uses a back-out feel
/// (the toast drop and the annotation stamp).
[[nodiscard]] inline float easeOutBack(float t) noexcept {
    constexpr float c1 = 1.70158f, c3 = c1 + 1.f;
    const float u = t - 1.f;
    return 1.f + c3 * u * u * u + c1 * u * u;
}

[[nodiscard]] inline float easeOutCubic(float t) noexcept {
    const float u = 1.f - t;
    return 1.f - u * u * u;
}

enum class Curve : std::uint8_t { Standard, OutBack, OutCubic, Linear };

[[nodiscard]] inline float applyCurve(Curve c, float t) noexcept {
    switch (c) {
        case Curve::Standard: return easeStandard(t);
        case Curve::OutBack:  return easeOutBack(t);
        case Curve::OutCubic: return easeOutCubic(t);
        case Curve::Linear:   break;
    }
    return t;
}

// ---------------------------------------------------------------------------
// Reduced motion
// ---------------------------------------------------------------------------

/// Global reduced-motion flag, honoured by every Animated. The token file
/// specifies durationScale = 0.0, i.e. transitions become instant rather than
/// merely faster - which is what the accessibility guidance actually asks for.
class Motion {
public:
    static void setReduced(bool r) noexcept { reduced_ = r; }
    [[nodiscard]] static bool reduced() noexcept { return reduced_; }
    [[nodiscard]] static int scale(int durationMs) noexcept {
        return reduced_ ? 0 : durationMs;
    }

private:
    inline static bool reduced_ = false;
};

// ---------------------------------------------------------------------------
// Animated<T>
// ---------------------------------------------------------------------------

/// A value that eases toward a target. `T` must support `a + (b - a) * f`.
template <typename T>
class Animated {
public:
    Animated() = default;
    explicit Animated(T initial) : from_(initial), to_(initial) {}

    /// Jump with no animation (initial layout, or an explicit reset).
    void reset(T v) noexcept {
        from_ = to_ = v;
        durationMs_ = 0;
    }

    /// Ease toward `target`. If already heading there, does nothing, so
    /// calling this every frame from a hover handler is free.
    void animateTo(T target, int durationMs, double now,
                   Curve curve = Curve::Standard) noexcept {
        if (to_ == target && durationMs_ > 0) return;   // already in flight
        if (to_ == target && durationMs_ == 0) return;  // already settled there
        from_ = value(now);                             // rule 1: from current
        to_ = target;
        durationMs_ = Motion::scale(durationMs);
        start_ = now;
        curve_ = curve;
        if (durationMs_ <= 0) from_ = to_;
    }

    [[nodiscard]] T value(double now) const noexcept {
        if (durationMs_ <= 0) return to_;
        const double elapsed = (now - start_) * 1000.0;
        if (elapsed >= durationMs_) return to_;
        if (elapsed <= 0.0) return from_;
        const float t = static_cast<float>(elapsed / durationMs_);
        const float e = applyCurve(curve_, t);
        return static_cast<T>(from_ + (to_ - from_) * e);
    }

    [[nodiscard]] bool active(double now) const noexcept {
        return durationMs_ > 0 && (now - start_) * 1000.0 < durationMs_;
    }

    [[nodiscard]] T target() const noexcept { return to_; }

private:
    T from_{};
    T to_{};
    double start_ = 0.0;
    int durationMs_ = 0;
    Curve curve_ = Curve::Standard;
};

using AnimatedF = Animated<float>;

// ---------------------------------------------------------------------------
// One-shot and repeating timelines
// ---------------------------------------------------------------------------

/// A timeline that runs 0 -> 1 once when triggered. Used for entrance
/// animations (the staggered recent-cards, the page-enter sequence) where
/// there is no "current value" to ease from, just a phase.
class Timeline {
public:
    void start(double now, int durationMs, int delayMs = 0,
               Curve curve = Curve::Standard) noexcept {
        start_ = now + delayMs / 1000.0;
        durationMs_ = Motion::scale(durationMs);
        curve_ = curve;
        running_ = true;
    }
    void finish() noexcept { running_ = false; }

    /// Eased 0..1. Returns 1 when not running, so a widget that was never
    /// animated renders in its final state.
    [[nodiscard]] float phase(double now) const noexcept {
        if (!running_ || durationMs_ <= 0) return 1.f;
        const double elapsed = (now - start_) * 1000.0;
        if (elapsed <= 0.0) return 0.f;
        if (elapsed >= durationMs_) return 1.f;
        return applyCurve(curve_, static_cast<float>(elapsed / durationMs_));
    }
    [[nodiscard]] bool active(double now) const noexcept {
        return running_ && durationMs_ > 0 && (now - start_) * 1000.0 < durationMs_;
    }

private:
    double start_ = 0.0;
    int durationMs_ = 0;
    bool running_ = false;
    Curve curve_ = Curve::Standard;
};

/// Continuously looping 0..1 ramp, for @keyframes-style animations (the
/// indeterminate progress bar, the theme-icon spin).
[[nodiscard]] inline float loopPhase(double now, int periodMs) noexcept {
    if (periodMs <= 0) return 0.f;
    const double p = periodMs / 1000.0;
    return static_cast<float>(std::fmod(std::max(0.0, now), p) / p);
}

// ---------------------------------------------------------------------------
// Interpolation helpers
// ---------------------------------------------------------------------------

[[nodiscard]] inline float lerp(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}

[[nodiscard]] inline float clamp01(float v) noexcept {
    return std::clamp(v, 0.f, 1.f);
}

/// Smooth 0..1 ramp over [edge0, edge1], matching GLSL smoothstep.
[[nodiscard]] inline float smoothstep(float edge0, float edge1, float x) noexcept {
    if (edge1 <= edge0) return x < edge0 ? 0.f : 1.f;
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.f - 2.f * t);
}

/// Colour interpolation in straight (non-premultiplied) sRGB.
///
/// Blending in sRGB rather than linear light is "wrong" physically but it is
/// what CSS does, and the requirement is to match the prototype, not to be
/// photometrically correct. Doing this in linear space makes the theme
/// cross-fade pass through a visibly lighter midpoint than the original.
[[nodiscard]] render::Color lerpColor(const render::Color& a, const render::Color& b,
                                      float t) noexcept;

/// Interpolate a whole palette. This is what drives the 760 ms theme
/// transition: rather than cross-fading two fully rendered frames (which
/// costs a full-screen layer and double the paint), we interpolate the
/// palette and paint once.
[[nodiscard]] tokens::Palette lerpPalette(const tokens::Palette& a,
                                          const tokens::Palette& b, float t) noexcept;

[[nodiscard]] inline render::Rect lerpRect(const render::Rect& a, const render::Rect& b,
                                           float t) noexcept {
    return render::Rect{lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.w, b.w, t),
                        lerp(a.h, b.h, t)};
}

}  // namespace folio::ui
