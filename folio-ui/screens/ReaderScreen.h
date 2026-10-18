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
#if FOLIO_HAVE_PDFIUM_SOURCE
#include "pdfium/PdfiumWorker.h"
#endif

#include <functional>
#include <future>
#include <memory>
#include <unordered_set>
#include <vector>

namespace folio::engine { class PdfiumPageSource; }

namespace folio::ui {

/// Left panel content: page thumbnails with a current-page highlight.
class ThumbnailList : public ScrollView {
public:
    explicit ThumbnailList(AppState& s);

    std::function<void(int)> onPick;
    void setPageCount(int n);

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
    std::function<void(std::size_t)> onPickEntry;
    void setHeader(std::string header, std::string empty) { header_ = std::move(header); empty_ = std::move(empty); }
    void setPageCount(int n) { pages_ = n; }

    /// One heading from the document's own /Outlines tree.
    struct Entry {
        std::string title;
        int page = -1;
        int depth = 0;
    };
    void setEntries(std::vector<Entry> e);
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;

private:
    AppState& state_;
    int pages_ = 12;
    int hover_ = -1;
    std::vector<Entry> entries_;
    std::string header_ = "Outline";
    std::string empty_ = "This document has no bookmarks.";
};

class ReaderScreen : public Widget {
public:
    explicit ReaderScreen(AppState& state);

    std::function<void()> onBackToWelcome;
    std::function<void()> onDocumentChanged;
    void trimIdleVisualCaches();
    void releaseVisualCaches();
    void setWorkerWakeCallback(std::function<void()> wake);
    [[nodiscard]] bool hasDocumentWork() const;
    std::function<void()> onWorkerWake;
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
    /// Host publishes a fully written, validated private PDF through SAF.
    std::function<bool(const std::string&, const std::string&)> onExportPdf;
    std::function<bool(const std::string&, const std::string&)> onSharePdf;
    bool pollExport();
    void exportFinished(int status, const std::string& message);

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
    /// Install a PDF already parsed on a background loader, without blocking
    /// the window thread on PDFium's document open and metadata scan.
    bool openPreparedFile(const std::string& path,
                          std::shared_ptr<IPageSource> prepared, double now);
    void setTextInputCallback(std::function<void(bool)> callback) {
        stage_->onTextInputRequested = std::move(callback);
    }

    /// Desktop writes back incrementally with PoDoFo. Android asynchronously
    /// stages an annotated PDF and exports a separate copy through SAF;
    /// the imported original and in-memory marks remain intact.
    bool saveDocument();
    bool shareDocument();
    /// Page tree edits. All of them save, then reopen so the reader shows
    /// the real file rather than an optimistic guess at what it now holds.
    bool applyPageOp(const std::string& op);

    void onLayout() override;
    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    void onPaintOverlay(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onKey(const KeyEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;
    /// Geometry/page content is moving; clipping to a chrome hover region
    /// would leave stale pixels behind, so paint the whole retained scene.
    [[nodiscard]] bool requiresFullFrame(double now) const;

    /// Called by the app before the router runs, so an outside click can
    /// dismiss an open popover.
    void handleOutsideClick(Point windowPoint, double now);
    /// Reset the focus-controls idle deadline only for actual contact/key
    /// activity. Hover must not keep a phone awake or repainting.
    void noteFocusInteraction(const PointerEvent& e);
    void syncFocusAutoHidePreference(); // called after recents/preferences load

    [[nodiscard]] PageStage* stage() const noexcept { return stage_; }
    [[nodiscard]] std::string currentFilePath() const {
        return activeDoc_ >= 0 && activeDoc_ < static_cast<int>(docs_.size())
            ? docs_[static_cast<std::size_t>(activeDoc_)].path : std::string{};
    }

private:
    void buildTitleBar();
    void buildCommandBar();
    void buildWorkArea();
    void buildPill();
    void buildPopovers();
    void rebuildToolButtons(double now);
    void selectTool(Tool tool);
    void installPanelPainters();
    void setMode(Mode m);
    void toggleLeftPanel();
    void toggleRightPanel();
    void syncPageLabel();
    void applyPageTheme(PageTheme theme);

#if FOLIO_HAVE_PDFIUM_SOURCE
    std::shared_ptr<folio::engine::PdfiumPageSource> exportSource_;
    bool exportPending_ = false;
    bool sharingExport_ = false;
    std::string desktopExportPath_;
    std::string desktopStagingPath_;
#endif
    AppState& state_;
    bool layoutWasAnimating_ = false;
    bool panelLayoutDirty_ = false; // state changed even if next frame is late

    // title bar
    TabBar* tabs_ = nullptr;
    Button* backBtn_ = nullptr;
    Button* searchBtn_ = nullptr;

    // --- phone chrome ------------------------------------------------------
    // A tab strip needs width a phone does not have, so the title bar
    // carries the current document name as a dropdown instead. The former
    // sun/moon button remains in the phone title bar; only the redundant
    // command-bar Display shortcut is hidden on phones.
    Button* docBtn_ = nullptr;
    Button* docListBtn_ = nullptr; // fixed beside desktop window controls
    Popover* docPop_ = nullptr;
    ScrollView* docMenuScroll_ = nullptr;
    Widget* docMenuContent_ = nullptr;
    std::vector<Button*> docItems_;
    std::vector<std::weak_ptr<IPageSource>> warmVisuals_; // LRU, bounded inactive previews
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
    Button* toolOverflowBtn_ = nullptr;
    Popover* toolOverflowPop_ = nullptr;
    std::vector<Button*> toolOverflowItems_;
    Popover* shapePop_ = nullptr;
    std::vector<Button*> shapeItems_;
    PageStage::EditTool drawingShape_ = PageStage::EditTool::Ink;
    void openDrawingMenu(Button* anchor);
    void chooseDrawingShape(PageStage::EditTool shape);
    Button* displayBtn_ = nullptr;
    Button* leftToggle_ = nullptr;
    Button* rightToggle_ = nullptr;
    Button* focusBtn_ = nullptr;
    Button* deleteAnnotBtn_ = nullptr;
    Button* focusPanelBtn_ = nullptr;       // pinned in right-panel header, all modes
    Button* undoPanelBtn_ = nullptr;        // same per-document UndoStack as desktop
    Button* redoPanelBtn_ = nullptr;
    Button* rotateLeftBtn_ = nullptr;
    Button* rotateRightBtn_ = nullptr;
    Button* focusAutoHideBtn_ = nullptr;    // inspector preference, all modes
    Button* selectionMagnifierBtn_ = nullptr; // saved reader-display preference
    Button* focusExitBtn_ = nullptr;        // icon-only, shown on demand
    Widget* focusWakeZone_ = nullptr;       // invisible top-right phone escape
    bool focusControlsVisible_ = false;
    double focusHideAt_ = 0.0;
    std::unordered_set<PointerId> focusTouches_; // never hide while a finger is down
    void setFocusMode(bool on);
    void showFocusControls(bool on);
    void toggleFocusControlsFromPage();
    Button* shareBtn_ = nullptr;

    // work area
    SidePanel* leftPanel_ = nullptr;
    SidePanel* rightPanel_ = nullptr;
    ScrollView* inspectorScroll_ = nullptr;
    Widget* inspectorContent_ = nullptr;
    ThumbnailList* thumbs_ = nullptr;
    OutlineList* outline_ = nullptr;
    OutlineList* notes_ = nullptr;
    std::vector<std::uint64_t> noteIds_;
    std::vector<std::pair<int, int>> pdfNoteRefs_;
    int notesScanCursor_ = 0;
    std::vector<OutlineList::Entry> pdfNotesEntries_;
    std::vector<std::pair<int, int>> pdfNotesRefs_;
#if FOLIO_HAVE_PDFIUM_SOURCE
    std::vector<folio::engine::PdfAnnotationInfo> pdfNotesInfo_;
    std::uint64_t adoptSourceAnnotation(int page, const folio::engine::PdfAnnotationInfo& info);
    void syncImportedSuppression();
#endif
    std::uint64_t notesRevision_ = ~std::uint64_t{0};
    ScrollView* layersScroll_ = nullptr;
    Widget* layersContent_ = nullptr;
    std::vector<Button*> layerItems_;
    void syncNotes();
    unsigned lastOutlineRevision_ = 0;
    PageStage* stage_ = nullptr;
    std::shared_ptr<IPageSource> pageSource_;

    std::vector<PageStage::TextRun> searchHits_;
    int                             searchIndex_ = -1;
    std::u32string                  searchNeedle_;
    int                             searchCursor_ = 0;
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
    Button* zoomPctBtn_ = nullptr; // transparent hit target beneath painted percentage
    Button* fitWidthBtn_ = nullptr;
    Button* fitPageBtn_ = nullptr;
    // --- annotation editing -------------------------------------------
    AnnotationStore annots_;
    std::vector<Button*> annotColorSw_;
    Label* commentLabel_ = nullptr;
    TextField* commentField_ = nullptr;
    Button* saveCommentBtn_ = nullptr;
    std::uint64_t commentTarget_ = 0;
    std::string commentOriginal_;
    void commitComment();
    [[nodiscard]] std::string sidecarPath(const std::string& pdfPath) const;
    void persistAnnotations();

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

    float lastOptionLayoutWidth_ = -1.f;
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
    float markupOpacity_ = 1.f;

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
    void onAnnotPicked(std::uint64_t id, bool fromTouch);

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
    Popover* zoomPop_ = nullptr;
    std::vector<Button*> zoomPresetItems_;
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
    // Open PDF workers remain mapped for fast switches; only the visible
    // document retains decoded visual tiles. The lightweight snapshot below
    // restores the actual viewport/reader controls without retaining pixels.
    struct OpenDoc {
        std::string path;
        std::string title;
        std::vector<Annotation> annots;
        std::shared_ptr<IPageSource> source;
        PageStage::ViewState view{};
        PageTheme pageTheme = PageTheme::Light;
        std::uint64_t selectedAnnot = 0;
        float thumbnailScroll = 0.f;
        float outlineScroll = 0.f;
        int leftTab = 0;
        bool leftCollapsed = true;
        bool rightCollapsed = true;
        bool focusMode = false;
        bool hasView = false;
        int page = 1;
        int pages = 1;
    };
    std::vector<OpenDoc> docs_;
    int activeDoc_ = -1;
    // Closing a busy document must not join its PDFium worker on the UI
    // thread. Futures own retired sources until cleanup finishes; their
    // destruction at application shutdown still waits for safe teardown.
    std::vector<std::future<void>> retiringDocs_;
    void retireClosedSource(std::shared_ptr<IPageSource> source);
    void reapRetiredSources();
    /// Load a file into the view. Does not touch the tab strip or `docs_`.
    bool loadPath(const std::string& path, double now,
                  std::shared_ptr<IPageSource> prepared = {});
    /// Copy the live annotations and page number into `docs_[activeDoc_]`.
    void stashActiveDoc();
    void switchToDoc(int index, double now);
    void closeDoc(int index, double now);
    int pageCount_ = 12;
};

}  // namespace folio::ui
