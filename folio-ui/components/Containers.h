// folio-ui/components/Containers.h
//
// Layout containers, the collapsible side panels, document tabs and the
// recents grid.

#pragma once

#include "folio/ui/Animation.h"
#include "folio/ui/Draw.h"
#include "folio/ui/Widget.h"
#include "components/RecentCover.h"

#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace folio::ui {

/// A linear box. Deliberately not a full flexbox: the prototype's layout is
/// rows and columns with one or two flexible children, and implementing the
/// 20% of flexbox that covers it is a hundred lines instead of a thousand.
class Stack : public Widget {
public:
    enum class Dir : std::uint8_t { Row, Column };
    enum class Cross : std::uint8_t { Start, Centre, End, Stretch };

    explicit Stack(Dir d = Dir::Row) : dir(d) { acceptsPointer = false; }

    Dir dir = Dir::Row;
    Cross cross = Cross::Stretch;
    float spacing = 0.f;
    float padL = 0.f, padT = 0.f, padR = 0.f, padB = 0.f;

    void setPadding(float all) { padL = padT = padR = padB = all; }
    void setPadding(float h, float v) { padL = padR = h; padT = padB = v; }

    void onLayout() override;
};

/// A flexible gap. Clearer in a tree dump than a Stack child with flex=1
/// and no content, and it is what the prototype's `.fl` spacer is.
class Spacer : public Widget {
public:
    explicit Spacer(float flex = 1.f) {
        lp.flex = flex;
        acceptsPointer = false;
        debugName = "Spacer";
    }
};

/// A plain rounded surface with a token elevation.
class CardView : public Widget {
public:
    explicit CardView(float radius = tokens::radius::card) : radius_(radius) {
        acceptsPointer = false;
    }
    /// Which elevation to use; defaults to `elevation.card`.
    std::function<tokens::Elevation(const tokens::Palette&)> elevation =
        [](const tokens::Palette& p) { return tokens::elevation::card(p); };
    /// Override the fill; defaults to `color.card`.
    std::function<Color(const tokens::Palette&)> fill =
        [](const tokens::Palette& p) { return p.card; };

    void setRadius(float r) { radius_ = r; }
    /// Per-corner radii. A bottom sheet is rounded along its top edge only,
    /// so it reads as a surface that slid up from off-screen rather than a
    /// card that happens to be touching the bottom.
    void setCorners(Corners c) { corners_ = c; hasCorners_ = true; }
    void clearCorners() { hasCorners_ = false; }
    [[nodiscard]] Corners cornersOrRadius() const {
        return hasCorners_ ? corners_ : Corners::all(radius_);
    }
    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;

private:
    float radius_;
    Corners corners_{};
    bool hasCorners_ = false;
};

// ---------------------------------------------------------------------------

/// Clipping scroll container with wheel input and inertial fling.
///
/// The fling is integrated per frame rather than animated to a precomputed
/// endpoint, because the content height can change mid-fling (a thumbnail
/// list still loading) and a precomputed endpoint would then be wrong.
class ScrollView : public Widget {
public:
    ScrollView();

    /// Total scrollable content height, set by the owner after laying out.
    float contentHeight = 0.f;
    bool showScrollbar = true;

    [[nodiscard]] float scrollY() const noexcept { return scrollY_; }
    void setScrollY(float y);
    /// A tab switch restores the exact panel position, not an old animated
    /// reveal target that might override it on the next frame.
    void restoreScrollY(float y) {
        setScrollY(y);
        smooth_.reset(scrollY_);
        smoothActive_ = false;
        velocity_ = overscrollVelocity_ = 0.f;
        touchHeld_ = barHeld_ = barTrackPress_ = false;
    }
    void scrollBy(float dy);
    /// Bring [top, top+height] into view, animating over `durationMs`.
    void revealRange(float top, float height, double now, int durationMs);

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    void onPaintOverlay(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;

    /// Children are positioned by the owner in content coordinates; the view
    /// translates them. Call after changing scrollY.
    [[nodiscard]] float maxScroll() const;

protected:
    float scrollY_ = 0.f;
    float velocity_ = 0.f;
    float overscrollVelocity_ = 0.f;
    double lastFrame_ = 0.0;
    AnimatedF smooth_{0.f};
    bool smoothActive_ = false;
    AnimatedF barAlpha_{0.f};
    PointerId touchId_ = 0;
    float touchStartY_ = 0.f;
    float touchLastY_ = 0.f;
    double touchLastTime_ = 0.0;
    bool touchHeld_ = false;
    bool touchMoved_ = false;
    bool barHeld_ = false;
    bool barTrackPress_ = false;
    PointerId barId_ = 0;
    float barGrabOffset_ = 0.f;
};

// ---------------------------------------------------------------------------

/// One of the prototype's collapsible side panels.
///
/// Collapsing animates the width to `size.panelRailCollapsed` over 340 ms
/// and cross-fades the content out to a vertical icon rail. The owner reads
/// `currentWidth()` during layout, which is why the animation lives here and
/// not in the screen.
class SidePanel : public CardView {
public:
    enum class Side : std::uint8_t { Left, Right };

    /// How the panel presents itself.
    ///
    /// `Column` is the desktop behaviour: a fixed-width column that
    /// collapses sideways into an icon rail. `Sheet` is the phone
    /// behaviour: a full-width surface that slides up from the bottom
    /// edge, rounded along its top only, with no rail - a rail floating
    /// over a 390px page costs a third of the width and is exactly what
    /// the sheet exists to get rid of.
    enum class Presentation : std::uint8_t { Column, Sheet };

    explicit SidePanel(Side s);

    void setPresentation(Presentation p) { present_ = p; }
    [[nodiscard]] Presentation presentation() const noexcept { return present_; }

    /// 0 fully off-screen, 1 fully raised. Only meaningful in Sheet mode;
    /// the owner turns it into a y offset.
    [[nodiscard]] float sheetProgress(double now) const;

    void setCollapsed(bool c, double now);
    /// Initial state: avoid animating an open panel across the first frame.
    void setCollapsedImmediately(bool c) noexcept;

    /// The width this panel animates to when expanded.
    ///
    /// Was a token read directly in three places, which meant a panel could
    /// not be narrower on a tablet than on a desktop. The breakpoint sets
    /// it; everything else asks the panel.
    void setExpandedWidth(float w, double now);
    [[nodiscard]] float expandedWidth() const noexcept { return expanded_; }
    [[nodiscard]] bool collapsed() const noexcept { return collapsed_; }
    void toggle(double now) { setCollapsed(!collapsed_, now); }

    /// Animated width for this frame.
    [[nodiscard]] float currentWidth(double now) const { return width_.value(now); }
    /// Animated height. A collapsed panel is not a full-height sliver: the
    /// prototype shrinks it to a short floating rail (204px left, 90px
    /// right) holding just enough icon buttons to bring it back.
    [[nodiscard]] float currentHeight(double now, float expandedH) const;
    /// 0 expanded, 1 fully collapsed - drives the rail's fade-in.
    [[nodiscard]] float railAlpha(double now) const;
    /// 1 while expanded, 0 while collapsed: the content opacity.
    [[nodiscard]] float contentAlpha(double now) const;

    [[nodiscard]] bool animating(double now) const override;

    /// Captions and colour chips the owner wants drawn above the card but
    /// still inside the panel's own subtree. Drawing them from the screen's
    /// onPaintOverlay instead puts them above *every* sibling, which made
    /// the panel's text show through an open popover.
    std::function<void(IPaintTarget&, const PaintCtx&)> drawOverlay;
    void onPaintOverlay(IPaintTarget& t, const PaintCtx& ctx) override {
        if (drawOverlay) drawOverlay(t, ctx);
    }

private:
    Side side_;
    Presentation present_ = Presentation::Column;
    bool collapsed_ = false;
    AnimatedF width_;
    float expanded_;
    AnimatedF collapseT_{0.f};  // 0 expanded, 1 collapsed
    AnimatedF sheetT_{0.f};     // 0 hidden below, 1 raised
};

// ---------------------------------------------------------------------------

/// Document tab strip, with the prototype's 350 ms open and 300 ms close.
class TabBar : public Widget {
public:
    struct Tab {
        std::string title;
        bool dirty = false;
        /// 0..1 open animation; 1 for a settled tab.
        Timeline open;
        /// Set when closing; the tab is erased once it reaches 0.
        AnimatedF closing{1.f};
        bool isClosing = false;
    };

    TabBar();

    std::function<void(int)> onSelect;
    std::function<void(int)> onClose;
    std::function<void()> onNew;

    void addTab(std::string title, double now, bool select = true);
    void closeTab(int index, double now);
    void selectTab(int index, double now);
    /// Rename an existing tab, for when a real file replaces a placeholder.
    void setTitle(int index, std::string title);
    [[nodiscard]] int active() const noexcept { return active_; }
    [[nodiscard]] std::size_t count() const noexcept { return tabs_.size(); }
    [[nodiscard]] const std::vector<Tab>& tabs() const noexcept { return tabs_; }
    void setDirty(int index, bool dirty);

    /// Width the strip wants, so the title bar can size it.
    [[nodiscard]] float intrinsicWidth(double now) const;

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;

private:
    [[nodiscard]] float tabWidth(const Tab& t, double now) const;
    /// Returns the tab index under `local`, and whether the close button
    /// specifically was hit.
    [[nodiscard]] int hitTab(Point local, double now, bool* onClose) const;
    /// Bounds of the "+" glyph, in local coordinates. Hit testing and the
    /// hover highlight both go through this so they cannot disagree.
    [[nodiscard]] Rect plusRect() const;
    void reapClosed(double now);

    std::vector<Tab> tabs_;
    Point hoverPos_{-1.f, -1.f};
    /// Last painted clock, so plusRect() can measure animating tab widths
    /// from an event handler (which has no clock of its own).
    mutable double lastNow_ = 0.0;
    int active_ = 0;
    int hoverTab_ = -1;
    bool hoverClose_ = false;
};

// ---------------------------------------------------------------------------

/// A recent-document card on the welcome screen: thumbnail, title, meta,
/// hover lift, and a staggered entrance.
class RecentCard : public Widget {
public:
    RecentCard(std::string title, std::string meta, int accentIndex);

    std::function<void()> onOpen;
    std::function<void(Point)> onMenu; // window position of hold/right-click
    std::string title;
    std::string meta;
    std::shared_ptr<const RecentCover> cover;
    /// 0..1 reading progress, drawn as a bar along the bottom.
    float progress = 0.f;
    int accentIndex = 0;

    /// Begin the staggered entrance.
    void enter(double now, int indexInGrid);

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;

private:
    AnimatedF hoverT_{0.f};
    Timeline enter_;
    double barDelay_ = 0.0;
    double pressStart_ = -1.0;
    Point pressPos_{};
    bool menuOpened_ = false;
};

// ---------------------------------------------------------------------------

/// The dashed drop target.
class Dropzone : public Widget {
public:
    Dropzone();
    std::string title = "Drop a PDF here";
    std::string subtitle = "or click to browse";
    std::function<void()> onActivate;
    /// Set by the shell during a real drag-and-drop.
    void setDragOver(bool v, double now);

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;

private:
    AnimatedF glow_{0.f};
    bool dragOver_ = false;
};

}  // namespace folio::ui
