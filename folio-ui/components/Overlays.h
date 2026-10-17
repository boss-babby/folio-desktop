// folio-ui/components/Overlays.h
//
// Things that float above the rest of the UI: tooltips, toasts, popovers and
// the indeterminate progress bar.
//
// Tooltips and toasts are NOT widgets in the tree at the point they are
// triggered. A tooltip owned by the button that raises it would be clipped
// by that button's toolbar and would paint underneath its neighbours. They
// live in a single OverlayHost that sits last in the root's child list, so
// it paints above everything with no z-index bookkeeping.

#pragma once

#include "folio/ui/Animation.h"
#include "folio/ui/Draw.h"
#include "folio/ui/Widget.h"

#include <functional>
#include <string>

namespace folio::ui {

/// A width/height pair. Rect carries an origin we do not always have.
struct Size2 {
    float w = 0.f, h = 0.f;
};

/// Top-most overlay layer. One per window.
class OverlayHost : public Widget {
public:
    OverlayHost();

    /// The host for the window currently being built/painted. Components
    /// reach for this rather than threading a pointer through every
    /// constructor. Returns a harmless no-op instance if none is installed,
    /// so unit-testing a lone button does not need an app around it.
    static OverlayHost& current();
    static void setCurrent(OverlayHost* h);

    // --- tooltip -----------------------------------------------------------
    /// Arm the 380 ms hover delay. Re-arming for the same owner is a no-op.
    void requestTooltip(Widget* owner, std::string text, double now);
    void cancelTooltip(const Widget* owner);
    void dismissTooltip();

    // --- toast -------------------------------------------------------------
    void showToast(std::string text, double now);

    // --- progress ----------------------------------------------------------
    /// 0..1 for determinate, or negative for the indeterminate sweep.
    void setProgress(float v, double now);
    void hideProgress(double now);

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    [[nodiscard]] bool animating(double now) const override;
    [[nodiscard]] Rect damageBounds() const override;

private:
    void paintTooltip(IPaintTarget& t, const PaintCtx& ctx);
    void paintToast(IPaintTarget& t, const PaintCtx& ctx);
    void paintProgress(IPaintTarget& t, const PaintCtx& ctx);

    // tooltip
    Widget* tipOwner_ = nullptr;
    std::string tipText_;
    double tipArmed_ = 0.0;
    AnimatedF tipAlpha_{0.f};
    Point tipAnchor_{};

    // toast
    std::string toastText_;
    double toastShown_ = -1e9;
    AnimatedF toastT_{0.f};

    // progress
    float progress_ = 0.f;
    bool progressVisible_ = false;
    AnimatedF progressAlpha_{0.f};
    AnimatedF progressValue_{0.f};
    /// Union of what the last paint actually touched.
    mutable Rect painted_{0.f, 0.f, 0.f, 0.f};
};

// ---------------------------------------------------------------------------
// Popover
// ---------------------------------------------------------------------------

/// A card that reveals from an anchor with the prototype's 460 ms
/// scale+fade, holds arbitrary children, and dismisses on outside click or
/// Escape.
class Popover : public Widget {
public:
    Popover();

    /// Anchor in *window* coordinates; the popover positions itself below
    /// and flips above if it would leave the window.
    void open(Point anchorWindow, double now);
    void close(double now);
    void constrainToWindow();
    [[nodiscard]] bool isOpen() const noexcept { return open_; }

    /// Content area the caller should lay children out within.
    [[nodiscard]] Rect contentRect() const;

    /// Open above the anchor rather than below. A menu hangs off its
    /// button, but a selection popup must not cover the text it acts on.
    bool preferAbove = false;

    /// Set by the owner so the popover can clamp itself to the window.
    Size2 windowSize{1440.f, 900.f};

    std::function<void()> onClosed;

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;

    /// Called by the screen when a click lands outside. Separate from
    /// onPointer because the popover never receives that click itself.
    void dismissIfOutside(Point windowPoint, double now);

    /// The control that opens this popover. A press on it must NOT count as
    /// an outside click: the press would close the popover and the button's
    /// own click would immediately reopen it, so the control appeared dead.
    void setOwner(const Widget* w) noexcept { owner_ = w; }

    /// Corner the reveal grows from, in local coordinates. Set by open()
    /// from the anchor side so a menu below its button opens downward.
    void applyContentClip(IPaintTarget& t, const PaintCtx& ctx) override;

private:
    bool open_ = false;
    const Widget* owner_ = nullptr;
    Point revealFrom_{0.f, 0.f};
    AnimatedF reveal_{0.f};
    Point anchor_{};
};

}  // namespace folio::ui
