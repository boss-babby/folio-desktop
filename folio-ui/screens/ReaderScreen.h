// folio-ui/screens/ReaderScreen.h
//
// The document screen: title bar with tabs, command bar, two collapsible
// panels around the page stage, and a floating control pill.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <array>
#include "components/Button.h"
#include "components/Containers.h"
#include "components/Controls.h"
#include "components/WidthStrip.h"
#include "components/Edits.h"
#include "components/WindowChrome.h"
#include "components/Overlays.h"
#include "components/PageStage.h"
#include "components/Annotations.h"
#include "screens/AppState.h"

#include <functional>
#include <memory>
#include <vector>

namespace folio::ui {

/// Left panel content: page thumbnails with a current-page highlight.
class ThumbnailList : public ScrollView {
public:
    explicit ThumbnailList(AppState& s);

    std::function<void(int)> onPick;
    void setPageCount(int n) { pages_ = n; }

    /// The same page source the stage draws, so thumbnails and pages can
    /// never show different content.
    void setSource(IPageSource* s) { source_ = s; }
    void setCurrent(int oneBased, double now);
    [[nodiscard]] int current() const noexcept { return current_; }
    /// Rows needed for the two-column grid, used to size the scroll range.
    [[nodiscard]] int rows() const noexcept { return (pages_ + 1) / 2; }

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;

private:
    [[nodiscard]] Rect thumbRect(int i) const;
    AppState& state_;
    IPageSource* source_ = nullptr;
    int pages_ = 12;
    int current_ = 1;
    int hover_ = -1;
};

/// Right panel content: an outline / annotation list.
class OutlineList : public ScrollView {
public:
    explicit OutlineList(AppState& s);
    std::function<void(int)> onPick;
    void setPageCount(int n) { pages_ = n; }

    /// One heading from the document's own /Outlines tree.
    struct Entry {
        std::string title;
        int page = -1;
        int depth = 0;
    };
    void setEntries(std::vector<Entry> e) { entries_ = std::move(e); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;

private:
    AppState& state_;
    int pages_ = 12;
    int hover_ = -1;
    std::vector<Entry> entries_;
};

class ReaderScreen : public Widget {
public:
    explicit ReaderScreen(AppState& state);

    std::function<void()> onBackToWelcome;
    /// Hand selected text to the platform clipboard. The streaming shell has
    /// nowhere to put it, so it is left unset there.
    std::function<void(const std::string&)> onCopyText;
    /// The window-control cluster, so the app can wire it to the host's
    /// real minimise/maximise/close.
    [[nodiscard]] WindowControls* windowControls() noexcept { return winCtl_; }
    /// Ask the app for a file chooser.
    std::function<void()> onBrowse;
    /// Ask the host for a destination path. The callback may run later, so
    /// nothing may assume it has fired by the time this returns.
    std::function<void(const std::string& suggestedName,
                       std::function<void(std::string)>)> onPickSavePath;
    std::function<void()> onQuit;

    void openDocument(const std::string& title, int pages, double now);

    /// Find every occurrence of `q` in the document and highlight them.
    /// Case-insensitive for ASCII. Empty query clears the highlights.
    void runSearch(const std::string& q);
    /// Step through the hits, scrolling each into view.
    void stepSearch(int delta);

    /// Open a real PDF from disk. Falls back to the placeholder source and
    /// returns false if it cannot be parsed, so a bad file degrades to the
    /// old behaviour rather than an empty window.
    bool openFile(const std::string& path, double now);

    /// Write the in-memory annotations into the PDF itself and save.
    /// Incremental by default, so the original revision survives in the file.
    bool saveDocument();
    /// Page tree edits. All of them save, then reopen so the reader shows
    /// the real file rather than an optimistic guess at what it now holds.
    bool applyPageOp(const std::string& op);

    void onLayout() override;
    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    void onPaintOverlay(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onKey(const KeyEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;

    /// Called by the app before the router runs, so an outside click can
    /// dismiss an open popover.
    void handleOutsideClick(Point windowPoint, double now);

    [[nodiscard]] PageStage* stage() const noexcept { return stage_; }

private:
    void buildTitleBar();
    void buildCommandBar();
    void buildWorkArea();
    void buildPill();
    void buildPopovers();
    void rebuildToolButtons(double now);
    void installPanelPainters();
    void setMode(Mode m);
    void toggleLeftPanel();
    void toggleRightPanel();
    void syncPageLabel();

    AppState& state_;

    // title bar
    TabBar* tabs_ = nullptr;
    Button* backBtn_ = nullptr;
    Button* searchBtn_ = nullptr;

    // --- phone chrome ------------------------------------------------------
    // A tab strip needs width a phone does not have, so the title bar
    // carries the current document name as a dropdown instead. The theme
    // toggle takes over the corner the window controls vacate - there is
    // no minimise/maximise/close on a phone, and that corner is otherwise
    // dead space.
    Button* docBtn_ = nullptr;
    Popover* docPop_ = nullptr;
    std::vector<Button*> docItems_;
    Button* themeBtn_ = nullptr;
    /// Bottom-corner sheet handles. Left thumb opens pages/outline, right
    /// thumb opens the inspector; both are within reach without shifting
    /// grip, which the top-right corner is not.
    Button* pagesFab_ = nullptr;
    Button* inspectFab_ = nullptr;
    void buildPhoneChrome();
    void layOutPhoneChrome(const LayoutMetrics& m, float W, float H,
                           float chromeY, float tb);
    void syncDocMenu();
    /// Raise one sheet and lower the other; `left` picks pages vs inspector.
    void togglePhoneSheet(bool left);
    void syncPhoneFabs();
    /// True when the panels present as bottom sheets rather than columns.
    [[nodiscard]] bool sheetMode() const;

    // command bar
    CardView* cmdBar_ = nullptr;
    Button* modeBtn_ = nullptr;
    std::vector<Widget*> cmdItems_;   // in order, for layout
    Widget* toolHost_ = nullptr;
    std::vector<Button*> toolBtns_;
    Button* displayBtn_ = nullptr;
    Button* leftToggle_ = nullptr;
    Button* rightToggle_ = nullptr;
    Button* focusBtn_ = nullptr;
    /// Shown only in focus mode. Hiding the chrome removed every visible
    /// way back out, which left the keyboard as the sole escape - fine if
    /// you know about it, a trap if you do not.
    Button* focusExitBtn_ = nullptr;
    void setFocusMode(bool on);
    Button* shareBtn_ = nullptr;

    // work area
    SidePanel* leftPanel_ = nullptr;
    SidePanel* rightPanel_ = nullptr;
    ThumbnailList* thumbs_ = nullptr;
    OutlineList* outline_ = nullptr;
    PageStage* stage_ = nullptr;
    std::shared_ptr<IPageSource> pageSource_;

    std::vector<PageStage::TextRun> searchHits_;
    int                             searchIndex_ = -1;
    Label*                          findCount_ = nullptr;
    Button*                         findPrev_ = nullptr;
    Button*                         findNext_ = nullptr;
    void scrollToHit(int i);
    void syncFindLabel();

    // left panel chrome (prototype: 4 tabs, a count row, then the pane)
    SegmentedControl* leftTabs_ = nullptr;
    Button* leftCollapse_ = nullptr;
    std::vector<Button*> pageActions_;    // Rotate / Insert / Extract / Delete
    std::vector<Button*> leftRail_;       // shown when collapsed

    // right panel chrome
    Button* rightCollapse_ = nullptr;
    std::vector<Button*> pageThemeSw_;    // Light / Sepia / Dark swatches
    SegmentedControl* layoutSegR_ = nullptr;
    std::vector<Button*> rightRail_;

    // pill
    CardView* pill_ = nullptr;
    Button* prevBtn_ = nullptr;
    Button* nextBtn_ = nullptr;
    Button* zoomOutBtn_ = nullptr;
    Button* zoomInBtn_ = nullptr;
    Button* fitWidthBtn_ = nullptr;
    Button* fitPageBtn_ = nullptr;
    // --- annotation editing -------------------------------------------
    AnnotationStore annots_;
    std::vector<Button*> annotColorSw_;

    // --- compact tool options, in the command bar ------------------------
    // Every option a tool has, at the end of the command bar, one icon
    // each. The side panel carries the same settings; this is the fast
    // path that does not need a trip across the window mid-stroke.

    /// What the option controls are currently editing. One set of controls
    /// serves three jobs, and without this they would silently write to the
    /// wrong place - picking a colour for a selected annotation would
    /// change the pen instead.
    enum class OptTarget { Tool, Markup, Annotation };
    OptTarget optTarget_ = OptTarget::Tool;

    Button* toolOptColor_ = nullptr;     ///< current ink, as a chip
    Button* toolOptSize_ = nullptr;      ///< stroke weight / text size
    Button* toolOptOpacity_ = nullptr;   ///< opacity
    Button* toolOptFont_ = nullptr;      ///< text family
    Popover* toolColorPop_ = nullptr;
    Popover* toolSizePop_ = nullptr;
    Popover* toolOpacityPop_ = nullptr;
    Popover* toolFontPop_ = nullptr;
    std::vector<Button*> toolColorItems_;
    std::vector<Button*> toolSizeItems_;
    std::vector<Button*> toolOpacityItems_;
    std::vector<Button*> toolFontItems_;
    Slider* toolOpacitySlider_ = nullptr;

    /// Ink shared by the Highlight tool and text-selection markup, so the
    /// two cannot disagree about what colour a highlight is.
    int markupColorIdx_ = 0;

    std::uint64_t selectedAnnot_ = 0;

    /// Set while the option controls are being written to from code.
    ///
    /// SegmentedControl::setSelected and Slider::setValue both fire
    /// onChange, so refreshing the panel to match a selected annotation
    /// would call straight back into the apply path - recursing, and
    /// pushing a spurious undo entry every time an annotation was merely
    /// *selected*. Every apply function returns early while this is set.
    bool syncingOpts_ = false;

    void buildToolOptions();
    void syncToolOptions();
    void openOptPopover(Popover* pop, Button* anchor);

    /// The single way a popover is opened.
    ///
    /// Three things were being done by hand at six call sites and were
    /// wrong at two of them: telling the popover how big the window is
    /// (without it the clamp runs against a hard-coded 1440x900 and the
    /// menu walks off a narrower window), anchoring on the trigger's
    /// *centre* rather than its left edge, and closing whatever else was
    /// already open.
    void openPopoverFor(Popover* pop, Widget* trigger, bool above);

    /// Close every popover except `keep`. Two open at once is never
    /// intentional and looks like one torn popup.
    void closeOtherPopovers(const Popover* keep);
    /// Apply an option to whatever optTarget_ points at.
    void applyOptColor(int inkIndex);
    void applyOptWidth(int widthIndex);
    void applyOptOpacity(float v);
    void applyOptFont(int familyIndex);
    void onAnnotPicked(std::uint64_t id);

    /// Apply `mutate` to the selected annotation *and every other line of
    /// the same markup*, as one undo entry. Returns false if nothing is
    /// selected.
    bool modifySelectedGroup(const std::function<void(Annotation&)>& mutate,
                             const char* label);

    /// Last size class laid out, so a crossing can be acted on once rather
    /// than every frame.
    Breakpoint lastBreakpoint_ = Breakpoint::Expanded;
    /// The pill's layout button drops its label on a phone, so the text
    /// has to be remembered rather than recomputed from the button.
    std::string viewLabelFull_ = "Continuous";
    void onBreakpointChanged(const LayoutMetrics& m);
    /// True when the right panel should show style controls: either a tool
    /// is armed, or an annotation is selected and being edited. Three
    /// places need this and they must agree - the layout pass rewrites the
    /// visibility that updateRightPanelContext() sets, so a disagreement
    /// shows up as controls that flash on and vanish.
    [[nodiscard]] bool stylingActive() const;
    /// True when what is being styled is free text, which takes font and
    /// size controls instead of a stroke width.
    [[nodiscard]] bool stylingText() const;
    [[nodiscard]] Annotation* selectedAnnotation();
    WidthStrip* annotWidthSeg_ = nullptr;
    /// Free text is the one tool where stroke width means nothing and
    /// typeface and point size mean everything, so it swaps the WIDTH row
    /// for its own two.
    SegmentedControl* textFontSeg_ = nullptr;
    SegmentedControl* textSizeSeg_ = nullptr;
    void syncTextStyle();
    Slider* annotOpacity_ = nullptr;
    Label* annotHdr_ = nullptr;
    int annotColorIdx_ = 0;
    /// Ink colour remembered per tool. A highlighter wants yellow; free
    /// text wants near-black, because yellow text on white paper cannot be
    /// read. Sharing one index across every tool makes one of them useless.
    /// Indexed by Tool: Select, Hand, Highlight, Pen, Eraser, Text, Note,
    /// Stamp, Signature, Shapes. 5 is near-black ink, 0 is highlighter
    /// yellow - only the highlighter wants yellow.
    std::array<int, 10> toolColor_{5, 5, 0, 5, 5, 5, 5, 5, 5, 5};
    void applyToolColour();
    /// Push the current tool + style down into the stage.
    void syncTool();
    /// Show tool properties or display options depending on the mode.
    void updateRightPanelContext();

    Button* viewBtn_ = nullptr;          // pill: current layout, opens layoutPop_
    Popover* layoutPop_ = nullptr;
    std::vector<Button*> layoutItems_;
    void setLayoutMode(int index);

    // popovers
    Popover* modePop_ = nullptr;
    std::vector<Button*> modeItems_;
    Popover* findPop_ = nullptr;
    TextField* findField_ = nullptr;
    Popover* displayPop_ = nullptr;
    WindowControls* winCtl_ = nullptr;
    Popover* selPop_ = nullptr;
    std::vector<Button*> selBtns_;
    Button* selColorBtn_ = nullptr;
    // Icon-only: four 36px cells plus padding. The labelled version was
    // 404px wide and covered the line underneath the one being marked up.
    // Five cells now: three markups, the colour chip, and Copy.
    static constexpr float kSelPopW = 224.f;
    static constexpr float kSelPopH = 44.f;
    void buildSelectionPopup();
    void closeSelectionPopup();
    void applyTextMarkup(AnnotKind kind);

    UndoStack edits_;
    void doUndo();
    void doRedo();
    void syncUndoButtons();
    void reloadAfterUndo(int toPage);
    void dropDanglingSelection();

    Button* dayBtn_ = nullptr;
    Button* nightBtn_ = nullptr;
    void syncSchemeButtons();
    void chooseScheme(ColorScheme s, Widget* from);
    float chromeLead_ = 0.f;
    SegmentedControl* pageThemeSeg_ = nullptr;
    Slider* gridSlider_ = nullptr;

    /// Computed in onLayout so the labels and the buttons they sit
    /// between can never disagree about where the gaps are.
    Rect pageLabelRect_{};
    Rect zoomLabelRect_{};

    /// The frame time of the most recent paint. Event handlers run between
    /// frames and have no clock of their own, and passing 0.0 to an
    /// animation makes it evaluate at its own start time forever - which is
    /// exactly why the panel collapse silently did nothing.
    

    AnimatedF focusT_{0.f};
    std::string pageLabel_ = "1 / 12";
    std::string docTitle_ = "Document";

    /// The page counter in the zoom pill doubles as a jump-to-page field:
    /// click it, type, Enter. Shows "3 / 24" when idle and just "3" while
    /// editing, so the thing being typed over is the thing being replaced.
    TextField* pageField_ = nullptr;
    void commitPageField();

    // --- open documents ----------------------------------------------------
    // One tab per open file. Switching re-reads the page source from disk
    // (PDFium is cheap to re-open and holding N documents resident is not
    // worth it for a reader), but the per-document state that cannot be
    // recovered from the file - annotations and the page you were on - is
    // stashed here and restored.
    struct OpenDoc {
        std::string path;
        std::string title;
        std::vector<Annotation> annots;
        int page = 1;
    };
    std::vector<OpenDoc> docs_;
    int activeDoc_ = -1;
    /// Load a file into the view. Does not touch the tab strip or `docs_`.
    bool loadPath(const std::string& path, double now);
    /// Copy the live annotations and page number into `docs_[activeDoc_]`.
    void stashActiveDoc();
    void switchToDoc(int index, double now);
    void closeDoc(int index, double now);
    int pageCount_ = 12;
};

}  // namespace folio::ui
