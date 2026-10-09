// folio-ui/components/PageStage.h
//
// The document viewport: the thing the whole application exists to show.
//
// WHY A PAGE SOURCE INTERFACE
// ---------------------------
// folio-ui-core must not link Skia (that is the seam the whole architecture
// rests on), but drawing a real page means handing a raster tile to a
// canvas, which is inherently backend-specific. The resolution is
// IPageSource: the stage owns layout, scrolling, zoom, rotation and every
// animation, and delegates *only* the act of putting page pixels in a
// rectangle. A placeholder implementation draws synthetic paper; the real
// one bridges to SkiaTileCompositor and the PDFium raster cache.
//
// This is the same split the architecture plan describes for WP F1 (the Qt
// QQuickItem viewport): identical stage logic, one small backend class.

#pragma once

#include "folio/ui/Animation.h"
#include "folio/ui/Draw.h"
#include "folio/ui/Theme.h"
#include "components/Gestures.h"
#include "folio/ui/Widget.h"
#include "components/Annotations.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace folio::ui {

/// Page geometry in PDF points (1/72 inch).
struct PageSizePt {
    float w = 612.f, h = 792.f;
};

/// Supplies page content to the stage.
/// One laid-out character on a page.
struct PageGlyph {
    char32_t code = 0;
    float    l = 0.f, t = 0.f, r = 0.f, b = 0.f;
};

class IPageSource {
public:
    virtual ~IPageSource() = default;
    [[nodiscard]] virtual int pageCount() const = 0;
    [[nodiscard]] virtual PageSizePt pageSize(int index) const = 0;
    /// Draw page `index` to fill `dest` exactly. `scale` is device pixels
    /// per point, passed so a tiled implementation can pick a raster scale
    /// without re-deriving it from the rectangle.
    virtual void drawPage(IPaintTarget& t, int index, const Rect& dest, float scale,
                          PageTheme theme) = 0;
    /// Human-readable, for the status bar.
    [[nodiscard]] virtual std::string describe() const { return "document"; }

    /// Asynchronous sources override these two. `tick` is called once per
    /// frame before any drawing and installs whatever finished off-thread;
    /// it returns true when the visible result changed. `pending` keeps the
    /// frame loop running while work is outstanding, so a raster that lands
    /// during an idle period is actually shown rather than waiting for the
    /// next unrelated event.
    virtual bool tick() { return false; }
    [[nodiscard]] virtual bool pending() const { return false; }

    /// True when drawPage() is already a blit of a bitmap the source keeps
    /// cached itself. The stage then draws straight to the canvas instead of
    /// routing through its own raster cache: re-caching a single blit costs
    /// a surface allocation and a full-page composite to save one blit,
    /// which is strictly worse. Vector sources want the opposite.
    [[nodiscard]] virtual bool cachesOwnRasters() const { return false; }

    /// Hint that the user is looking at `page`; async sources render
    /// outwards from there.
    virtual void setFocusPage(int /*page*/) {}

    /// The page's laid-out glyphs, 0..1 page-normalised with a top-left
    /// origin - the same space annotations use, so a selection rectangle
    /// and a highlight are directly comparable. Returns nullptr when the
    /// page has no text layer yet; asking is what queues the extraction, so
    /// a caller should simply ask again next frame.
    virtual const std::vector<PageGlyph>* glyphs(int /*page*/) { return nullptr; }
};

/// Synthetic paper, used before a document is opened and in tests. Produces
/// a plausible text page so layout and theming can be judged without a PDF.
class PlaceholderPageSource final : public IPageSource {
public:
    explicit PlaceholderPageSource(int pages = 24) : pages_(pages) {}

    /// Bar colour for the chart block; the prototype gives each document
    /// its own accent.
    void setAccent(render::Color c) { accent_ = c; }
    [[nodiscard]] int pageCount() const override { return pages_; }
    // The prototype's .pg is aspect-ratio 7/10, not US Letter.
    [[nodiscard]] PageSizePt pageSize(int) const override { return {560.f, 800.f}; }
    void drawPage(IPaintTarget& t, int index, const Rect& dest, float scale,
                  PageTheme theme) override;
    [[nodiscard]] std::string describe() const override { return "Sample document"; }

private:
    int pages_;
    render::Color accent_{0x43, 0x38, 0xCA, 0xFF};
};

enum class LayoutMode : std::uint8_t { Single, TwoUp, Continuous, Grid };

/// Zoom presets the UI exposes, plus free zoom between them.
enum class ZoomMode : std::uint8_t { Free, FitWidth, FitPage };

class PageStage : public Widget {
public:
    PageStage();

    void setSource(std::shared_ptr<IPageSource> src, double now);
    [[nodiscard]] IPageSource* source() const noexcept { return source_.get(); }

    // --- view state ---------------------------------------------------------
    void setLayout(LayoutMode m, double now);
    [[nodiscard]] LayoutMode layout() const noexcept { return layout_; }

    void setZoom(float z, double now);
    void setZoomMode(ZoomMode m, double now);
    [[nodiscard]] float zoom() const noexcept { return zoomTarget_; }
    [[nodiscard]] ZoomMode zoomMode() const noexcept { return zoomMode_; }

    /// Rotate the whole document by 90 degrees, animated.
    void rotate(int deltaQuarterTurns, double now);
    [[nodiscard]] int rotationQuarterTurns() const noexcept { return rotation_; }

    void setPageTheme(PageTheme t) noexcept { pageTheme_ = t; }

    /// 1-based, as shown in the UI.
    /// Horizontal pan, in content pixels. Only meaningful once the zoomed
    /// page is wider than the viewport.
    [[nodiscard]] float scrollX() const noexcept { return scrollX_; }
    [[nodiscard]] float contentWidth() const noexcept { return contentW_; }

    [[nodiscard]] int currentPage() const noexcept { return current_ + 1; }
    void goToPage(int oneBased, double now);
    void nextPage(double now) { goToPage(currentPage() + 1, now); }
    void prevPage(double now) { goToPage(currentPage() - 1, now); }

    /// Grid thumbnail size, for LayoutMode::Grid.
    void setGridSize(float px) { gridSize_ = px; }
    [[nodiscard]] float gridSize() const noexcept { return gridSize_; }

    std::function<void(int)> onPageChanged;

    /// Replay the staggered page-entrance animation (on open, and on a
    /// layout change, matching the prototype).
    void replayEntrance(double now);

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;

private:
    struct Placed {
        int index = 0;
        Rect rect;      // in content coordinates
    };

public:
    // --- annotation editing -------------------------------------------------

    /// What a pointer drag on a page does. Select/Hand pan and select; the
    /// rest author annotations.
    enum class EditTool : std::uint8_t {
        None, Highlight, Ink, Note, Eraser, Box, Ellipse, Text, Stamp, Signature
    };

    void setStore(AnnotationStore* s) noexcept { store_ = s; }

    // --- annotation selection ------------------------------------------------
    /// The annotation under the cursor can be picked up with the Select
    /// tool, dragged, and deleted. 0 means nothing is selected.
    [[nodiscard]] std::uint64_t selectedAnnot() const noexcept { return selAnnot_; }
    void selectAnnot(std::uint64_t id) noexcept { selAnnot_ = id; }
    void deselectAnnot() noexcept { selAnnot_ = 0; }
    // --- free text -----------------------------------------------------------
    /// While a free-text annotation is being typed, keystrokes belong to it
    /// rather than to the document's shortcuts. 0 when nothing is being
    /// edited.
    [[nodiscard]] std::uint64_t editingText() const noexcept { return editText_; }
    /// Feed one key to the text being edited. Returns false when the key is
    /// not ours, so the caller can fall through to its own handling.
    bool handleTextKey(const KeyEvent& e);
    /// Finish editing. `cancel` discards an empty or abandoned annotation.
    void commitText(bool cancel = false);
    /// Begin editing an existing text annotation (double-click, say).
    void beginTextEdit(std::uint64_t id);
    /// Typeface and glyph height for new free text, and for whatever is
    /// being edited or selected right now - changing the size with a box
    /// open should change that box, not just the next one.
    void setTextStyle(float normalisedSize, folio::render::FontFamily family);
    [[nodiscard]] float textSize() const noexcept { return textSize_; }
    [[nodiscard]] folio::render::FontFamily textFont() const noexcept { return textFont_; }
    void paintTextCaret(IPaintTarget& t, const PaintCtx& ctx);

    /// Delete the selected annotation. The caller supplies the undo record,
    /// so the stage stays ignorant of the command stack.
    std::function<void(const Annotation&)> onAnnotationDeleted;
    /// Fired once a drag finishes, with the geometry before and after, so
    /// the move lands on the undo stack as one entry rather than per frame.
    std::function<void(const Annotation&, const Annotation&)> onAnnotationMoved;
    /// Fired when the selection changes, so the panel can follow it.
    std::function<void(std::uint64_t)> onAnnotSelectionChanged;
    /// Delete whatever is selected, if anything. Returns true if it went.
    bool deleteSelectedAnnot();
    // --- text selection -----------------------------------------------------
    /// A position in the document's text: which page, and which glyph on it.
    struct TextPos {
        int page = -1;
        int glyph = 0;
        [[nodiscard]] bool valid() const noexcept { return page >= 0; }
        friend bool operator<(const TextPos& a, const TextPos& b) {
            return a.page != b.page ? a.page < b.page : a.glyph < b.glyph;
        }
        friend bool operator==(const TextPos& a, const TextPos& b) {
            return a.page == b.page && a.glyph == b.glyph;
        }
    };

    [[nodiscard]] bool hasSelection() const noexcept {
        return selA_.valid() && selB_.valid() && !(selA_ == selB_);
    }
    void clearSelection() noexcept { selA_ = selB_ = TextPos{}; }
    /// The selected text as UTF-8, empty when there is no selection.
    [[nodiscard]] std::string selectedText();
    /// Per-line boxes of the live selection as (page, normalised rect) - the
    /// coordinate space annotations are stored in, so a text markup
    /// annotation can be built straight from this.
    void selectionQuads(std::vector<std::pair<int, Rect>>& out) const;
    /// Fired when a selection drag finishes with a non-empty selection.
    /// The argument is the pointer position in window coordinates, so the
    /// caller can put a popup exactly where the user let go.
    std::function<void(Point)> onSelectionCommitted;
    /// Fired when the selection is cleared, so the caller can dismiss it.
    std::function<void()> onSelectionCleared;
    /// Double-click on a page in Grid ("All together") layout. The screen
    /// decides what "open" means - currently single page at fit-page - so
    /// the stage does not hard-code a layout change.
    std::function<void(int page)> onPageActivated;
    /// Select every glyph on every page that has a text layer.
    void selectAll();

    /// Ranges the find bar wants painted. Page, first glyph, count.
    struct TextRun { int page = 0; int start = 0; int count = 0; };
    void setSearchHits(std::vector<TextRun> hits, int current) {
        searchHits_ = std::move(hits);
        searchCurrent_ = current;
    }
    void clearSearchHits() { searchHits_.clear(); searchCurrent_ = -1; }

    void setEditTool(EditTool t) noexcept { editTool_ = t; }
    [[nodiscard]] EditTool editTool() const noexcept { return editTool_; }
    void setAnnotStyle(Color c, float width, float opacity) noexcept {
        annotColor_ = c;
        annotWidth_ = width;
        annotOpacity_ = opacity;
    }
    /// Fired after a stroke is committed, so the screen can refresh counts.
    std::function<void()> onAnnotationsChanged;
    /// A new annotation was committed by a drawing tool. Carries the stored
    /// copy (id already assigned) so the caller can record it for undo.
    std::function<void(const Annotation&)> onAnnotationAdded;

    /// Page under a local point, plus the point normalised into that page.
    /// Returns false when the point is not over any page.
    [[nodiscard]] bool hitPage(Point local, int* pageOut, Point* normOut) const;

private:
    bool handleAuthoring(const PointerEvent& e);

public:

    /// Recompute `placed_` and `contentH_` for the current mode/zoom.
    void relayout(double now);
    [[nodiscard]] float effectiveScale(double now) const;
    [[nodiscard]] float fitScale(bool width) const;
    void clampScroll();
    void updateCurrentFromScroll();

    std::shared_ptr<IPageSource> source_;
    std::vector<Placed> placed_;
    float contentW_ = 0.f;
    float scrollX_ = 0.f;

    // Scrollbars. Drawn in the overlay pass so they sit above the pages,
    // and hit-tested before anything else so a press on one never reaches
    // the page underneath.
    enum class Bar : std::uint8_t { None, Vertical, Horizontal };
    Bar   barDrag_ = Bar::None;
    float barGrabOffset_ = 0.f;
    Bar   barHover_ = Bar::None;
    [[nodiscard]] bool canScrollX() const noexcept { return contentW_ > frame.w + 1.f; }
    [[nodiscard]] bool canScrollY() const noexcept { return contentH_ > frame.h + 1.f; }
    [[nodiscard]] Rect vBarTrack() const;
    [[nodiscard]] Rect vBarThumb() const;
    [[nodiscard]] Rect hBarTrack() const;
    [[nodiscard]] Rect hBarThumb() const;
    void paintScrollbars(IPaintTarget& t, const PaintCtx& ctx);
    bool handleScrollbar(const PointerEvent& e);
    std::vector<Timeline> entrance_;

    LayoutMode layout_ = LayoutMode::Continuous;
    ZoomMode zoomMode_ = ZoomMode::FitWidth;
    float lastFitW_ = 0.f;
    float zoomTarget_ = 1.f;
    AnimatedF zoomAnim_{1.f};
    AnimatedF rotationAnim_{0.f};
    int rotation_ = 0;

    PageTheme pageTheme_ = PageTheme::Light;
    float gridSize_ = tokens::size::gridThumbDefault;

    float scrollY_ = 0.f;
    float contentH_ = 0.f;
    float velocity_ = 0.f;
    double lastFrame_ = 0.0;
    AnimatedF smoothScroll_{0.f};
    bool smoothing_ = false;

    int current_ = 0;
    float lastLayoutW_ = 0.f;
    float lastLayoutScale_ = 0.f;
    /// Scale the cached page bitmaps were rasterised at; frozen mid-gesture.
    float rasterScale_ = 0.f;

    /// Zoom anchor. Changing scale rescales the whole layout, so holding
    /// scrollY_ constant slides the document under the viewport and you end
    /// up looking at a different page. Instead we remember which page was
    /// under the viewport centre and where, and put it back after relayout.
    int anchorPage_ = 0;
    float anchorFrac_ = 0.5f;     // 0..1 down the anchored page
    float anchorViewFrac_ = 0.5f;   // 0..1 down the viewport
    float anchorViewFracX_ = 0.5f;  // 0..1 across the viewport
    float anchorFracX_ = 0.5f;      // 0..1 across the anchored page
    /// Point the next zoom should hold still, in local widget coords.
    /// Set from the pointer on ctrl+wheel; reset to the viewport centre
    /// for the pill's zoom buttons, which have no pointer to speak of.
    void setZoomFocus(float localX, float localY);
    void clearZoomFocus();

    // --- touch -------------------------------------------------------------
    /// Consume touch before the mouse paths see it. Returns true when the
    /// gesture owns the event.
    ///
    /// The rule, which matches every serious annotation app: two fingers
    /// always pinch and pan, whatever tool is selected - it is the escape
    /// hatch out of a drawing mode. One finger pans while reading and
    /// draws while a tool is active. A pen always draws, because that is
    /// why you picked it up.
    bool handleTouch(const PointerEvent& e);
    GestureTracker touch_;
    bool touchPanning_ = false;
    float pinchZoom_ = 1.f;
    /// Mean nib pressure over the stroke in flight, so a pen can commit a
    /// heavier line than a light pass. Per-point pressure profiles would
    /// need the Annotation schema to carry a width per point.
    float penPressSum_ = 0.f;
    int   penPressN_ = 0;
    bool anchorValid_ = false;
    /// Where an in-flight wheel glide is heading, so consecutive notches
    /// extend one animation instead of each restarting from a standstill.
    float smoothTarget_ = 0.f;

    AnnotationStore* store_ = nullptr;
    EditTool editTool_ = EditTool::None;

    // Text selection. `selA_` is the anchor (where the press landed) and
    // `selB_` the head; either may be the earlier one in document order.
    TextPos  selA_, selB_;
    bool     selecting_ = false;
    std::vector<TextRun> searchHits_;
    int      searchCurrent_ = -1;

    /// Nearest glyph to a normalised point on `page`, or -1. Prefers a
    /// direct hit, then the closest glyph on the nearest text line, which is
    /// what makes dragging past the end of a line behave.
    [[nodiscard]] int glyphAt(int page, Point n);
    /// Expand a glyph index to the whitespace-delimited word around it,
    /// writing the half-open range into selA_/selB_. A long press has no
    /// drag behind it, so selecting one character would be useless - every
    /// mobile reader selects a word.
    void selectWordAt(int page, int glyph);
    /// Merge a glyph range into per-line rectangles in content coordinates.
    void selectionRects(int page, int from, int to,
                        std::vector<Rect>& out);
    /// Line boxes for a glyph range, page-normalised (0..1, top-left).
    void selectionLines(int page, int from, int to,
                        std::vector<Rect>& out) const;
    void paintTextOverlay(IPaintTarget& t, const PaintCtx& ctx);
    Color annotColor_{250, 204, 21, 255};
    float annotWidth_ = 2.5f;
    float annotOpacity_ = 1.f;
    bool authoring_ = false;
    int authorPage_ = 0;
    Point authorStart_{0.f, 0.f};
    void captureZoomAnchor();
    void applyZoomAnchor();
    bool dragging_ = false;
    Point dragLast_{};

    // Annotation selection and dragging.
    std::uint64_t selAnnot_ = 0;
    std::uint64_t editText_ = 0;      ///< free-text annotation being typed
    std::size_t   textCaret_ = 0;     ///< caret index within its text
    double        caretBlink_ = 0.0;  ///< reset on every keystroke
    bool          textIsNew_ = false; ///< true until the first commit
    float         textSize_ = 16.f / 792.f;
    folio::render::FontFamily textFont_ = folio::render::FontFamily::Sans;
    bool movingAnnot_ = false;
    Point moveLast_{0.f, 0.f};
    Annotation moveBefore_{};          // geometry when the drag started
    void paintAnnotSelection(IPaintTarget& t, const PaintCtx& ctx);
    bool handleAnnotSelection(const PointerEvent& e);
};

}  // namespace folio::ui
