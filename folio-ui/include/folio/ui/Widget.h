// folio-ui/include/folio/ui/Widget.h
//
// The retained widget tree.
//
// WHY RETAINED, WHEN IPaintTarget IS IMMEDIATE
// --------------------------------------------
// The paint interface is immediate-mode (plan: "chrome is cheap enough to
// re-emit every frame"), but the *state* cannot be. A hover transition, a
// scroll offset, a text cursor and a half-open popover all have to survive
// between frames, and an immediate-mode UI has to key that state on something
// - usually a hashed ID path, which is where immediate-mode GUIs get fragile.
// A tiny retained tree costs one allocation per widget at construction and
// makes state ownership obvious.
//
// COORDINATE CONVENTION
// ---------------------
// `frame` is in PARENT coordinates. Everything a widget does inside onPaint
// and onPointer is in LOCAL coordinates, with its own top-left at (0, 0).
// The traversal applies the translation. Getting this consistent is what
// keeps nested scrolling and popover placement from turning into a thicket
// of manual offset arithmetic.

#pragma once

#include "folio/render/IPaintTarget.h"
#include "folio/ui/Animation.h"
#include "folio/ui/Event.h"
#include "folio/ui/Tokens.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace folio::ui {

using folio::render::IPaintTarget;

/// Paint layers.
///
/// Layering used to be expressed by shuffling the child list with
/// raiseToFront(). That has no inverse, so a panel raised above the canvas
/// for a phone drawer stayed above *everything* - popovers included - for
/// the rest of the session, at every window size. A widget now declares
/// which layer it belongs to and the traversal honours it; insertion order
/// still decides ties, so anything left at `content` behaves exactly as
/// before.
namespace zlayer {
inline constexpr int content = 0;   ///< Ordinary chrome and the canvas.
inline constexpr int floating = 10; ///< Floats over the canvas (focus exit).
inline constexpr int panel = 20;    ///< Side panels when they overlay.
inline constexpr int popover = 30;  ///< Menus, pickers, selection popups.
inline constexpr int modal = 40;    ///< Dialogs and their scrim.
} // namespace zlayer

/// Everything a widget needs in order to paint, and nothing it does not.
/// Passed by const reference down the tree; widgets must not retain it.
struct PaintCtx {
    /// The *effective* palette for this frame. During the 760 ms theme
    /// transition this is an interpolation of light and dark, not either one,
    /// which is why widgets must never reach for tokens::kLight directly.
    const tokens::Palette& pal;
    double now = 0.0;
    /// Device pixel ratio. Hairlines are drawn at 1/dpr so they stay crisp.
    float dpr = 1.f;
    /// Non-empty only when painting a retained scene's damaged chrome.
    Rect damage{};
    bool chromeOnly = false;
};

class Widget {
public:
    Widget() = default;
    virtual ~Widget(); // notifies live event routers before pointer targets disappear
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // --- tree --------------------------------------------------------------

    template <typename T, typename... Args>
    T* emplace(Args&&... args) {
        auto owned = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = owned.get();
        adopt(std::move(owned));
        return raw;
    }

    Widget* adopt(std::unique_ptr<Widget> child);
    /// Move this widget to the end of its parent's child list. Children
    /// paint in order, so the last one is on top - which is what a floating
    /// control needs when it is created before the canvas it floats over.
    ///
    /// This only reorders *within* a layer. To put a widget above a whole
    /// class of other widgets, give it a higher zOrder: that is reversible,
    /// and raising is not.
    void raiseToFront();

    /// Which layer this widget paints in; see `zlayer`. Higher paints later,
    /// and is therefore hit-tested first. Ties keep insertion order.
    [[nodiscard]] int zOrder() const noexcept { return zOrder_; }
    void setZOrder(int z);
    void removeAllChildren() { children_.clear(); }
    [[nodiscard]] const std::vector<std::unique_ptr<Widget>>& children() const {
        return children_;
    }
    [[nodiscard]] Widget* parent() const noexcept { return parent_; }

    // --- geometry ----------------------------------------------------------

    Rect frame{};
    [[nodiscard]] Rect localBounds() const noexcept {
        return Rect{0.f, 0.f, frame.w, frame.h};
    }
    void setFrame(float x, float y, float w, float h) noexcept {
        frame = Rect{x, y, w, h};
    }
    /// Local point -> window point.
    [[nodiscard]] Point toWindow(Point local) const noexcept;

    /// How a Stack should treat this child.
    ///
    /// Living on Widget rather than in a parallel map is a deliberate
    /// trade: 16 bytes on every widget, against the alternative of a
    /// lookup on every layout pass and the chance of the two getting out
    /// of sync when a child is reparented.
    struct LayoutParams {
        /// Share of leftover space along the stack's main axis. 0 means
        /// "use my preferred size".
        float flex = 0.f;
        /// Preferred main-axis size. Negative means "whatever frame already
        /// says", which is how fixed-size controls opt out.
        float basis = -1.f;
        /// Excluded from layout entirely (not merely invisible), so a
        /// collapsed panel does not leave a gap.
        bool skip = false;
    };
    LayoutParams lp;

    // --- flags -------------------------------------------------------------

    bool visible = true;
    bool enabled = true;
    /// If false the widget is transparent to the pointer; children are still
    /// tested. Containers and decorative layers set this.
    bool acceptsPointer = false;
    /// A vertical touch drag may transfer capture from a child (such as a
    /// document card) to this scroll container after the drag passes slop.
    bool touchScrollable = false;
    /// Clip everything this widget draws - its own onPaint and
    /// onPaintOverlay as well as its children - to `localBounds()`.
    ///
    /// Clipping only the children is not enough and was a real bug: a
    /// ScrollView subclass paints its rows in its own onPaint, and a
    /// PageStage paints the pages in its own onPaint, so with a
    /// children-only clip the thumbnails spilled past the panel's rounded
    /// bottom and the pages drew over the command bar and off the window.
    /// This is CSS `overflow:hidden` on the element, which is what the
    /// prototype specifies for .panel, .pane and #cv.
    bool clipsContent = false;

    /// Shape of that clip. The default is the widget's own bounds; a
    /// popover overrides it with a growing circle to get a circular reveal.
    virtual void applyContentClip(IPaintTarget& t, const PaintCtx& ctx);

    /// Keyboard focus arrived or left. Lets a control swap presentation
    /// between "showing a value" and "editing a value" without polling
    /// `focused` from inside onPaint.
    virtual void onFocusChanged(bool /*gained*/) {}
    /// The host may need to show an on-screen keyboard for this control.
    [[nodiscard]] virtual bool wantsTextInput() const { return false; }

    /// Area this widget can actually repaint, in its own coordinates.
    /// Defaults to the whole frame. Full-window containers that only ever
    /// draw something small - the overlay layer, for instance - must narrow
    /// this, or every tooltip would mark the entire window damaged and
    /// defeat partial repainting.
    [[nodiscard]] virtual Rect damageBounds() const { return localBounds(); }

    /// The current time, as seen by both painting and input.
    ///
    ///
    /// Event handlers are called from the input queue, which carries no
    /// clock of its own, and an animation retargeted at a stale timestamp is
    /// already finished the first time it is evaluated - it snaps instead of
    /// animating. That bug has shipped four separate times here (panel
    /// collapse, theme cross-fade twice, popover reveal).
    ///
    /// Caching the last *paint* time is not enough: the app is event-driven
    /// and stops painting when idle, so after a few seconds of stillness the
    /// cached value is seconds behind and the next click animates nothing.
    /// This clock is therefore advanced by painting AND by input dispatch,
    /// whichever happened most recently.
    [[nodiscard]] static double now() noexcept { return sClock; }
    static void setClock(double t) noexcept {
        if (t > sClock) sClock = t;
    }
    /// Group opacity. Below 1 the subtree is composited through a layer so
    /// overlapping children do not double-darken.
    float opacity = 1.f;
    std::string debugName;

    // --- interaction state (maintained by the router) -----------------------

    bool hovered = false;
    bool pressed = false;
    bool focused = false;

    // --- overridables -------------------------------------------------------

    /// Position children. Called top-down after any resize.
    virtual void onLayout() {}
    /// Paint self, in local coordinates, before children.
    virtual void onPaint(IPaintTarget&, const PaintCtx&) {}
    /// Paint after children, for overlays like focus rings and scrim edges.
    virtual void onPaintOverlay(IPaintTarget&, const PaintCtx&) {}
    /// Cached PDF pixels survive chrome-only frames in the backing surface.
    [[nodiscard]] virtual bool isDocumentCanvas() const noexcept { return false; }
    /// Return true to consume. Position is in local coordinates.
    virtual bool onPointer(const PointerEvent&) { return false; }
    virtual bool onKey(const KeyEvent&) { return false; }
    /// True while this widget still needs frames. The frame loop stops
    /// repainting when the whole tree reports false, which is what lets an
    /// idle window cost zero CPU.
    [[nodiscard]] virtual bool animating(double) const { return false; }

    // --- traversal (non-virtual, applied uniformly) -------------------------

    void layoutTree();
    void paintTree(IPaintTarget& t, const PaintCtx& ctx);
    /// Deepest pointer-accepting descendant containing `parentPoint`.
    /// Returns nullptr if the point misses this subtree.
    /// `minTarget` is the smallest tappable edge length to honour: any
    /// control painted smaller than that is hit-tested as if it were that
    /// big, without changing a single pixel of how it is drawn. Pass 0 for
    /// an exact test (mouse and pen).
    [[nodiscard]] Widget* hitTest(Point parentPoint, float minTarget = 0.f);
    [[nodiscard]] bool treeAnimating(double now) const;

    /// Depth-first visit, self first.
    void forEach(const std::function<void(Widget&)>& fn);

protected:
    /// Hit shape, in local coordinates. Override for non-rectangular or
    /// enlarged touch targets. Default is the full local bounds.
    [[nodiscard]] virtual bool hitLocal(Point local) const;

    /// `hitLocal` with the frame inflated to at least `minTarget` on each
    /// axis. Only ever grows, never shrinks, so a control that is already
    /// comfortably large keeps its exact shape - including any custom
    /// `hitLocal` override, which is consulted first.
    [[nodiscard]] bool hitLocalPadded(Point local, float minTarget) const;

private:
    /// Re-sort children by layer if anything changed since the last pass.
    /// Cheap: a bool test per traversal, and a stable_sort only after an
    /// adopt() or a setZOrder().
    void ensureChildOrder();

    std::vector<std::unique_ptr<Widget>> children_;
    Widget* parent_ = nullptr;
    int zOrder_ = zlayer::content;
    bool childOrderDirty_ = false;
    static inline double sClock = 0.0;
};

// ---------------------------------------------------------------------------
// EventRouter
// ---------------------------------------------------------------------------

/// Turns a stream of window-coordinate pointer events into per-widget
/// Enter/Leave/Down/Up/Move, with pointer capture.
///
/// Capture matters more than it looks: without it, pressing a slider and
/// dragging past its edge drops the drag, which every real UI gets right and
/// every first-draft UI gets wrong.
class EventRouter {
public:
    EventRouter();
    ~EventRouter();
    EventRouter(const EventRouter&) = delete;
    EventRouter& operator=(const EventRouter&) = delete;

    /// Smallest tappable edge length, in logical px, applied to touch
    /// input only. 44 is the figure Apple, Google (48 dp) and Microsoft
    /// all converge on; it is a field rather than a constant so a test
    /// can turn the behaviour off and assert the exact geometry.
    float touchTarget = 44.f;
    std::function<void(bool)> onTextFocusChange;

    /// Returns true if any widget consumed the event.
    bool dispatchPointer(Widget& root, const PointerEvent& windowEvent);
    bool dispatchKey(Widget& root, const KeyEvent& e);

    void setFocus(Widget* w);
    [[nodiscard]] Widget* focused() const noexcept { return focus_; }
    [[nodiscard]] Widget* hovered() const noexcept { return hover_; }

    /// Forget a widget that is about to be destroyed. Screens call this when
    /// they rebuild their subtree.
    void forget(const Widget* w);

private:
    static PointerEvent localise(const PointerEvent& e, Widget& target);

    struct TouchCapture {
        Widget* target = nullptr;
        Point start{};
        double startTime = 0.0;
    };
    Widget* hover_ = nullptr;
    Widget* capture_ = nullptr; // mouse / pen
    std::unordered_map<PointerId, TouchCapture> touches_;
    Widget* focus_ = nullptr;
};

}  // namespace folio::ui
