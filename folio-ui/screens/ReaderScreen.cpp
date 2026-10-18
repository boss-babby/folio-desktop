#include "screens/ReaderScreen.h"

#if FOLIO_HAVE_PDFIUM_SOURCE
#include "pdfium/PdfiumPageSource.h"
#include <fpdf_annot.h>
#endif
#if FOLIO_HAVE_PODOFO_SOURCE
#include "podofo/PodofoDocument.h"
#endif

#include "folio/ui/Icons.h"
#if FOLIO_HAVE_PDFIUM_SOURCE
#include "AnnotationSpec.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>

namespace folio::ui {

#if FOLIO_HAVE_PDFIUM_SOURCE
namespace {
std::filesystem::path nativePath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}
std::string utf8Path(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}
} // namespace
#endif

namespace {
// Keep the dropdown's hit regions in sync with inertial/wheel scrolling in
// the SAME frame, before its button children are painted or hit-tested.
class DocumentMenuScroll final : public ScrollView {
public:
    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override {
        ScrollView::onPaint(t, ctx);
        if (!children().empty()) children().front()->frame.y = -scrollY();
    }
};

/// The ink palette. Shared by the side-panel swatches, the command-bar
/// chip and the popover grid, so there is one definition of "yellow".
///
/// Laid out as four rows of six, and read that way by every grid that
/// shows it. The first six are the original inks and their indices are
/// deliberately unchanged: annotations already on disk, the default
/// markup colour and every saved document all resolve through these.
///   row 0  vivid    - the classic highlighter set
///   row 1  pastel   - light enough to mark over dark body text
///   row 2  deep     - for ink, boxes and signatures on pale paper
///   row 3  neutral  - greys, plus brown and violet
constexpr int kInkCols = 6;
constexpr int kInkCount = 24;
constexpr Color kToolInks[kInkCount] = {
    // vivid
    {250, 204, 21, 255},  {244, 114, 182, 255}, {74, 222, 128, 255},
    {96, 165, 250, 255},  {248, 113, 113, 255}, {30, 32, 44, 255},
    // pastel
    {254, 240, 138, 255}, {251, 207, 232, 255}, {187, 247, 208, 255},
    {191, 219, 254, 255}, {254, 202, 202, 255}, {233, 213, 255, 255},
    // deep
    {234, 88, 12, 255},   {190, 24, 93, 255},   {13, 148, 136, 255},
    {37, 99, 235, 255},   {185, 28, 28, 255},   {91, 33, 182, 255},
    // neutral
    {255, 255, 255, 255}, {203, 213, 225, 255}, {148, 163, 184, 255},
    {71, 85, 105, 255},   {120, 83, 44, 255},   {124, 58, 237, 255}};

/// Stroke weights, in the same page units as Annotation::width. Was three
/// values duplicated at four call sites, which is how a palette drifts out
/// of step with the control that sets it.
constexpr int kWeightCount = 6;
constexpr float kToolWidths[kWeightCount] = {0.6f, 1.2f, 2.5f, 4.f, 6.5f, 10.f};

/// Closest palette entry to an arbitrary colour.
///
/// An annotation stores a Color, not an index - it may have come from a
/// file, or from a tint rather than a base ink - so showing which swatch is
/// "current" means measuring. Plain squared RGB distance is enough to pick
/// the right chip out of six well-separated hues.
int nearestInk(const Color& c) {
    int best = 0;
    long bestD = 1L << 30;
    for (int i = 0; i < kInkCount; ++i) {
        const long dr = static_cast<long>(c.r) - kToolInks[i].r;
        const long dg = static_cast<long>(c.g) - kToolInks[i].g;
        const long db = static_cast<long>(c.b) - kToolInks[i].b;
        const long d = dr * dr + dg * dg + db * db;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}
}  // namespace

namespace {

constexpr float kThumbW = tokens::size::thumbnail::width;
constexpr float kThumbH = tokens::size::thumbnail::height;
constexpr float kPanelPad = 10.f;
// Prototype .tgrid / .thb metrics.
constexpr float kGridGapX = 12.f;
constexpr float kGridGapY = 6.f;
// The prototype's .thb is 162 tall with a 108-wide 7:10 page (154.3) and a
// 22px label strip, which overlap by 14px. Giving the label its own band
// instead keeps the number legible.
constexpr float kThumbPageH = 154.f;
constexpr float kThumbLabelH = 20.f;
constexpr float kThumbCellH = kThumbPageH + kThumbLabelH;
constexpr float kCmdH = tokens::size::commandBar;
constexpr float kPillH = tokens::size::pill;
constexpr double kFocusIdleSeconds = 3.0;

// Only active while immersive controls are hidden on a phone. It occupies a
// small top-corner touch area, so an armed annotation tool can still reveal
// the exit control without drawing an accidental mark on the PDF. The rest
// of the canvas remains available for pinch, scroll and drawing.
class FocusWakeZone final : public Widget {
public:
    FocusWakeZone() { acceptsPointer = true; debugName = "Focus wake zone"; }
    std::function<void()> onWake;
    bool onPointer(const PointerEvent& e) override {
        if (e.phase == PointerPhase::Up && onWake) onWake();
        return e.phase == PointerPhase::Down || e.phase == PointerPhase::Up;
    }
};
constexpr float kRowH = 34.f;

}  // namespace

// The inspector owns a real scroll viewport. Moving only its content child
// avoids relaying out 24 colour buttons on every drag/fling frame.
class InspectorScrollView final : public ScrollView {
public:
    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override {
        ScrollView::onPaint(t, ctx);
        if (!children().empty()) children().front()->frame.y = -scrollY();
    }
    bool onPointer(const PointerEvent& e) override {
        const bool handled = ScrollView::onPointer(e);
        if (!children().empty()) children().front()->frame.y = -scrollY();
        return handled;
    }
};

class InspectorContent final : public Widget {
public:
    std::function<void(IPaintTarget&, const PaintCtx&)> drawOverlay;
    InspectorContent() { acceptsPointer = false; }
    void onPaintOverlay(IPaintTarget& t, const PaintCtx& ctx) override {
        if (drawOverlay) drawOverlay(t, ctx);
    }
};

// ---------------------------------------------------------------------------
// ThumbnailList
// ---------------------------------------------------------------------------

ThumbnailList::ThumbnailList(AppState& s) : state_(s) {
    debugName = "ThumbnailList";
}

void ThumbnailList::setPageCount(int n) {
    pages_ = n;
    contentHeight = static_cast<float>(rows()) * (kThumbCellH + kGridGapY) + 10.f;
}

Rect ThumbnailList::thumbRect(int i) const {
    // Prototype .tgrid: two equal columns, 12px column gap, 6px row gap,
    // items 108x162 with the page itself 108 wide at the 7:10 page ratio
    // and the number sitting in the 22px strip below it.
    const float colW = (frame.w - kGridGapX) * 0.5f;
    const float x = static_cast<float>(i % 2) * (colW + kGridGapX) +
                    (colW - kThumbW) * 0.5f;
    const float y = static_cast<float>(i / 2) * (kThumbCellH + kGridGapY) + 2.f -
                    scrollY_;
    return Rect{x, y, kThumbW, kThumbPageH};
}

void ThumbnailList::setCurrent(int oneBased, double now) {
    if (oneBased == current_) return;
    current_ = oneBased;
    const float top = static_cast<float>((oneBased - 1) / 2) * (kThumbCellH + kGridGapY);
    revealRange(top, kThumbCellH, now, tokens::dur::paneSwitch);
}

bool ThumbnailList::onPointer(const PointerEvent& e) {
    if (ScrollView::onPointer(e)) return true;
    switch (e.phase) {
        case PointerPhase::Move:
        case PointerPhase::Enter: {
            int next = -1;
            for (int i = 0; i < pages_; ++i) {
                const Rect r = thumbRect(i);
                if (r.contains(e.pos.x, e.pos.y)) { next = i; break; }
            }
            if (next == hover_) return false; // unchanged motion needs no frame
            hover_ = next;
            return true;
        }
        case PointerPhase::Leave: {
            const bool changed = hover_ != -1;
            hover_ = -1;
            return changed;
        }
        case PointerPhase::Up:
            if (hover_ >= 0 && onPick) onPick(hover_ + 1);
            return true;
        case PointerPhase::Down: return true;
        default: break;
    }
    return false;
}

void ThumbnailList::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    ScrollView::onPaint(t, ctx);
    contentHeight = static_cast<float>(rows()) * (kThumbCellH + kGridGapY) + 10.f;
    if (source_) {
        const float rowH = kThumbCellH + kGridGapY;
        const int first = std::clamp(static_cast<int>(scrollY_ / rowH) * 2,
                                     0, std::max(0, pages_ - 1));
        const int last = std::min(pages_ - 1, first +
            (static_cast<int>(std::ceil((frame.h + 80.f) / rowH)) + 1) * 2 - 1);
        source_->setVisibleThumbnailRange(pages_ > 0 ? first : -1, last);
    }

    const Color paper = (state_.theme.pageTheme() == PageTheme::Dark)
                            ? tokens::pageTheme_dark.paper
                            : (state_.theme.pageTheme() == PageTheme::Sepia
                                   ? tokens::pageTheme_sepia.paper
                                   : tokens::pageTheme_light.paper);

    for (int i = 0; i < pages_; ++i) {
        const Rect r = thumbRect(i);
        if (r.bottom() < -40.f || r.y > frame.h + 40.f) continue;  // cull

        const bool isCur = (i + 1) == current_;
        const bool isHov = i == hover_;

        if (isHov && !isCur) {
            t.fillRoundedRect(r.inset(-5.f), Corners::all(8.f), ctx.pal.hover);
        }
        drawElevation(t, r, Corners::all(3.f), tokens::elevation::pageSmall(ctx.pal),
                      1.f, /*interiorOpaque=*/true);
        t.fillRoundedRect(r, Corners::all(3.f), paper);

        // Draw the real page, scaled down. The prototype uses the very
        // same page markup at --pw:108px, so a thumbnail is never allowed
        // to drift from the page it stands for.
        if (source_) {
            t.save();
            t.clipRoundedRect(r, Corners::all(3.f), true);
            source_->drawPage(t, i, r, r.w / 560.f, state_.theme.pageTheme());
            t.restore();
        }

        if (isCur) {
            t.strokeRoundedRect(r.inset(-2.5f), Corners::all(6.f), ctx.pal.accent, 2.f);
        }
        t.drawTextInBox(std::to_string(i + 1),
                        Rect{r.x, r.bottom() + 2.f, r.w, kThumbLabelH - 2.f},
                        styleFor(tokens::type_small,
                                 isCur ? ctx.pal.accentText : ctx.pal.muted),
                        folio::render::TextAlign::Centre);
    }
}

// ---------------------------------------------------------------------------
// OutlineList
// ---------------------------------------------------------------------------

OutlineList::OutlineList(AppState& s) : state_(s) { debugName = "OutlineList"; }

void OutlineList::setEntries(std::vector<Entry> e) {
    entries_ = std::move(e);
    contentHeight = kPanelPad * 2.f +
        static_cast<float>(std::max<std::size_t>(1, entries_.size())) * kRowH + 30.f;
}

bool OutlineList::onPointer(const PointerEvent& e) {
    if (ScrollView::onPointer(e)) return true;
    switch (e.phase) {
        case PointerPhase::Move:
        case PointerPhase::Enter: {
            // The 22 px matches the "Outline" header that onPaint reserves
            // above the first row. Leaving it out made every click land on
            // the row below the one under the cursor.
            int next = static_cast<int>(
                std::floor((e.pos.y + scrollY_ - kPanelPad - 22.f) / kRowH));
            if (next < 0 || next >= static_cast<int>(entries_.size())) next = -1;
            if (next == hover_) return false;
            hover_ = next;
            return true;
        }
        case PointerPhase::Leave: {
            const bool changed = hover_ != -1;
            hover_ = -1;
            return changed;
        }
        case PointerPhase::Up:
            // Report the entry's own destination page, not the row index.
            if (hover_ >= 0 && hover_ < static_cast<int>(entries_.size())) {
                if (onPickEntry) onPickEntry(static_cast<std::size_t>(hover_));
                const int p = entries_[static_cast<std::size_t>(hover_)].page;
                if (p >= 0 && onPick) onPick(p + 1);
            }
            return true;
        case PointerPhase::Down: return true;
        default: break;
    }
    return false;
}

void OutlineList::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    ScrollView::onPaint(t, ctx);

    const int n = static_cast<int>(entries_.size());
    contentHeight = kPanelPad * 2.f + static_cast<float>(std::max(1, n)) * kRowH + 30.f;

    t.drawTextInBox(header_, Rect{kPanelPad + 2.f, 4.f, frame.w - 20.f, 18.f},
                    styleFor(tokens::type_sectionHeader, ctx.pal.muted),
                    folio::render::TextAlign::Left);

    // Most PDFs in the wild have no /Outlines at all. Saying so is more
    // use than the invented "Chapter 1 / Section 1.1" ladder that used to
    // stand here, which looked like data and was not.
    if (n == 0) {
        t.drawTextInBox(empty_,
                        Rect{kPanelPad + 2.f, kPanelPad + 26.f,
                             frame.w - kPanelPad * 2.f, 20.f},
                        styleFor(tokens::type_body, ctx.pal.muted),
                        folio::render::TextAlign::Left);
        return;
    }

    for (int i = 0; i < n; ++i) {
        const float y = kPanelPad + 22.f + static_cast<float>(i) * kRowH - scrollY_;
        if (y < -kRowH || y > frame.h) continue;
        const Entry& e = entries_[static_cast<std::size_t>(i)];
        const Rect row{6.f, y, frame.w - 18.f, kRowH - 4.f};
        if (i == hover_) {
            t.fillRoundedRect(row, Corners::all(tokens::radius::xs), ctx.pal.hover);
        }
        const bool top = e.depth == 0;
        const float indent = 10.f + static_cast<float>(std::min(e.depth, 4)) * 12.f;
        t.drawTextInBox(e.title,
                        Rect{row.x + indent, row.y, row.w - indent - 34.f, row.h},
                        styleFor(top ? tokens::type_bodyStrong : tokens::type_body,
                                 top ? ctx.pal.text : ctx.pal.muted),
                        folio::render::TextAlign::Left);
        if (e.page >= 0) {
            t.drawTextInBox(std::to_string(e.page + 1),
                            Rect{row.x, row.y, row.w - 8.f, row.h},
                            styleFor(tokens::type_small, ctx.pal.muted),
                            folio::render::TextAlign::Right);
        }
    }
}

// ---------------------------------------------------------------------------
// ReaderScreen
// ---------------------------------------------------------------------------

ReaderScreen::ReaderScreen(AppState& state) : state_(state) {
    debugName = "ReaderScreen";
    buildTitleBar();
    buildCommandBar();
    buildWorkArea();
    leftPanel_->setCollapsedImmediately(state_.leftPanelCollapsed);
    rightPanel_->setCollapsedImmediately(state_.rightPanelCollapsed);
    buildPill();
    buildPopovers();
    buildToolOptions();
    stage_->setStore(&annots_);
#if FOLIO_HAVE_PDFIUM_SOURCE
    stage_->onImportedAnnotHit = [this](int page, Point at) -> std::uint64_t {
        auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(pageSource_);
        if (!pdf) return 0;
        const auto* list = pdf->annotationsForPage(page);
        if (!list) return 0; // metadata is requested lazily on the worker
        for (auto it = list->rbegin(); it != list->rend(); ++it) {
            if (!it->editable || at.x < it->l - .008f || at.x > it->r + .008f ||
                at.y < it->t - .008f || at.y > it->b + .008f) continue;
            return adoptSourceAnnotation(page, *it);
        }
        return 0;
    };
#endif
    stage_->onAnnotationsChanged = [this] { syncPageLabel(); };
    stage_->onAnnotationAdded = [this](const Annotation& a) {
        edits_.push(std::make_unique<AddAnnotCommand>(annots_, a));
        persistAnnotations();
        syncUndoButtons();
    };
    stage_->onAnnotationDeleted = [this](const Annotation& a) {
        edits_.push(std::make_unique<RemoveAnnotCommand>(annots_, a));
        persistAnnotations();
        syncUndoButtons();
    };
    stage_->onAnnotationGroupDeleted = [this](std::vector<Annotation> copies) {
        edits_.push(std::make_unique<RemoveManyCommand>(annots_, std::move(copies)));
        persistAnnotations();
        syncUndoButtons();
    };
    stage_->onAnnotationMoved = [this](const Annotation& before,
                                       const Annotation& after) {
        edits_.push(std::make_unique<ModifyAnnotCommand>(annots_, before, after,
                                                         "move annotation"));
        persistAnnotations();
        syncUndoButtons();
    };
    syncTool();
    syncUndoButtons();
    modePop_->setOwner(modeBtn_);
    layoutPop_->setOwner(viewBtn_);
    zoomPop_->setOwner(zoomPctBtn_);
    displayPop_->setOwner(displayBtn_);
    findPop_->setOwner(searchBtn_);
    installPanelPainters();
    // Created early so the focus toggle can reach it, but it floats over
    // the canvas, so it belongs in the floating layer rather than merely
    // being shuffled to the end of the list.
    focusExitBtn_->setZOrder(zlayer::floating);
}

void ReaderScreen::buildTitleBar() {
    tabs_ = emplace<TabBar>();
    tabs_->onNew = [this] {
        // A new tab with nothing in it is a dead end; ask for a document.
        if (onBrowse) onBrowse();
    };
    tabs_->onSelect = [this](int i) { switchToDoc(i, now()); };
    tabs_->onClose = [this](int i) { closeDoc(i, now()); };
    docListBtn_ = emplace<Button>("", "chevron-down", ButtonStyle::Icon);
    docListBtn_->tooltip = "All open PDFs";
    docListBtn_->onClick = [this] {
        if (docPop_->isOpen()) { docPop_->close(now()); return; }
        syncDocMenu();
        docPop_->setOwner(docListBtn_);
        openPopoverFor(docPop_, docListBtn_, false);
    };

    // The prototype's title bar has no way back to the library, which is a
    // gap in the prototype rather than a design decision: once a document is
    // open there is no route home short of closing the last tab. A home
    // button sits left of the tab strip.
    backBtn_ = emplace<Button>("", "home", ButtonStyle::Icon);
    backBtn_->tooltip = "Library (Ctrl W)";
    backBtn_->visible = true;
    backBtn_->onClick = [this] { if (onBackToWelcome) onBackToWelcome(); };

    winCtl_ = emplace<WindowControls>();
    winCtl_->onClose = [this] { if (onQuit) onQuit(); };
    cmdItems_.clear();
    buildPhoneChrome();
}

bool ReaderScreen::sheetMode() const {
    return rightPanel_ &&
           rightPanel_->presentation() == SidePanel::Presentation::Sheet;
}

void ReaderScreen::buildPhoneChrome() {
    // --- document name as a switcher --------------------------------------
    docBtn_ = emplace<Button>("Untitled", "", ButtonStyle::Ghost);
    docBtn_->trailingChevron = true;
    docBtn_->chevronHugsLabel = true;
    docBtn_->visible = false;
    docBtn_->tooltip = "Switch document";
    docBtn_->onClick = [this] {
        if (docPop_->isOpen()) { docPop_->close(now()); return; }
        syncDocMenu();
        docPop_->setOwner(docBtn_);
        // Drops straight below the name with the same circular reveal as
        // every other menu. A different animation here would read as a
        // different kind of surface.
        openPopoverFor(docPop_, docBtn_, false);
    };

    docPop_ = emplace<Popover>();
    docPop_->setOwner(docBtn_);
    docMenuScroll_ = docPop_->emplace<DocumentMenuScroll>();
    docMenuContent_ = docMenuScroll_->emplace<Widget>();

    // Keep the always-reachable title-bar theme shortcut on phones. It is
    // distinct from the Display control in the command bar, which is the
    // redundant icon that must be removed on mobile.
    themeBtn_ = emplace<Button>("", "sun", ButtonStyle::Icon);
    themeBtn_->visible = false;
    themeBtn_->tooltip = "Day / night";
    themeBtn_->onClick = [this] {
        const bool dark = state_.theme.scheme() == ColorScheme::Dark;
        // Same path the Day/Night buttons use, so the circular reveal
        // grows from this button rather than flashing the whole window.
        chooseScheme(dark ? ColorScheme::Light : ColorScheme::Dark, themeBtn_);
        themeBtn_->icon = dark ? "sun" : "moon";
    };

    // --- bottom-corner sheet handles --------------------------------------
    // Left thumb opens pages/outline, right thumb opens the inspector.
    // Both are reachable without shifting grip; the top-right corner,
    // where these options live on desktop, is not.
    pagesFab_ = emplace<Button>("", "thumbnails", ButtonStyle::Card);
    // Built with the title bar, long before the canvas exists, so without
    // a layer the stage would paint straight over both of these.
    pagesFab_->setZOrder(zlayer::floating);
    pagesFab_->visible = false;
    pagesFab_->tooltip = "Pages and outline";
    pagesFab_->onClick = [this] { togglePhoneSheet(true); };

    inspectFab_ = emplace<Button>("", "more", ButtonStyle::Card);
    inspectFab_->setZOrder(zlayer::floating);
    inspectFab_->visible = false;
    inspectFab_->tooltip = "Tool options";
    inspectFab_->onClick = [this] { togglePhoneSheet(false); };
}

void ReaderScreen::layOutPhoneChrome(const LayoutMetrics& m, float W, float H,
                                     float chromeY, float tb) {
    const bool phone = m.panelsOverlay;

    // Keep the title-bar sun/moon on a phone; only the command-bar Display
    // icon is redundant. Native window controls do not belong on Android.
#if defined(__ANDROID__)
    if (winCtl_) winCtl_->visible = false;
#else
    if (winCtl_) winCtl_->visible = true; // desktop chrome survives narrow windows
#endif
    const float titleTrailing = W - 12.f - (winCtl_->visible
                                ? winCtl_->preferredWidth() : 0.f);
    if (themeBtn_) {
        themeBtn_->visible = phone;
        if (phone) themeBtn_->setFrame(titleTrailing - 34.f,
                                       chromeY + (tb - 34.f) * 0.5f,
                                       34.f, 34.f);
    }

    // Tabs need width a phone has not got; the document name takes over,
    // and carries the switcher the tab strip used to be.
    if (docBtn_) {
        docBtn_->visible = phone;
        if (phone) {
            const float x = chromeLead_ + 86.f + 34.f;
            const float right = titleTrailing - 34.f - 8.f;
            docBtn_->label = docTitle_.empty() ? "Untitled" : docTitle_;
            docBtn_->setFrame(x, chromeY + (tb - 30.f) * 0.5f,
                              std::max(0.f, right - x), 30.f);
        }
    }

    // Bottom corners, outside the pill, so neither thumb has to travel to
    // the top of the screen to reach a panel.
    const float fab = 42.f;
    const float fy = H - m.gutter - fab;
    // A raised sheet covers both corners, so the handles would only show
    // as slivers poking out past its edge. They come back as it retracts.
    const float raised = std::max(leftPanel_->sheetProgress(now()),
                                  rightPanel_->sheetProgress(now()));
    const bool showFabs = phone && !state_.focusMode && raised < 0.5f;
    if (pagesFab_) {
        pagesFab_->visible = showFabs;
        pagesFab_->setFrame(m.gutter, fy, fab, fab);
    }
    if (inspectFab_) {
        inspectFab_->visible = showFabs;
        inspectFab_->setFrame(W - m.gutter - fab, fy, fab, fab);
    }
}

void ReaderScreen::togglePhoneSheet(bool left) {
    // Only one sheet at a time: two stacked surfaces on a phone leaves
    // almost no page visible, and the second would have to overlap the
    // first anyway.
    const bool wantOpen = left ? leftPanel_->collapsed()
                               : rightPanel_->collapsed();
    state_.leftPanelCollapsed = !(left && wantOpen);
    state_.rightPanelCollapsed = !(!left && wantOpen);
    leftPanel_->setCollapsed(state_.leftPanelCollapsed, now());
    rightPanel_->setCollapsed(state_.rightPanelCollapsed, now());
    panelLayoutDirty_ = true;
    syncPhoneFabs();
}

void ReaderScreen::syncPhoneFabs() {
    if (pagesFab_) pagesFab_->toggled = !state_.leftPanelCollapsed;
    if (inspectFab_) inspectFab_->toggled = !state_.rightPanelCollapsed;
    if (leftToggle_) leftToggle_->toggled = !state_.leftPanelCollapsed;
    if (rightToggle_) rightToggle_->toggled = !state_.rightPanelCollapsed;
}

void ReaderScreen::syncDocMenu() {
    docItems_.clear();
    docMenuContent_->removeAllChildren();

    constexpr float kRowH = 40.f;
    constexpr float w = 264.f;
    const int count = tabs_ ? static_cast<int>(tabs_->count()) : 0;
    float y = 6.f;
    for (int i = 0; i < count; ++i) {
        auto* b = docMenuContent_->emplace<Button>(tabs_->tabs()[i].title, "file-text",
                                           ButtonStyle::ListItem);
        b->toggled = (i == tabs_->active());
        b->setFrame(6.f, y, w - 12.f, kRowH);
        b->onClick = [this, i] {
            docPop_->close(now());
            switchToDoc(i, now());
        };
        docItems_.push_back(b);
        y += kRowH;
    }
    // The library is the only route to a second document in this build, so
    // the menu has to carry it or the switcher is a dead end.
    auto* open = docMenuContent_->emplace<Button>("Open a PDF...", "folder-open",
                                          ButtonStyle::ListItem);
    open->setFrame(6.f, y + 4.f, w - 12.f, kRowH);
    open->onClick = [this] {
        docPop_->close(now());
        if (onBrowse) onBrowse();
    };
    docItems_.push_back(open);
    const float totalH = y + kRowH + 14.f;
    const float height = std::min(totalH, std::max(96.f, frame.h - 96.f));
    docPop_->setFrame(0.f, 0.f, w, height);
    docMenuScroll_->setFrame(0.f, 0.f, w, height);
    docMenuScroll_->contentHeight = totalH;
    docMenuScroll_->restoreScrollY(0.f);
    docMenuContent_->setFrame(0.f, 0.f, w, totalH);
}

void ReaderScreen::buildCommandBar() {
    cmdBar_ = emplace<CardView>(tokens::radius::card);
    cmdBar_->elevation = [](const tokens::Palette& p) {
        return tokens::elevation::cardFlat(p);
    };

    modeBtn_ = cmdBar_->emplace<Button>(modeName(state_.mode), modeIcon(state_.mode),
                                        ButtonStyle::Ghost);
    modeBtn_->toggled = true;
    modeBtn_->tooltip = "Switch mode";
    // The prototype's #modeBtn is a dropdown with a chevron that flips
    // when the menu is open - not a button that silently cycles.
    modeBtn_->trailingChevron = true;
    modeBtn_->tooltip = "Switch mode";
    modeBtn_->onClick = [this] {
        if (modePop_->isOpen()) {
            modePop_->close(now());
            return;
        }
        openPopoverFor(modePop_, modeBtn_, false);
        modeBtn_->chevronOpen = true;
    };

    // Prototype order: open save print export | undo redo. Open and export
    // were missing; undo/redo were in front of them.
    struct Act { const char* icon; const char* tip; };
    static constexpr Act kActs[] = {
        {"folder-open", "Open  (Ctrl+O)"}, {"save", "Save  (Ctrl+S)"},
        {"print", "Print  (Ctrl+P)"},      {"upload", "Export  (Ctrl+E)"},
        {"undo", "Undo  (Ctrl+Z)"},        {"redo", "Redo  (Ctrl+Shift+Z)"},
    };
    for (const Act& a : kActs) {
        auto* b = cmdBar_->emplace<Button>("", a.icon, ButtonStyle::Icon);
        b->tooltip = a.tip;
        const std::string tip = a.tip;
        const bool isOpen = std::string_view(a.icon) == "folder-open";
        b->onClick = [this, tip, isOpen] {
            if (isOpen) { if (onBrowse) onBrowse(); return; }
            if (tip.rfind("Save", 0) == 0) { saveDocument(); return; }
            if (tip.rfind("Undo", 0) == 0) { doUndo(); return; }
            if (tip.rfind("Redo", 0) == 0) { doRedo(); return; }
            OverlayHost::current().showToast(tip, now());
        };
        cmdItems_.push_back(b);
    }

    toolHost_ = cmdBar_->emplace<Widget>();
    toolHost_->acceptsPointer = false;
    toolOverflowBtn_ = cmdBar_->emplace<Button>("", "more", ButtonStyle::Icon);
    toolOverflowBtn_->tooltip = "More tools";
    toolOverflowBtn_->visible = false;
    toolOverflowBtn_->onClick = [this] {
        if (toolOverflowPop_->isOpen()) toolOverflowPop_->close(now());
        else openPopoverFor(toolOverflowPop_, toolOverflowBtn_, false);
    };
    rebuildToolButtons(0.0);

    // Search lives in the command bar's right-hand cluster, next to the
    // display control - not as a wide pill in the title bar. It is one
    // icon: the field it opens is in the popover, so the chrome only has
    // to carry the affordance.
    searchBtn_ = cmdBar_->emplace<Button>("", "search", ButtonStyle::Icon);
    searchBtn_->tooltip = "Find in document  (Ctrl+F)";
    searchBtn_->onClick = [this] {
        openPopoverFor(findPop_, searchBtn_, false);
    };

    displayBtn_ = cmdBar_->emplace<Button>("", "settings", ButtonStyle::Icon);
    displayBtn_->tooltip = "Display options";
    displayBtn_->onClick = [this] {
        openPopoverFor(displayPop_, displayBtn_, false);
    };

    leftToggle_ = cmdBar_->emplace<Button>("", "sidebar", ButtonStyle::Icon);
    leftToggle_->tooltip = "Toggle thumbnails";
    leftToggle_->toggled = !state_.leftPanelCollapsed;
    leftToggle_->onClick = [this] { toggleLeftPanel(); };

    rightToggle_ = cmdBar_->emplace<Button>("", "panel-right", ButtonStyle::Icon);
    rightToggle_->tooltip = "Toggle outline";
    rightToggle_->toggled = !state_.rightPanelCollapsed;
    rightToggle_->onClick = [this] { toggleRightPanel(); };

    focusBtn_ = cmdBar_->emplace<Button>("", "maximise-view", ButtonStyle::Icon);
    focusBtn_->tooltip = "Focus mode  (F)";
    focusBtn_->onClick = [this] {
        setFocusMode(!state_.focusMode);
    };

    // Only a small icon, not an always-visible text pill. Focus mode can
    // remove it together with the bottom bar after an idle delay.
    focusExitBtn_ = emplace<Button>("", "minimise-view", ButtonStyle::Card);
    focusExitBtn_->tooltip = "Leave focus mode  (Esc)";
    focusExitBtn_->visible = false;
    focusExitBtn_->onClick = [this] { setFocusMode(false); };

    auto* wake = emplace<FocusWakeZone>();
    wake->visible = false;
    wake->setZOrder(zlayer::floating + 1);
    wake->onWake = [this] { showFocusControls(true); };
    focusWakeZone_ = wake;

    shareBtn_ = cmdBar_->emplace<Button>("Share", "share", ButtonStyle::Primary);
    shareBtn_->onClick = [this] { shareDocument(); };
}

void ReaderScreen::openDrawingMenu(Button* anchor) {
    if (!shapePop_ || !anchor) return;
    if (shapePop_->isOpen()) { shapePop_->close(now()); return; }
    openPopoverFor(shapePop_, anchor, false);
}

void ReaderScreen::chooseDrawingShape(PageStage::EditTool shape) {
    drawingShape_ = shape;
    if (shapePop_) shapePop_->close(now());
    selectTool(state_.mode == Mode::Edit ? Tool::Shapes : Tool::Pen);
    for (std::size_t i = 0; i < shapeItems_.size(); ++i) {
        static constexpr PageStage::EditTool shapes[] = {
            PageStage::EditTool::Ink, PageStage::EditTool::Box,
            PageStage::EditTool::Ellipse, PageStage::EditTool::Line,
            PageStage::EditTool::Arrow};
        shapeItems_[i]->toggled = shapes[i] == shape;
    }
}

void ReaderScreen::selectTool(Tool tool) {
    state_.tool = tool;
    const auto choices = toolsFor(state_.mode);
    for (std::size_t i = 0; i < toolBtns_.size(); ++i)
        toolBtns_[i]->toggled = choices[i] == tool;
    for (std::size_t i = 0; i < toolOverflowItems_.size(); ++i)
        toolOverflowItems_[i]->toggled = choices[i] == tool;
    if (annotHdr_) annotHdr_->text = toolName(tool);
    if (toolOverflowPop_) toolOverflowPop_->close(now());
    applyToolColour();
    syncTool();
    layoutTree();
}

void ReaderScreen::rebuildToolButtons(double now) {
    // A hovered tool may be about to be destroyed. The tooltip host keeps
    // the owner's pointer; release it before removing the button subtree.
    OverlayHost::current().dismissTooltip();
    if (shapePop_) {
        shapePop_->close(now);
        shapePop_->setOwner(nullptr);
    }
    toolBtns_.clear();
    toolHost_->removeAllChildren();
    if (toolOverflowPop_) {
        toolOverflowPop_->close(now);
        toolOverflowPop_->removeAllChildren();
        toolOverflowItems_.clear();
    }
    const auto choices = toolsFor(state_.mode);
    for (Tool tool : choices) {
        auto* b = toolHost_->emplace<Button>("", toolIcon(tool), ButtonStyle::Icon);
        b->tooltip = toolName(tool);
        b->toggled = tool == state_.tool;
        b->onClick = [this, tool, b] {
            if (tool == Tool::Pen || tool == Tool::Shapes) openDrawingMenu(b);
            else selectTool(tool);
        };
        toolBtns_.push_back(b);
        if (toolOverflowPop_) {
            auto* row = toolOverflowPop_->emplace<Button>(toolName(tool),
                                                          toolIcon(tool), ButtonStyle::ListItem);
            row->toggled = tool == state_.tool;
            row->onClick = [this, tool] {
                if (tool == Tool::Pen || tool == Tool::Shapes) {
                    toolOverflowPop_->close(this->now());
                    openDrawingMenu(toolOverflowBtn_);
                } else selectTool(tool);
            };
            toolOverflowItems_.push_back(row);
        }
    }
    if (toolOverflowPop_)
        toolOverflowPop_->setFrame(toolOverflowPop_->frame.x,
                                   toolOverflowPop_->frame.y, 182.f,
                                   12.f + 40.f * static_cast<float>(choices.size()));
}

void ReaderScreen::buildWorkArea() {
    // Panel captions are painted by the panels themselves so that they stay
    // inside the panel's subtree and below any popover.
    // (bodies assigned at the end of this function)

    // ---- left panel: Pages / Outline / Notes / Layers ---------------------
    leftPanel_ = emplace<SidePanel>(SidePanel::Side::Left);

    leftTabs_ = leftPanel_->emplace<SegmentedControl>(
        std::vector<SegmentedControl::Item>{
            {"Pages", "", ""}, {"Outline", "", ""}, {"Notes", "", ""},
            {"Layers", "", ""}},
        0);
    leftTabs_->onChange = [this](int i) {
        state_.leftTab = i;
        // Thumbnails and outline are separate children. Changing just the
        // model leaves the old child's visibility cached until a resize.
        layoutTree();
    };

    leftCollapse_ = leftPanel_->emplace<Button>("", "chevron-left", ButtonStyle::Icon);
    leftCollapse_->tooltip = "Hide panel";
    leftCollapse_->onClick = [this] { toggleLeftPanel(); };

    thumbs_ = leftPanel_->emplace<ThumbnailList>(state_);
    thumbs_->onPick = [this](int p) { stage_->goToPage(p, now()); };

    // The outline is a tab of the left panel in the prototype, not a
    // separate right-hand list.
    outline_ = leftPanel_->emplace<OutlineList>(state_);
    outline_->onPick = [this](int page) { stage_->goToPage(page, now()); };
    notes_ = leftPanel_->emplace<OutlineList>(state_);
    notes_->debugName = "NotesList";
    notes_->setHeader("Notes & highlights", "No indexed notes or highlights yet.");
    notes_->onPickEntry = [this](std::size_t i) {
        if (i >= noteIds_.size()) return;
        const auto id = noteIds_[i];
        if (!id) {
            const auto [page, index] = pdfNoteRefs_[i];
#if FOLIO_HAVE_PDFIUM_SOURCE
            auto found = std::find(pdfNotesRefs_.begin(), pdfNotesRefs_.end(),
                                   std::pair{page, index});
            if (found != pdfNotesRefs_.end()) {
                const auto position = static_cast<std::size_t>(found - pdfNotesRefs_.begin());
                if (position < pdfNotesInfo_.size() && pdfNotesInfo_[position].editable) {
                    const auto adopted = adoptSourceAnnotation(page, pdfNotesInfo_[position]);
                    if (adopted) {
                        stage_->goToPage(page + 1, now());
                        stage_->selectAnnot(adopted);
                        onAnnotPicked(adopted, true);
                        return;
                    }
                }
            }
#endif
            stage_->goToPage(page + 1, now());
            OverlayHost::current().showToast(
                "This PDF annotation subtype is preserved but not editable", now());
            return;
        }
        const auto* a = annots_.get(id);
        if (!a) return;
        stage_->goToPage(a->page + 1, now());
        stage_->selectAnnot(id);
        selectedAnnot_ = id;
        updateRightPanelContext();
        if (!annots_.kindVisible(a->kind)) {
            annots_.setKindVisible(a->kind, true);
            for (std::size_t j = 0; j < 12; ++j)
                layerItems_[j]->toggled = annots_.kindVisible(static_cast<AnnotKind>(j));
        }
        layoutTree();
    };
    layersScroll_ = leftPanel_->emplace<ScrollView>();
    layersContent_ = layersScroll_->emplace<Widget>();
    static constexpr const char* kLayerNames[] = {
        "Highlights", "Underlines", "Strikethroughs", "Ink", "Notes",
        "Boxes", "Ellipses", "Text", "Stamps", "Signatures", "Lines", "Arrows"};
    for (std::size_t i = 0; i < std::size(kLayerNames); ++i) {
        auto* row = layersContent_->emplace<Button>(kLayerNames[i], "", ButtonStyle::ListItem);
        row->tooltip = std::string("Show or hide Folio ") + kLayerNames[i];
        row->toggled = true;
        row->onClick = [this, row, i] {
            const auto kind = static_cast<AnnotKind>(i);
            row->toggled = !annots_.kindVisible(kind);
            annots_.setKindVisible(kind, row->toggled);
            if (!row->toggled && annots_.get(selectedAnnot_) &&
                annots_.get(selectedAnnot_)->kind == kind) {
                stage_->deselectAnnot();
                selectedAnnot_ = 0;
                updateRightPanelContext();
            }
        };
        layerItems_.push_back(row);
    }
    auto* pdfLayers = layersContent_->emplace<Button>(
        "PDF layers (read-only)", "layers", ButtonStyle::ListItem);
    pdfLayers->subtitle = "PDFium cannot list or switch source PDF layers";
    pdfLayers->enabled = false;
    layerItems_.push_back(pdfLayers);

    for (const char* lbl : {"Rotate", "Insert", "Extract", "Delete"}) {
        auto* b = leftPanel_->emplace<Button>(lbl, "", ButtonStyle::Ghost);
        b->compact = true;
        b->onClick = [this, lbl] { applyPageOp(lbl); };
        pageActions_.push_back(b);
    }

    // The rail that replaces the panel once it is collapsed.
    {
        auto* b = leftPanel_->emplace<Button>("", "chevron-right", ButtonStyle::Icon);
        b->tooltip = "Show panel";
        b->onClick = [this] { toggleLeftPanel(); };
        leftRail_.push_back(b);
        int idx = 0;
        for (auto [icon, tip] : {std::pair{"thumbnails", "Pages"}, {"outline", "Outline"},
                                 {"comment", "Notes"}, {"layers", "Layers"}}) {
            auto* r = leftPanel_->emplace<Button>("", icon, ButtonStyle::Icon);
            r->tooltip = tip;
            r->onClick = [this, idx] {
                state_.leftTab = idx;
                leftTabs_->setSelected(idx, now());
                toggleLeftPanel();
                layoutTree();
            };
            leftRail_.push_back(r);
            ++idx;
        }
    }

    // ---- stage ------------------------------------------------------------
    stage_ = emplace<PageStage>();
    stage_->setSelectionMagnifierEnabled(state_.selectionMagnifier);
    stage_->onPageChanged = [this](int p) {
        thumbs_->setCurrent(p, now());
        syncPageLabel();
    };
    stage_->onSelectionCommitted = [this](Point windowPos) {
        if (!selPop_) return;
        syncToolOptions();   // the command bar now targets the markup
        layoutTree();       // reserve property widths before the next frame
        if (selColorBtn_)
            selColorBtn_->swatchColor =
                kToolInks[static_cast<std::size_t>(
                    std::clamp(markupColorIdx_, 0, kInkCount - 1))];
        selPop_->windowSize = Size2{frame.w, frame.h};
        selPop_->open(windowPos, now());
    };
    stage_->onSelectionCleared = [this] { closeSelectionPopup(); };
    stage_->onCanvasTap = [this] { toggleFocusControlsFromPage(); };
    stage_->thumbInk = [](int i) {
        return kToolInks[static_cast<std::size_t>(std::clamp(i, 0, kInkCount - 1))];
    };
    stage_->onThumbChoice = [this](int tool, int option, int value) {
        // The radial palette uses the same tool, swatch and stroke state as
        // the ordinary toolbar; switching mode never turns Focus off.
        if (tool == 0) {
            if (state_.mode != Mode::Read) setMode(Mode::Read);
            selectTool(Tool::Hand);
            return;
        }
        if (state_.mode != Mode::Annotate) setMode(Mode::Annotate);
        if (tool == 1) selectTool(Tool::Highlight);
        else if (tool == 2) {
            // A second-layer colour/weight/opacity change must not silently
            // reset a previously chosen Arrow/Line/Shape to freehand. A new
            // Pen pick still intentionally starts with the freehand tip.
            if (option < 0 || (state_.tool != Tool::Pen &&
                               state_.tool != Tool::Shapes))
                chooseDrawingShape(PageStage::EditTool::Ink);
        } else if (tool == 3) selectTool(Tool::Eraser);
        else return;
        if (option == 0 && value >= 0 && value < kInkCount) {
            toolColor_[static_cast<std::size_t>(state_.tool)] = value;
            if (state_.tool == Tool::Highlight) markupColorIdx_ = value;
            applyToolColour();
            syncTool();
        } else if (option == 1 && value >= 0 && value < kWeightCount) {
            if (annotWidthSeg_) annotWidthSeg_->setSelected(value, now());
            syncTool();
        } else if (option == 2 && value >= 0 && value < 4) {
            const float alpha = static_cast<float>(value + 1) * .25f;
            if (annotOpacity_) annotOpacity_->setValue(alpha);
            if (state_.tool == Tool::Highlight) markupOpacity_ = alpha;
            syncTool();
        } else if (option == 3 && tool == 2 && value >= 0 && value < 5) {
            static constexpr PageStage::EditTool shapes[] = {
                PageStage::EditTool::Ink, PageStage::EditTool::Box,
                PageStage::EditTool::Ellipse, PageStage::EditTool::Line,
                PageStage::EditTool::Arrow};
            chooseDrawingShape(shapes[value]);
        }
    };
    stage_->onAnnotSelectionChanged = [this](std::uint64_t id, bool fromTouch) {
        onAnnotPicked(id, fromTouch);
    };
    stage_->onPageActivated = [this](int page) {
        // "Open" the tile: single page, fitted, on the one that was hit.
        // setLayoutMode keeps the pill label and the panel's LAYOUT
        // control in step, so the UI does not end up disagreeing with
        // itself about which layout is active.
        setLayoutMode(0);
        stage_->goToPage(page + 1, now());
        stage_->setZoomMode(ZoomMode::FitPage, now());
        syncPageLabel();
    };

    // ---- right panel: Display ---------------------------------------------
    rightPanel_ = emplace<SidePanel>(SidePanel::Side::Right);
    rightPanel_->clipsContent = true;
    inspectorScroll_ = rightPanel_->emplace<InspectorScrollView>();
    inspectorScroll_->showScrollbar = true;
    inspectorContent_ = inspectorScroll_->emplace<InspectorContent>();

    rightCollapse_ = rightPanel_->emplace<Button>("", "chevron-right", ButtonStyle::Icon);
    rightCollapse_->tooltip = "Hide panel";
    rightCollapse_->onClick = [this] { toggleRightPanel(); };

    deleteAnnotBtn_ = rightPanel_->emplace<Button>("", "trash", ButtonStyle::Icon);
    deleteAnnotBtn_->tooltip = "Delete selected annotation";
    deleteAnnotBtn_->visible = false;
    deleteAnnotBtn_->onClick = [this] {
        if (stage_->deleteSelectedAnnot())
            OverlayHost::current().showToast("Annotation deleted · Undo available", now());
    };

    // Pinned above the scrolling inspector so Focus can be entered from
    // Read, Annotate or Edit, even when tool options fill a long panel.
    focusPanelBtn_ = rightPanel_->emplace<Button>("", "maximise-view",
                                                  ButtonStyle::Icon);
    focusPanelBtn_->tooltip = "Focus mode (right panel)";
    focusPanelBtn_->onClick = [this] { setFocusMode(!state_.focusMode); };
    undoPanelBtn_ = rightPanel_->emplace<Button>("", "undo", ButtonStyle::Icon);
    undoPanelBtn_->tooltip = "Undo  (Ctrl+Z)";
    undoPanelBtn_->onClick = [this] { doUndo(); };
    redoPanelBtn_ = rightPanel_->emplace<Button>("", "redo", ButtonStyle::Icon);
    redoPanelBtn_->tooltip = "Redo  (Ctrl+Shift+Z)";
    redoPanelBtn_->onClick = [this] { doRedo(); };
    rotateLeftBtn_ = rightPanel_->emplace<Button>("", "view-rotate-ccw", ButtonStyle::Icon);
    rotateLeftBtn_->tooltip = "Rotate view left 90° (does not alter PDF)";
    rotateLeftBtn_->onClick = [this] { stage_->rotate(-1, now()); };
    rotateRightBtn_ = rightPanel_->emplace<Button>("", "view-rotate-cw", ButtonStyle::Icon);
    rotateRightBtn_->tooltip = "Rotate view right 90° (does not alter PDF)";
    rotateRightBtn_->onClick = [this] { stage_->rotate(1, now()); };
    focusAutoHideBtn_ = inspectorContent_->emplace<Button>(
        "Auto-hide focus controls", "", ButtonStyle::ListItem);
    focusAutoHideBtn_->tooltip = "Hide focus controls after 3 seconds";
    focusAutoHideBtn_->subtitle = "Tap top corner to reveal · Back exits";
    focusAutoHideBtn_->toggled = state_.autoHideFocusControls;
    focusAutoHideBtn_->onClick = [this] {
        state_.autoHideFocusControls = !state_.autoHideFocusControls;
        focusAutoHideBtn_->toggled = state_.autoHideFocusControls;
        state_.saveRecents();
        if (state_.focusMode && !state_.autoHideFocusControls)
            showFocusControls(true);
        else if (state_.focusMode && focusControlsVisible_)
            focusHideAt_ = now() + kFocusIdleSeconds;
    };

    selectionMagnifierBtn_ = inspectorContent_->emplace<Button>(
        "Selection & highlighter magnifier", "", ButtonStyle::ListItem);
    selectionMagnifierBtn_->tooltip = "Magnify text selection and annotation highlighter";
    selectionMagnifierBtn_->subtitle = "Select text or drag the Highlight tool";
    selectionMagnifierBtn_->toggled = state_.selectionMagnifier;
    selectionMagnifierBtn_->onClick = [this] {
        state_.selectionMagnifier = !state_.selectionMagnifier;
        selectionMagnifierBtn_->toggled = state_.selectionMagnifier;
        stage_->setSelectionMagnifierEnabled(state_.selectionMagnifier);
        state_.saveRecents();
    };

    // Page-theme swatches. These are colour chips, not icons, so they are
    // painted by the panel overlay; the buttons exist for hit-testing and
    // the selection ring.
    for (int i = 0; i < 3; ++i) {
        auto* b = inspectorContent_->emplace<Button>("", "", ButtonStyle::Ghost);
        b->tooltip = (i == 0) ? "Light page" : (i == 1 ? "Sepia page" : "Dark page");
        b->onClick = [this, i] {
            applyPageTheme(static_cast<PageTheme>(i));
        };
        b->toggled = (i == 0);
        pageThemeSw_.push_back(b);
    }

    // ---- right panel: annotation tool properties -------------------------
    // Shown instead of the page-display controls while an annotation tool is
    // active, because that is what the panel is for: the options that apply
    // to what you are doing right now.
    annotHdr_ = rightPanel_->emplace<Label>("TOOL", tokens::type_sectionHeader,
                                            TextRole::Muted);
    {
        // The whole palette, not a sample of it. Six was "a very minimum
        // colour choice"; these are the same 24 the popover shows, in the
        // same order, so the two never disagree about what index 11 is.
        for (int i = 0; i < kInkCount; ++i) {
            auto* b = inspectorContent_->emplace<Button>("", "", ButtonStyle::Ghost);
            b->tooltip = "Ink colour";
            b->toggled = (i == 0);
            // Routed through applyOptColor so the panel and the command
            // bar do the same thing: recolour the selected annotation if
            // there is one, otherwise set the tool's ink.
            b->onClick = [this, i] { applyOptColor(i); };
            annotColorSw_.push_back(b);
        }
    }
    commentLabel_ = inspectorContent_->emplace<Label>(
        "COMMENT", tokens::type_sectionHeader, TextRole::Muted);
    commentField_ = inspectorContent_->emplace<TextField>("Write a note or comment…");
    commentField_->multiline = true;
    commentField_->onChanged = [this](const std::string& value) {
        if (!commentTarget_ || value.size() > 131072) return;
        for (std::uint64_t id : annots_.groupMembers(commentTarget_)) {
            if (const Annotation* a = annots_.get(id)) {
                Annotation changed = *a;
                changed.comment = value;
                annots_.replace(id, changed);
            }
        }
        // Save to private Folio storage as it is composed: Android can
        // suspend the app without ever sending a blur or "Done" event.
        persistAnnotations();
        const float targetH = std::clamp(
            108.f + static_cast<float>(value.size() / 42) * 21.f,
            108.f, 218.f);
        if (std::fabs(targetH - commentField_->frame.h) > .5f)
            layoutTree();
    };
    commentField_->onFocus = [this](bool gained) {
        if (!gained) commitComment();
    };
    saveCommentBtn_ = inspectorContent_->emplace<Button>(
        "Save comment", "check", ButtonStyle::ListItem);
    saveCommentBtn_->tooltip = "Save annotation comment";
    saveCommentBtn_->onClick = [this] { commitComment(); };
    // Six weights drawn at their real proportions, not three adjectives.
    // The display thickness is the page-unit weight on a gentle curve -
    // a literal mapping would make 10.f dwarf the row.
    annotWidthSeg_ = inspectorContent_->emplace<WidthStrip>(
        std::vector<float>{1.f, 2.f, 3.5f, 5.f, 7.5f, 11.f}, 2);
    annotWidthSeg_->onChange = [this](int i) {
        if (selectedAnnot_) applyOptWidth(i); else syncTool();
    };

    textFontSeg_ = inspectorContent_->emplace<SegmentedControl>(
        std::vector<SegmentedControl::Item>{
            {"Sans", "", ""}, {"Serif", "", ""}, {"Mono", "", ""}},
        0);
    textFontSeg_->onChange = [this](int) { syncTextStyle(); };

    // Point sizes rather than S/M/L: this is a font picker, and everyone
    // already knows what 16 pt looks like.
    textSizeSeg_ = inspectorContent_->emplace<SegmentedControl>(
        std::vector<SegmentedControl::Item>{
            {"12", "", ""}, {"16", "", ""}, {"22", "", ""}, {"30", "", ""}},
        1);
    textSizeSeg_->onChange = [this](int) { syncTextStyle(); };

    annotOpacity_ = inspectorContent_->emplace<Slider>(0.15f, 1.f, 1.f);
    annotOpacity_->onChange = [this](float v) {
        if (selectedAnnot_) applyOptOpacity(v); else syncTool();
    };

    layoutSegR_ = inspectorContent_->emplace<SegmentedControl>(
        std::vector<SegmentedControl::Item>{
            {"Scroll", "", ""}, {"Single", "", ""}, {"Two-up", "", ""}},
        0);
    layoutSegR_->onChange = [this](int i) {
        // Prototype order is Scroll / Single / Two-up.
        static constexpr LayoutMode kModes[] = {LayoutMode::Continuous,
                                                LayoutMode::Single,
                                                LayoutMode::TwoUp};
        stage_->setLayout(kModes[static_cast<std::size_t>(i)], now());
    };


    {
        auto* b = rightPanel_->emplace<Button>("", "chevron-left", ButtonStyle::Icon);
        b->tooltip = "Show panel";
        b->onClick = [this] { toggleRightPanel(); };
        rightRail_.push_back(b);
        auto* p2 = rightPanel_->emplace<Button>("", "settings", ButtonStyle::Icon);
        p2->tooltip = "Show properties";
        p2->onClick = [this] { toggleRightPanel(); };
        rightRail_.push_back(p2);
    }
}


void ReaderScreen::syncTextStyle() {
    if (!stage_) return;
    // Sizes are stored as a fraction of page height so they survive zoom
    // and export; 792 pt is the nominal page these numbers are quoted at.
    static constexpr float kPt[4] = {12.f, 16.f, 22.f, 30.f};
    const int si = textSizeSeg_ ? std::clamp(textSizeSeg_->selected(), 0, 3) : 1;
    const int fi = textFontSeg_ ? std::clamp(textFontSeg_->selected(), 0, 2) : 0;
    using FF = folio::render::FontFamily;
    static constexpr FF kFam[3] = {FF::Sans, FF::Serif, FF::Mono};
    stage_->setTextStyle(kPt[si] / 792.f, kFam[fi]);
}

void ReaderScreen::applyToolColour() {
    // Index 0 is highlighter yellow and index 5 is near-black ink. Every
    // tool that draws a mark the user is meant to read starts on the ink;
    // only the highlighter starts on yellow. Whatever they pick after that
    // is remembered for that tool alone.
    annotColorIdx_ = toolColor_[static_cast<std::size_t>(state_.tool)];
    for (std::size_t k = 0; k < annotColorSw_.size(); ++k)
        annotColorSw_[k]->toggled = (static_cast<int>(k) == annotColorIdx_);
}

void ReaderScreen::syncTool() {
    using ET = PageStage::EditTool;
    ET et = ET::None;
    switch (state_.tool) {
        case Tool::Highlight: et = ET::Highlight; break;
        case Tool::Pen:       et = drawingShape_; break;
        case Tool::Note:      et = ET::Note;      break;
        case Tool::Eraser:    et = ET::Eraser;    break;
        case Tool::Text:      et = ET::Text;      break;
        case Tool::Shapes:    et = drawingShape_; break;
        case Tool::Stamp:     et = ET::Stamp;     break;
        case Tool::Signature: et = ET::Signature; break;
        case Tool::Select:
        case Tool::Hand:      et = ET::None;      break;
    }
    // Changing tool finishes whatever was being typed rather than leaving
    // an invisible editor armed under the new tool.
    stage_->commitText();
    stage_->setEditTool(et);
    if (state_.tool == Tool::Text) syncTextStyle();

    static constexpr Color kInks[6] = {
        {250, 204, 21, 255},  {244, 114, 182, 255}, {74, 222, 128, 255},
        {96, 165, 250, 255},  {248, 113, 113, 255}, {30, 32, 44, 255}};
    const int wi = annotWidthSeg_ ? annotWidthSeg_->selected() : 1;
    stage_->setAnnotStyle(kInks[static_cast<std::size_t>(annotColorIdx_)],
                          kToolWidths[static_cast<std::size_t>(std::clamp(wi, 0, kWeightCount - 1))],
                          annotOpacity_ ? annotOpacity_->value() : 1.f);
    updateRightPanelContext();
    syncToolOptions();
}

bool ReaderScreen::stylingActive() const {
    if (stage_ && stage_->editTool() != PageStage::EditTool::None) return true;
    return selectedAnnot_ != 0 && annots_.get(selectedAnnot_) != nullptr;
}

bool ReaderScreen::stylingText() const {
    if (const Annotation* a =
            selectedAnnot_ ? annots_.get(selectedAnnot_) : nullptr)
        return a->kind == AnnotKind::Text;
    return stage_ && stage_->editTool() != PageStage::EditTool::None &&
           state_.tool == Tool::Text;
}

void ReaderScreen::updateRightPanelContext() {
    const bool authoring = stage_ && stage_->editTool() != PageStage::EditTool::None;
    // A selected annotation puts the panel into the same mode as authoring:
    // the controls are the same, they just act on an existing object. This
    // is how you change the colour of something after drawing it.
    const Annotation* sel = selectedAnnot_ ? annots_.get(selectedAnnot_) : nullptr;
    const bool styling = authoring || sel != nullptr;

    if (sel) {
        static constexpr const char* kKindNames[] = {
            "HIGHLIGHT", "UNDERLINE", "STRIKETHROUGH", "DRAWING", "NOTE",
            "RECTANGLE", "ELLIPSE", "TEXT", "STAMP", "SIGNATURE",
            "LINE", "ARROW"};
        const auto k = static_cast<std::size_t>(sel->kind);
        annotHdr_->text = k < std::size(kKindNames) ? kKindNames[k]
                                                    : "ANNOTATION";
    } else {
        // Authoring: the header names the tool, which is what it said
        // before a selected annotation could take the panel over.
        annotHdr_->text = toolName(state_.tool);
    }

    annotHdr_->visible = styling;
    for (Button* b : annotColorSw_) b->visible = styling;
    annotWidthSeg_->visible = styling;
    annotOpacity_->visible = styling;
    for (Button* b : pageThemeSw_) b->visible = !styling;

    // Reflect the selected annotation's own values, so the panel is not
    // showing the tool's settings next to a selected object that has
    // different ones.
    if (sel) {
        const bool wasSyncing = syncingOpts_;
        syncingOpts_ = true;
        const int ink = nearestInk(sel->color);
        for (std::size_t k = 0; k < annotColorSw_.size(); ++k)
            annotColorSw_[k]->toggled = (static_cast<int>(k) == ink);
        if (annotOpacity_) annotOpacity_->setValue(sel->opacity);
        if (annotWidthSeg_) {
                    int best = 1; float bd = 1e9f;
            for (int k = 0; k < kWeightCount; ++k) {
                const float d = std::fabs(kToolWidths[k] - sel->width);
                if (d < bd) { bd = d; best = k; }
            }
            annotWidthSeg_->setSelected(best, now());
        }
        syncingOpts_ = wasSyncing;
    }
    layoutTree();
}

void ReaderScreen::installPanelPainters() {
    leftPanel_->drawOverlay = [this](IPaintTarget& t, const PaintCtx& ctx) {
        const float lBody = leftPanel_->contentAlpha(ctx.now);
        if (lBody <= 0.01f || !leftTabs_->visible) return;
        t.drawTextInBox(std::to_string(pageCount_) + " pages",
                        Rect{14.f, 60.f, 120.f, 28.f},
                        styleFor(tokens::type_small, fade(ctx.pal.muted, lBody)),
                        folio::render::TextAlign::Left);
    };

    // The scrolling content already begins with PAGE THEME. A second DISPLAY
    // caption in the parent covered it at the same y (including on phones).
    rightPanel_->drawOverlay = {};

    static_cast<InspectorContent*>(inspectorContent_)->drawOverlay =
        [this](IPaintTarget& t, const PaintCtx& ctx) {
        const float rBody = rightPanel_->contentAlpha(ctx.now);
        if (rBody <= 0.01f || !rightCollapse_->visible) return;
        const float w = inspectorContent_->frame.w;
        const auto head = [&](const char* s2, float y) {
            t.drawTextInBox(s2, Rect{14.f, y, w - 28.f, 16.f},
                            styleFor(tokens::type_sectionHeader,
                                     fade(ctx.pal.muted, rBody)),
                            folio::render::TextAlign::Left);
        };
        const bool authoring = stylingActive();

        if (authoring) {
            // The TOOL header is a real Label (it carries the tool name);
            // the rest are plain section captions.
            head("COLOUR", annotColorSw_.front()->frame.y - 22.f);
            if (textFontSeg_->visible) {
                head("FONT", textFontSeg_->frame.y - 22.f);
                head("SIZE", textSizeSeg_->frame.y - 22.f);
            } else {
                head("WIDTH", annotWidthSeg_->frame.y - 22.f);
            }
            head("OPACITY", annotOpacity_->frame.y - 22.f);

            for (std::size_t i = 0; i < annotColorSw_.size(); ++i) {
                const Rect r = annotColorSw_[i]->frame;
                const float rad = std::min(r.w, r.h) * 0.5f;
                t.fillRoundedRect(r, Corners::all(rad),
                                  fade(kToolInks[i], rBody));
                // White and the pale tints vanish on a light panel without
                // a hairline, and the whole point of this row is that you
                // can see what you are picking.
                t.strokeRoundedRect(r, Corners::all(rad),
                                    fade(Color{0, 0, 0, 36}, rBody), 1.f);
                if (annotColorSw_[i]->toggled) {
                    t.strokeRoundedRect(r.inset(-2.f), Corners::all(rad + 2.f),
                                        fade(ctx.pal.accent, rBody), 2.f);
                }
            }
        } else {
            head("PAGE THEME", 2.f);

            static constexpr Color kChip[3] = {{0xFE, 0xFE, 0xFE, 0xFF},
                                               {0xF1, 0xE4, 0xC8, 0xFF},
                                               {0x22, 0x25, 0x2E, 0xFF}};
            for (std::size_t i = 0; i < pageThemeSw_.size(); ++i) {
                const Rect r = pageThemeSw_[i]->frame;
                t.fillRoundedRect(r, Corners::all(12.f), fade(kChip[i], rBody));
                const bool on = pageThemeSw_[i]->toggled;
                t.strokeRoundedRect(r.inset(-1.5f), Corners::all(13.5f),
                                    fade(on ? ctx.pal.accent : ctx.pal.border, rBody),
                                    on ? 2.f : 1.f);
            }
        }

        head("LAYOUT", layoutSegR_->frame.y - 22.f);
        if (!authoring) {
            const float capY = layoutSegR_->frame.y + 46.f;
            t.drawTextInBox("Page colors apply to the document only.",
                            Rect{14.f, capY, w - 28.f, 18.f},
                            styleFor(tokens::type_small, fade(ctx.pal.muted, rBody)),
                            folio::render::TextAlign::Left);
            t.drawTextInBox("The app theme is separate.",
                            Rect{14.f, capY + 17.f, w - 28.f, 18.f},
                            styleFor(tokens::type_small, fade(ctx.pal.muted, rBody)),
                            folio::render::TextAlign::Left);
        }
    };
}

void ReaderScreen::setMode(Mode m) {
    state_.mode = m;
    modeBtn_->label = modeName(m);
    modeBtn_->icon = modeIcon(m);
    for (std::size_t i = 0; i < modeItems_.size(); ++i)
        modeItems_[i]->toggled = (static_cast<int>(m) == static_cast<int>(i));
    modePop_->close(now());
    modeBtn_->chevronOpen = false;
    // Restore a valid tool BEFORE building its toggled buttons/menu.
    const auto tools = toolsFor(m);
    if (std::find(tools.begin(), tools.end(), state_.tool) == tools.end())
        state_.tool = tools.front();
    rebuildToolButtons(now());
    if (annotHdr_) annotHdr_->text = toolName(state_.tool);
    applyToolColour();
    syncTool();
    layoutTree();
}

void ReaderScreen::toggleLeftPanel() {
    state_.leftPanelCollapsed = !state_.leftPanelCollapsed;
    leftPanel_->setCollapsed(state_.leftPanelCollapsed, now());
    leftToggle_->toggled = !state_.leftPanelCollapsed;
    panelLayoutDirty_ = true;
}

void ReaderScreen::toggleRightPanel() {
    state_.rightPanelCollapsed = !state_.rightPanelCollapsed;
    rightPanel_->setCollapsed(state_.rightPanelCollapsed, now());
    rightToggle_->toggled = !state_.rightPanelCollapsed;
    panelLayoutDirty_ = true;
}

void ReaderScreen::buildPill() {
    pill_ = emplace<CardView>(tokens::radius::pill);

    pageField_ = pill_->emplace<TextField>();
    pageField_->flat = true;
    pageField_->align = folio::render::TextAlign::Centre;
    pageField_->onFocus = [this](bool gained) {
        if (gained) {
            // Show only the number being replaced; "3 / 24" is a label,
            // not something anyone wants to edit around.
            pageField_->text = std::to_string(stage_->currentPage());
            pageField_->setCaretToEnd();
        } else {
            commitPageField();
        }
    };
    pageField_->onSubmit = [this] { commitPageField(); };

    prevBtn_ = pill_->emplace<Button>("", "chevron-up", ButtonStyle::Icon);
    prevBtn_->tooltip = "Previous page";
    prevBtn_->onClick = [this] { stage_->prevPage(0.0); };

    nextBtn_ = pill_->emplace<Button>("", "chevron-down", ButtonStyle::Icon);
    nextBtn_->tooltip = "Next page";
    nextBtn_->onClick = [this] { stage_->nextPage(0.0); };

    zoomOutBtn_ = pill_->emplace<Button>("", "zoom-out", ButtonStyle::Icon);
    zoomOutBtn_->tooltip = "Zoom out";
    // Button zoom has no pointer to follow, so it holds the viewport
    // centre - otherwise it would reuse whatever the last ctrl+wheel set.
    zoomOutBtn_->onClick = [this] {
        stage_->clearZoomFocus();
        stage_->setZoom(stage_->zoom() / 1.25f, now());
    };

    zoomInBtn_ = pill_->emplace<Button>("", "zoom-in", ButtonStyle::Icon);
    zoomInBtn_->tooltip = "Zoom in";
    zoomInBtn_->onClick = [this] {
        stage_->clearZoomFocus();
        stage_->setZoom(stage_->zoom() * 1.25f, now());
    };

    // The percentage is painted in onPaintOverlay, but its own pill child
    // handles taps. Never take space from the numeric label on small phones.
    zoomPctBtn_ = pill_->emplace<Button>("", "", ButtonStyle::Ghost);
    zoomPctBtn_->tooltip = "Zoom presets, fit width or fit page";
    zoomPctBtn_->onClick = [this] {
        if (zoomPop_->isOpen()) zoomPop_->close(now());
        else openPopoverFor(zoomPop_, zoomPctBtn_, true);
    };

    fitWidthBtn_ = pill_->emplace<Button>("", "fit-width", ButtonStyle::Icon);
    fitWidthBtn_->tooltip = "Fit width";
    fitWidthBtn_->onClick = [this] { stage_->setZoomMode(ZoomMode::FitWidth, now()); };

    fitPageBtn_ = pill_->emplace<Button>("", "fit-page", ButtonStyle::Icon);
    fitPageBtn_->tooltip = "Fit page";
    fitPageBtn_->onClick = [this] { stage_->setZoomMode(ZoomMode::FitPage, now()); };

    // One button showing the current layout, opening a menu - four icon
    // segments side by side gave no hint what any of them did.
    viewBtn_ = pill_->emplace<Button>("Continuous", "continuous", ButtonStyle::Ghost);
    viewBtn_->tooltip = "Page layout";
    viewBtn_->trailingChevron = true;
    viewBtn_->compact = true;
    viewBtn_->onClick = [this] {
        if (layoutPop_->isOpen()) { layoutPop_->close(now()); return; }
        // Opens upward: the pill is already at the bottom of the window.
        // This used to pre-offset the anchor up by the popup's own height
        // and open "below" that invented point. It landed in the right
        // place, but Popover could no longer tell it had flipped, so the
        // circular reveal grew from the top edge - the far side from the
        // button the user just clicked. preferAbove lets the popover do
        // the flip itself, and the reveal starts at the click.
        openPopoverFor(layoutPop_, viewBtn_, true);
        viewBtn_->chevronOpen = true;
    };
}

void ReaderScreen::setLayoutMode(int index) {
    static constexpr LayoutMode kModes[] = {LayoutMode::Single, LayoutMode::Continuous,
                                            LayoutMode::TwoUp, LayoutMode::Grid};
    static constexpr const char* kNames[] = {"Single page", "Continuous", "Two up",
                                             "All together"};
    static constexpr const char* kIcons[] = {"single-page", "continuous", "two-up",
                                             "grid"};
    const auto i = static_cast<std::size_t>(std::clamp(index, 0, 3));
    stage_->setLayout(kModes[i], now());
    viewLabelFull_ = kNames[i];
    viewBtn_->label = kNames[i];
    viewBtn_->icon = kIcons[i];
    for (std::size_t k = 0; k < layoutItems_.size(); ++k)
        layoutItems_[k]->toggled = (k == i);
    if (layoutPop_->isOpen()) layoutPop_->close(now());
    viewBtn_->chevronOpen = false;
    // The right panel's LAYOUT control is the same setting in another place.
    if (layoutSegR_ && i < 3) {
        static constexpr int kToR[] = {1, 0, 2};
        layoutSegR_->setSelected(kToR[i], now());
    }
}


// ---------------------------------------------------------------------------
// Tool options
//
// One row of icons at the end of the command bar carrying every option the
// current tool has. The same controls also edit a selected annotation and
// the markup about to be applied to selected text - see OptTarget.
// ---------------------------------------------------------------------------


void ReaderScreen::closeOtherPopovers(const Popover* keep) {
    for (Popover* p : {toolColorPop_, toolSizePop_, toolOpacityPop_,
                       toolFontPop_, modePop_, findPop_, layoutPop_, zoomPop_,
                       displayPop_, selPop_, toolOverflowPop_, shapePop_}) {
        // The selection palette and its colour picker form one interaction:
        // opening a colour must not discard the selected-text actions.
        if (p == selPop_ && keep == toolColorPop_ &&
            optTarget_ == OptTarget::Markup && stage_->hasSelection()) continue;
        if (p && p != keep && p->isOpen()) p->close(now());
    }
    if (modePop_ != keep && modeBtn_) modeBtn_->chevronOpen = false;
    if (layoutPop_ != keep && viewBtn_) viewBtn_->chevronOpen = false;
}

void ReaderScreen::openPopoverFor(Popover* pop, Widget* trigger, bool above) {
    if (!pop || !trigger) return;
    closeOtherPopovers(pop);
    // Without this the clamp in Popover::open() measures against the
    // default 1440x900, so on any other window size the menu is positioned
    // for a window that is not there.
    pop->windowSize = Size2{frame.w, frame.h};
    pop->preferAbove = above;
    // Anchor on the trigger's centre. Anchoring on its left edge (local
    // x = 0) centres the popover half its own width further left, which is
    // how the mode menu ended up hanging past the left panel.
    const Point a = trigger->toWindow(
        Point{trigger->frame.w * 0.5f,
              above ? 0.f : trigger->frame.h + 6.f});
    pop->open(a, now());
}

void ReaderScreen::openOptPopover(Popover* pop, Button* anchor) {
    if (!pop || !anchor) return;
    if (pop->isOpen()) { pop->close(now()); return; }
    openPopoverFor(pop, anchor, false);
}

Annotation* ReaderScreen::selectedAnnotation() {
    return selectedAnnot_ ? annots_.get(selectedAnnot_) : nullptr;
}

// --- applying an option ----------------------------------------------------
// Each of these writes to whatever optTarget_ points at. Editing an existing
// annotation goes through ModifyAnnotCommand so it lands on the undo stack
// like every other change to the document.

bool ReaderScreen::modifySelectedGroup(
    const std::function<void(Annotation&)>& mutate, const char* label) {
    if (selectedAnnot_ == 0) return false;
    const std::vector<std::uint64_t> ids = annots_.groupMembers(selectedAnnot_);
    if (ids.empty()) return false;
    std::vector<Annotation> before, after;
    before.reserve(ids.size());
    after.reserve(ids.size());
    for (std::uint64_t id : ids) {
        const Annotation* a = annots_.get(id);
        if (!a) continue;
        before.push_back(*a);
        Annotation mod = *a;
        mutate(mod);
        annots_.replace(mod.id, mod);
        after.push_back(mod);
    }
    if (before.empty()) return false;
    persistAnnotations();
    edits_.push(std::make_unique<ModifyManyCommand>(annots_, std::move(before),
                                                    std::move(after), label));
    syncUndoButtons();
    return true;
}

void ReaderScreen::applyOptColor(int inkIndex) {
    if (const char* d = std::getenv("FOLIO_DEBUG_OPT"); d && *d == '1') {
        std::fprintf(stderr,
                     "[opt] color ink=%d target=%d syncing=%d selAnnot=%llu\n",
                     inkIndex, static_cast<int>(optTarget_),
                     static_cast<int>(syncingOpts_),
                     static_cast<unsigned long long>(selectedAnnot_));
    }
    if (syncingOpts_) return;
    const auto i = static_cast<std::size_t>(std::clamp(inkIndex, 0, kInkCount - 1));
    switch (optTarget_) {
        case OptTarget::Annotation: {
            if (!modifySelectedGroup(
                    [i](Annotation& x) { x.color = kToolInks[i]; }, "recolour"))
                return;
            // The panel's swatches show which ink the *annotation* has, so
            // they have to be refreshed from it - otherwise the old one
            // keeps its ring and two look selected at once.
            updateRightPanelContext();
            break;
        }
        case OptTarget::Markup:
            // The Highlight tool and text markup share one ink on purpose.
            markupColorIdx_ = inkIndex;
            toolColor_[static_cast<std::size_t>(Tool::Highlight)] = inkIndex;
            break;
        case OptTarget::Tool:
            annotColorIdx_ = inkIndex;
            toolColor_[static_cast<std::size_t>(state_.tool)] = inkIndex;
            if (state_.tool == Tool::Highlight) markupColorIdx_ = inkIndex;
            for (std::size_t k = 0; k < annotColorSw_.size(); ++k)
                annotColorSw_[k]->toggled = (static_cast<int>(k) == inkIndex);
            syncTool();
            break;
    }
    syncToolOptions();
}

void ReaderScreen::applyOptWidth(int widthIndex) {
    if (syncingOpts_) return;
    // Stroke weights now go up to six; text sizes are still four, and the
    // two ladders are clamped separately because they are different units.
    const auto i = static_cast<std::size_t>(
        std::clamp(widthIndex, 0, kWeightCount - 1));
    if (optTarget_ == OptTarget::Annotation) {
        Annotation* a = selectedAnnotation();
        if (!a) return;
        const Annotation before = *a;
        const int wi = std::clamp(widthIndex, 0, 3);
        modifySelectedGroup(
            [i, wi](Annotation& x) {
                if (x.kind == AnnotKind::Text) {
                    static constexpr float kPts[4] = {12.f, 16.f, 22.f, 30.f};
                    x.fontSize = kPts[static_cast<std::size_t>(wi)] / 792.f;
                } else {
                    x.width = kToolWidths[i];
                }
            },
            "resize");
    } else if (state_.tool == Tool::Text) {
        if (textSizeSeg_) textSizeSeg_->setSelected(widthIndex, now());
        syncTextStyle();
    } else {
        if (annotWidthSeg_) annotWidthSeg_->setSelected(widthIndex, now());
        syncTool();
    }
    syncToolOptions();
}

void ReaderScreen::applyOptOpacity(float v) {
    if (syncingOpts_) return;
    if (optTarget_ == OptTarget::Annotation) {
        Annotation* a = selectedAnnotation();
        if (!a) return;
        modifySelectedGroup([v](Annotation& x) { x.opacity = v; }, "opacity");
    } else if (optTarget_ == OptTarget::Markup) {
        markupOpacity_ = v;
    } else {
        if (state_.tool == Tool::Highlight) markupOpacity_ = v;
        if (annotOpacity_) annotOpacity_->setValue(v);
        syncTool();
    }
    syncToolOptions();
}

void ReaderScreen::applyOptFont(int familyIndex) {
    if (syncingOpts_) return;
    static constexpr folio::render::FontFamily kFams[3] = {
        folio::render::FontFamily::Sans, folio::render::FontFamily::Serif,
        folio::render::FontFamily::Mono};
    const auto i = static_cast<std::size_t>(std::clamp(familyIndex, 0, 2));
    if (optTarget_ == OptTarget::Annotation) {
        Annotation* a = selectedAnnotation();
        if (!a) return;
        modifySelectedGroup([i](Annotation& x) { x.font = kFams[i]; }, "font");
    } else {
        if (textFontSeg_) textFontSeg_->setSelected(familyIndex, now());
        syncTextStyle();
    }
    syncToolOptions();
}

void ReaderScreen::buildToolOptions() {
    // --- the four controls -------------------------------------------------
    toolOptColor_ = cmdBar_->emplace<Button>("", "", ButtonStyle::Icon);
    toolOptColor_->isSwatch = true;
    toolOptColor_->swatchColor = kToolInks[0];
    toolOptColor_->tooltip = "Colour";
    toolOptColor_->visible = false;
    toolOptColor_->onClick = [this] { openOptPopover(toolColorPop_, toolOptColor_); };

    toolOptSize_ = cmdBar_->emplace<Button>("", "stroke-weight", ButtonStyle::Icon);
    toolOptSize_->tooltip = "Thickness";
    toolOptSize_->visible = false;
    toolOptSize_->onClick = [this] { openOptPopover(toolSizePop_, toolOptSize_); };

    toolOptOpacity_ = cmdBar_->emplace<Button>("", "opacity", ButtonStyle::Icon);
    toolOptOpacity_->tooltip = "Opacity";
    toolOptOpacity_->visible = false;
    toolOptOpacity_->onClick = [this] {
        openOptPopover(toolOpacityPop_, toolOptOpacity_);
    };

    toolOptFont_ = cmdBar_->emplace<Button>("", "type", ButtonStyle::Icon);
    toolOptFont_->tooltip = "Font";
    toolOptFont_->visible = false;
    toolOptFont_->onClick = [this] { openOptPopover(toolFontPop_, toolOptFont_); };

    // --- colour: 6x2 grid --------------------------------------------------
    // The whole palette. This used to be six base inks plus six tints
    // generated on the fly, which meant the popover could offer colours
    // the panel could not and indices that did not survive a round trip.
    toolColorPop_ = emplace<Popover>();
    toolColorPop_->setOwner(toolOptColor_);
    toolColorPop_->setFrame(0.f, 0.f, 212.f, 96.f);
    for (int i = 0; i < kInkCount; ++i) {
        auto* b = toolColorPop_->emplace<Button>("", "", ButtonStyle::Ghost);
        b->isSwatch = true;
        b->compact = true;
        b->swatchColor = kToolInks[static_cast<std::size_t>(i)];
        b->onClick = [this, i] {
            applyOptColor(i);
            toolColorPop_->close(now());
        };
        toolColorItems_.push_back(b);
    }

    // --- weight / size -----------------------------------------------------
    toolSizePop_ = emplace<Popover>();
    toolSizePop_->setOwner(toolOptSize_);
    toolSizePop_->setFrame(0.f, 0.f, 176.f, 128.f);
    // Six rows: the stroke tools use all of them and show a sample line,
    // the Text tool uses the first four and shows point sizes.
    for (int i = 0; i < kWeightCount; ++i) {
        auto* b = toolSizePop_->emplace<Button>("", "", ButtonStyle::ListItem);
        b->onClick = [this, i] {
            applyOptWidth(i);
            toolSizePop_->close(now());
        };
        toolSizeItems_.push_back(b);
    }

    // --- opacity -----------------------------------------------------------
    // Presets plus a slider: the presets are what gets used, the slider is
    // there when a preset is not quite right.
    toolOpacityPop_ = emplace<Popover>();
    toolOpacityPop_->setOwner(toolOptOpacity_);
    toolOpacityPop_->setFrame(0.f, 0.f, 196.f, 132.f);
    static constexpr int kOpacPct[4] = {25, 50, 75, 100};
    for (int i = 0; i < 4; ++i) {
        auto* b = toolOpacityPop_->emplace<Button>(
            std::to_string(kOpacPct[i]) + "%", "", ButtonStyle::Segment);
        b->compact = true;
        b->onClick = [this, i] {
            applyOptOpacity(static_cast<float>(kOpacPct[i]) / 100.f);
            toolOpacityPop_->close(now());
        };
        toolOpacityItems_.push_back(b);
    }
    toolOpacitySlider_ = toolOpacityPop_->emplace<Slider>(0.15f, 1.f, 1.f);
    toolOpacitySlider_->onChange = [this](float v) { applyOptOpacity(v); };

    // --- font family -------------------------------------------------------
    toolFontPop_ = emplace<Popover>();
    toolFontPop_->setOwner(toolOptFont_);
    toolFontPop_->setFrame(0.f, 0.f, 168.f, 128.f);
    static constexpr const char* kFamNames[3] = {"Sans", "Serif", "Mono"};
    for (int i = 0; i < 3; ++i) {
        auto* b = toolFontPop_->emplace<Button>(kFamNames[i], "",
                                                ButtonStyle::ListItem);
        b->onClick = [this, i] {
            applyOptFont(i);
            toolFontPop_->close(now());
        };
        toolFontItems_.push_back(b);
    }
}

void ReaderScreen::syncToolOptions() {
    if (!toolOptColor_) return;
    const bool wasSyncing = syncingOpts_;
    syncingOpts_ = true;
    struct Restore {
        bool* f; bool v;
        ~Restore() { *f = v; }
    } restore{&syncingOpts_, wasSyncing};

    const Annotation* sel = selectedAnnot_ ? annots_.get(selectedAnnot_) : nullptr;
    if (selectedAnnot_ && !sel) selectedAnnot_ = 0;   // deleted under us

    // Which options exist depends on what is being edited, not just on the
    // active tool: a selected annotation brings its own.
    bool hasColour = false, hasWeight = false, hasOpacity = false, hasFont = false;
    bool isText = false;

    if (sel) {
        optTarget_ = OptTarget::Annotation;
        isText = sel->kind == AnnotKind::Text;
        hasColour = true;
        hasOpacity = true;
        hasWeight = sel->kind == AnnotKind::Ink || sel->kind == AnnotKind::Box ||
                    sel->kind == AnnotKind::Ellipse || sel->kind == AnnotKind::Line ||
                    sel->kind == AnnotKind::Arrow || sel->kind == AnnotKind::Stamp ||
                    sel->kind == AnnotKind::Signature || isText;
        hasFont = isText;
    } else if (stage_ && stage_->hasSelection()) {
        // Text is selected: the options act on the markup that is about to
        // be applied, which is the thing the user is looking at.
        optTarget_ = OptTarget::Markup;
        hasColour = true;
        hasOpacity = true;
    } else {
        optTarget_ = OptTarget::Tool;
        const Tool t = state_.tool;
        isText = t == Tool::Text;
        hasColour = t == Tool::Pen || t == Tool::Highlight || t == Tool::Text ||
                    t == Tool::Note || t == Tool::Shapes || t == Tool::Stamp ||
                    t == Tool::Signature;
        // The freehand Highlight marker has a real width-controlled band and
        // magnifier preview. Leaving it out disabled the top Thickness chip
        // even while the same six weights worked in the inspector/thumb menu.
        hasWeight = t == Tool::Pen || t == Tool::Highlight ||
                    t == Tool::Shapes || t == Tool::Eraser ||
                    t == Tool::Stamp || t == Tool::Signature || isText;
        hasOpacity = t == Tool::Pen || t == Tool::Highlight || t == Tool::Shapes ||
                     t == Tool::Note || t == Tool::Stamp || t == Tool::Signature || isText;
        hasFont = isText;
    }

    if (const char* d = std::getenv("FOLIO_DEBUG_OPT"); d && *d == '1') {
        std::fprintf(stderr,
                     "[opt] sync tool=%d target=%d col=%d wt=%d op=%d fo=%d\n",
                     static_cast<int>(state_.tool), static_cast<int>(optTarget_),
                     hasColour, hasWeight, hasOpacity, hasFont);
    }
    // Three predictable option slots on phones, even for tools without a
    // meaningful colour/weight/opacity (Select and Eraser). Inapplicable
    // controls stay visibly disabled rather than silently changing another
    // tool. Desktop retains only applicable controls plus the text font.
    const bool compactSlots = LayoutMetrics::forViewport(frame.w, frame.h).panelsOverlay &&
                              state_.mode != Mode::Read;
    toolOptColor_->visible = hasColour || compactSlots;
    toolOptSize_->visible = hasWeight || compactSlots;
    toolOptOpacity_->visible = hasOpacity || compactSlots;
    toolOptFont_->visible = hasFont && !compactSlots;
    toolOptColor_->acceptsPointer = hasColour;
    toolOptSize_->acceptsPointer = hasWeight;
    toolOptOpacity_->acceptsPointer = hasOpacity;
    toolOptColor_->opacity = hasColour ? 1.f : .36f;
    toolOptSize_->opacity = hasWeight ? 1.f : .36f;
    toolOptOpacity_->opacity = hasOpacity ? 1.f : .36f;
    toolOptSize_->tooltip = isText ? "Text size" : "Thickness";
    toolOptSize_->icon = isText ? "text-size" : "stroke-weight";

    // --- current values ----------------------------------------------------
    int inkIdx = annotColorIdx_;
    float opacity = annotOpacity_ ? annotOpacity_->value() : 1.f;
    int sizeIdx = annotWidthSeg_ ? annotWidthSeg_->selected() : 1;
    int fontIdx = textFontSeg_ ? textFontSeg_->selected() : 0;

    if (optTarget_ == OptTarget::Markup)
        opacity = markupOpacity_;
    if (sel) {
        inkIdx = nearestInk(sel->color);
        opacity = sel->opacity;
        if (isText) {
            static constexpr float kPts[4] = {12.f, 16.f, 22.f, 30.f};
            const float pt = sel->fontSize * 792.f;
            sizeIdx = 0;
            float best = 1e9f;
            for (int k = 0; k < 4; ++k) {
                const float d = std::fabs(kPts[k] - pt);
                if (d < best) { best = d; sizeIdx = k; }
            }
            fontIdx = static_cast<int>(sel->font);
        } else {
                    sizeIdx = 1;
            float best = 1e9f;
            for (int k = 0; k < kWeightCount; ++k) {
                const float d = std::fabs(kToolWidths[k] - sel->width);
                if (d < best) { best = d; sizeIdx = k; }
            }
        }
    } else if (optTarget_ == OptTarget::Markup) {
        inkIdx = markupColorIdx_;
    } else if (isText && textSizeSeg_) {
        sizeIdx = textSizeSeg_->selected();
    }

    toolOptColor_->swatchColor =
        kToolInks[static_cast<std::size_t>(std::clamp(inkIdx, 0, kInkCount - 1))];
    // The selection popup's chip is the same setting shown in a second
    // place, so it has to be refreshed here too - updating it only when the
    // selection was first made left it showing the previous colour.
    if (selColorBtn_)
        selColorBtn_->swatchColor =
            kToolInks[static_cast<std::size_t>(
                std::clamp(markupColorIdx_, 0, kInkCount - 1))];

    // --- popover contents --------------------------------------------------
    static constexpr const char* kSizeNames[4] = {"12 pt", "16 pt", "22 pt",
                                                  "30 pt"};
    // Display thickness for the weight rows - the same ladder the panel
    // strip uses, so the two controls agree about what "4" looks like.
    static constexpr float kWeightSample[kWeightCount] = {1.f, 2.f,  3.5f,
                                                          5.f, 7.5f, 11.f};
    for (std::size_t k = 0; k < toolSizeItems_.size(); ++k) {
        const bool show = isText ? (k < 4) : true;
        toolSizeItems_[k]->visible = show;
        toolSizeItems_[k]->isWidthSample = !isText;
        toolSizeItems_[k]->widthSample = kWeightSample[k];
        toolSizeItems_[k]->sampleColor =
            kToolInks[static_cast<std::size_t>(
                std::clamp(inkIdx, 0, kInkCount - 1))];
        toolSizeItems_[k]->label = (isText && k < 4) ? kSizeNames[k] : "";
        toolSizeItems_[k]->toggled = (static_cast<int>(k) == sizeIdx);
    }
    // The command-bar chip shows the weight itself rather than a glyph.
    if (toolOptSize_) {
        toolOptSize_->isWidthSample = !isText;
        toolOptSize_->widthSample =
            kWeightSample[static_cast<std::size_t>(
                std::clamp(sizeIdx, 0, kWeightCount - 1))];
        toolOptSize_->sampleColor =
            kToolInks[static_cast<std::size_t>(
                std::clamp(inkIdx, 0, kInkCount - 1))];
    }
    // Tint the panel strip's samples with the live ink too.
    if (annotWidthSeg_)
        annotWidthSeg_->setSampleColor(
            kToolInks[static_cast<std::size_t>(
                std::clamp(inkIdx, 0, kInkCount - 1))]);
    for (std::size_t k = 0; k < toolColorItems_.size(); ++k)
        toolColorItems_[k]->toggled =
            (static_cast<int>(k) == std::clamp(inkIdx, 0, kInkCount - 1));
    for (std::size_t k = 0; k < toolFontItems_.size(); ++k)
        toolFontItems_[k]->toggled = (static_cast<int>(k) == fontIdx);
    static constexpr int kOpacPct[4] = {25, 50, 75, 100};
    for (std::size_t k = 0; k < toolOpacityItems_.size(); ++k)
        toolOpacityItems_[k]->toggled =
            std::fabs(static_cast<float>(kOpacPct[k]) / 100.f - opacity) < 0.02f;
    if (toolOpacitySlider_) toolOpacitySlider_->setValue(opacity);

    // A control that no longer applies must not leave its popover hanging.
    if (!hasColour && toolColorPop_ && toolColorPop_->isOpen())
        toolColorPop_->close(now());
    if (!hasWeight && toolSizePop_ && toolSizePop_->isOpen())
        toolSizePop_->close(now());
    if (!hasOpacity && toolOpacityPop_ && toolOpacityPop_->isOpen())
        toolOpacityPop_->close(now());
    if (!hasFont && toolFontPop_ && toolFontPop_->isOpen())
        toolFontPop_->close(now());
}

std::string ReaderScreen::sidecarPath(const std::string& pdfPath) const {
    if (state_.recentStateFile.empty() || pdfPath.empty()) return {};
    // Stable FNV-1a, not std::hash (whose output may change across builds).
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : pdfPath) { hash ^= c; hash *= 1099511628211ULL; }
    char hex[32]{};
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(hash));
    return (std::filesystem::u8path(state_.recentStateFile).parent_path() /
            (std::string("folio-annotations-") + hex + ".txt")).string();
}

#if FOLIO_HAVE_PDFIUM_SOURCE
void ReaderScreen::syncImportedSuppression() {
    auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(pageSource_);
    if (!pdf) return;
    folio::engine::PdfiumWorker::Suppressed indices;
    for (const auto& a : annots_.all())
        if (a.sourceIndex >= 0)
            indices[a.page].emplace_back(a.sourceIndex, a.sourceSubtype);
    pdf->suppressSourceAnnotations(std::move(indices));
}

std::uint64_t ReaderScreen::adoptSourceAnnotation(
    int page, const folio::engine::PdfAnnotationInfo& info) {
    if (!info.editable || info.r <= info.l || info.b <= info.t) return 0;
    for (const auto& a : annots_.all())
        if (a.page == page && a.sourceIndex == info.index)
            return a.deleted ? 0 : a.id;
    Annotation a;
    a.page = page;
    a.sourceIndex = info.index;
    a.sourceSubtype = info.subtype;
    a.sourceFingerprint = info.fingerprint;
    a.box = {info.l, info.t, info.r - info.l, info.b - info.t};
    a.color = {info.red, info.green, info.blue, info.alpha};
    a.opacity = static_cast<float>(info.alpha) / 255.f;
    a.comment = info.contents;
    switch (info.subtype) {
        case FPDF_ANNOT_HIGHLIGHT: a.kind = AnnotKind::Highlight; a.textLinked = true; break;
        case FPDF_ANNOT_UNDERLINE: a.kind = AnnotKind::Underline; a.textLinked = true; break;
        case FPDF_ANNOT_STRIKEOUT: a.kind = AnnotKind::Strikethrough; a.textLinked = true; break;
        case FPDF_ANNOT_TEXT: a.kind = AnnotKind::Note; break;
        case FPDF_ANNOT_SQUARE: a.kind = AnnotKind::Box; break;
        case FPDF_ANNOT_CIRCLE: a.kind = AnnotKind::Ellipse; break;
        default: return 0;
    }
    const auto id = annots_.add(std::move(a));
    // Hide the original ONLY in this view document, never in the source file.
    syncImportedSuppression();
    persistAnnotations();
    for (std::size_t i = 0; i < pdfNotesRefs_.size(); ++i) {
        if (pdfNotesRefs_[i] != std::pair{page, info.index}) continue;
        pdfNotesRefs_.erase(pdfNotesRefs_.begin() + static_cast<std::ptrdiff_t>(i));
        pdfNotesEntries_.erase(pdfNotesEntries_.begin() + static_cast<std::ptrdiff_t>(i));
        pdfNotesInfo_.erase(pdfNotesInfo_.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }
    notesRevision_ = ~std::uint64_t{0};
    if (onDocumentChanged) onDocumentChanged();
    return id;
}
#endif

void ReaderScreen::persistAnnotations() {
    if (activeDoc_ < 0 || activeDoc_ >= static_cast<int>(docs_.size())) return;
    const std::string& path = docs_[static_cast<std::size_t>(activeDoc_)].path;
    const std::string file = sidecarPath(path);
    if (!file.empty()) annots_.saveSidecar(file, path);
}

void ReaderScreen::commitComment() {
    if (!commentField_ || !commentTarget_ ||
        commentField_->text == commentOriginal_) return;
    const auto ids = annots_.groupMembers(commentTarget_);
    std::vector<Annotation> before, after;
    for (std::uint64_t id : ids) {
        if (const Annotation* current = annots_.get(id)) {
            Annotation previous = *current;
            previous.comment = commentOriginal_;
            before.push_back(std::move(previous));
            after.push_back(*current);
        }
    }
    if (before.empty()) return;
    edits_.push(std::make_unique<ModifyManyCommand>(
        annots_, std::move(before), std::move(after), "edit comment"));
    commentOriginal_ = commentField_->text;
    syncUndoButtons();
}

void ReaderScreen::onAnnotPicked(std::uint64_t id, bool fromTouch) {
    if (id != commentTarget_) {
        commitComment();
        commentTarget_ = id;
        if (commentField_) {
            if (const Annotation* a = id ? annots_.get(id) : nullptr)
                commentField_->text = a->comment;
            else commentField_->text.clear();
            commentField_->setCaretToEnd();
            commentOriginal_ = commentField_->text;
        }
    }
    selectedAnnot_ = id;
    updateRightPanelContext();
    syncToolOptions();
    layoutTree(); // the selected mark collapses Hand/Select immediately

    // Picking an existing mark is an explicit "I want to change this", and
    // on a phone the options are a sheet away rather than visible in a
    // column. So the sheet comes up by itself here - but only here.
    // Raising it every time a tool is chosen would fight the user, since
    // choosing a tool is usually followed by drawing, not configuring.
    // A finger may continue dragging the mark immediately after selecting
    // it. Raising the inspector under that finger would hide the canvas and
    // steal the drag; the inspector FAB remains available when wanted.
    if (!fromTouch && sheetMode() && id != 0 && rightPanel_->collapsed()) {
        togglePhoneSheet(false);
    }
}

void ReaderScreen::buildPopovers() {
    // --- mode menu ---------------------------------------------------------
    modePop_ = emplace<Popover>();
    shapePop_ = emplace<Popover>();
    shapePop_->setFrame(0.f, 0.f, 240.f, 58.f);
    static constexpr PageStage::EditTool kShapeTools[] = {
        PageStage::EditTool::Ink, PageStage::EditTool::Box,
        PageStage::EditTool::Ellipse, PageStage::EditTool::Line,
        PageStage::EditTool::Arrow};
    static constexpr const char* kShapeIcons[] = {
        "pen", "shape-rect", "shape-oval", "shape-line", "shape-arrow"};
    static constexpr const char* kShapeNames[] = {
        "Freehand", "Rectangle", "Circle / ellipse", "Straight line", "Arrow"};
    for (int i = 0; i < 5; ++i) {
        auto* item = shapePop_->emplace<Button>("", kShapeIcons[i], ButtonStyle::Icon);
        item->tooltip = kShapeNames[i];
        item->toggled = kShapeTools[i] == drawingShape_;
        item->onClick = [this, i] { chooseDrawingShape(kShapeTools[i]); };
        shapeItems_.push_back(item);
    }
    toolOverflowPop_ = emplace<Popover>();
    toolOverflowPop_->setOwner(toolOverflowBtn_);
    rebuildToolButtons(now());
    {
        static constexpr const char* kNames[] = {"Read", "Annotate", "Edit"};
        static constexpr const char* kIcons[] = {"eye", "highlighter", "cursor"};
        static constexpr const char* kHints[] = {"Ctrl 1", "Ctrl 2", "Ctrl 3"};
        const float rowH = 36.f;
        modePop_->setFrame(0.f, 0.f, 212.f, rowH * 3.f + 12.f);
        for (int i = 0; i < 3; ++i) {
            auto* r = modePop_->emplace<Button>(kNames[i], kIcons[i],
                                                ButtonStyle::ListItem);
            r->trailing = kHints[i];
            r->toggled = (static_cast<int>(state_.mode) == i);
            r->onClick = [this, i] { setMode(static_cast<Mode>(i)); };
            modeItems_.push_back(r);
        }
    }

    findPop_ = emplace<Popover>();
    findPop_->setFrame(0.f, 0.f, 320.f, 108.f);
    // --- page layout menu --------------------------------------------------
    layoutPop_ = emplace<Popover>();
    {
        static constexpr const char* kNames[] = {"Single page", "Continuous",
                                                 "Two up", "All together"};
        static constexpr const char* kIcons[] = {"single-page", "continuous",
                                                 "two-up", "grid"};
        static constexpr const char* kHints[] = {"One page at a time",
                                                 "Scroll continuously",
                                                 "Facing pages",
                                                 "Every page, side by side"};
        const float rowH = 42.f;
        layoutPop_->setFrame(0.f, 0.f, 248.f, rowH * 4.f + 12.f);
        for (int i = 0; i < 4; ++i) {
            auto* r = layoutPop_->emplace<Button>(kNames[i], kIcons[i],
                                                  ButtonStyle::ListItem);
            r->subtitle = kHints[i];
            r->toggled = (i == 1);
            r->onClick = [this, i] { setLayoutMode(i); };
            layoutItems_.push_back(r);
        }
    }

    // The stock Popover supplies the same anchor-clipped open and reverse
    // close used elsewhere. On a phone the percentage itself is its owner.
    zoomPop_ = emplace<Popover>();
    zoomPop_->setFrame(0.f, 0.f, 226.f, 180.f);
    for (int i = 0; i < 8; ++i) {
        static constexpr const char* labels[] = {
            "Fit width", "Fit page", "50%", "75%", "100%", "125%", "150%", "200%"};
        static constexpr const char* icons[] = {
            "fit-width", "fit-page", "", "", "", "", "", ""};
        auto* b = zoomPop_->emplace<Button>(labels[i], icons[i], ButtonStyle::Ghost);
        b->compact = true;
        b->onClick = [this, i] {
            stage_->clearZoomFocus();
            if (i == 0) stage_->setZoomMode(ZoomMode::FitWidth, now());
            else if (i == 1) stage_->setZoomMode(ZoomMode::FitPage, now());
            else {
                static constexpr float values[] = {.5f, .75f, 1.f, 1.25f, 1.5f, 2.f};
                stage_->setZoom(values[i - 2], now());
            }
            zoomPop_->close(now());
        };
        zoomPresetItems_.push_back(b);
    }

    findField_ = findPop_->emplace<TextField>("Find in document");
    findField_->leadingIcon = "search";
    findField_->onChanged = [this](const std::string& q) { runSearch(q); };
    findField_->onSubmit = [this] { stepSearch(+1); };

    findCount_ = findPop_->emplace<Label>("", tokens::type_small,
                                          TextRole::Muted);
    findPrev_ = findPop_->emplace<Button>("", "chevron-up", ButtonStyle::Ghost);
    findPrev_->onClick = [this] { stepSearch(-1); };
    findNext_ = findPop_->emplace<Button>("", "chevron-down", ButtonStyle::Ghost);
    findNext_->onClick = [this] { stepSearch(+1); };

    displayPop_ = emplace<Popover>();
    displayPop_->setFrame(0.f, 0.f, 290.f, 214.f);
    pageThemeSeg_ = displayPop_->emplace<SegmentedControl>(
        std::vector<SegmentedControl::Item>{
            {"Paper", "", "White paper"}, {"Sepia", "", "Warm paper"},
            {"Dark", "", "Inverted paper"}},
        0);
    pageThemeSeg_->onChange = [this](int i) {
        applyPageTheme(static_cast<PageTheme>(i));
    };
    // Two explicit buttons, not a sliding switch. A switch states "on/off"
    // for something with no natural off; Day and Night are two destinations,
    // and the pressed one should look selected.
    dayBtn_ = displayPop_->emplace<Button>("Day", "sun", ButtonStyle::Segment);
    nightBtn_ = displayPop_->emplace<Button>("Night", "moon", ButtonStyle::Segment);
    dayBtn_->onClick = [this] { chooseScheme(ColorScheme::Light, dayBtn_); };
    nightBtn_->onClick = [this] { chooseScheme(ColorScheme::Dark, nightBtn_); };
    syncSchemeButtons();

    buildSelectionPopup();

    gridSlider_ = displayPop_->emplace<Slider>(tokens::size::gridThumbMin,
                                               tokens::size::gridThumbMax,
                                               tokens::size::gridThumbDefault);
    gridSlider_->onChange = [this](float v) { stage_->setGridSize(v); };
}

void ReaderScreen::buildSelectionPopup() {
    selPop_ = emplace<Popover>();
    selPop_->preferAbove = true;   // never cover the text it acts on
    selPop_->setFrame(0.f, 0.f, kSelPopW, kSelPopH);

    struct Item { const char* label; const char* icon; AnnotKind kind; };
    // Copy is last and separated: the three markups mutate the document,
    // Copy does not.
    static constexpr Item kItems[3] = {
        {"Highlight", "highlighter",   AnnotKind::Highlight},
        {"Strike",    "strikethrough", AnnotKind::Strikethrough},
        {"Underline", "underline",     AnnotKind::Underline},
    };
    for (const Item& it : kItems) {
        // Icon only. The tooltip carries the name - a four-word row sitting
        // on top of the text you just selected is in the way of the thing
        // it is describing.
        auto* b = selPop_->emplace<Button>("", it.icon, ButtonStyle::Ghost);
        b->tooltip = it.label;
        b->compact = true;
        const AnnotKind k = it.kind;
        b->onClick = [this, k] { applyTextMarkup(k); };
        selBtns_.push_back(b);
    }
    // The ink the three markups will use, and a way to change it without
    // leaving the selection. It edits the same value as the Highlight
    // tool's colour control - they are one setting.
    selColorBtn_ = selPop_->emplace<Button>("", "", ButtonStyle::Ghost);
    selColorBtn_->isSwatch = true;
    selColorBtn_->swatchColor = kToolInks[0];
    selColorBtn_->tooltip = "Markup colour";
    selColorBtn_->compact = true;
    selColorBtn_->onClick = [this] {
        optTarget_ = OptTarget::Markup;
        toolColorPop_->setZOrder(zlayer::popover + 1);
        openOptPopover(toolColorPop_, selColorBtn_);
    };
    selBtns_.push_back(selColorBtn_);

    auto* copy = selPop_->emplace<Button>("", "copy", ButtonStyle::Ghost);
    copy->tooltip = "Copy";
    copy->compact = true;
    copy->onClick = [this] {
        const std::string sel = stage_->selectedText();
        if (!sel.empty() && onCopyText) onCopyText(sel);
        OverlayHost::current().showToast(
            std::to_string(sel.size()) + " characters copied", now());
        closeSelectionPopup();
    };
    selBtns_.push_back(copy);
}

void ReaderScreen::closeSelectionPopup() {
    if (selPop_ && selPop_->isOpen()) selPop_->close(now());
    if (toolColorPop_ && optTarget_ == OptTarget::Markup &&
        toolColorPop_->isOpen()) toolColorPop_->close(now());
}

void ReaderScreen::applyTextMarkup(AnnotKind kind) {
    std::vector<std::pair<int, Rect>> quads;
    stage_->selectionQuads(quads);
    if (quads.empty()) { closeSelectionPopup(); return; }

    // One annotation per line, not one per selection: a selection spanning
    // three lines must not produce a single box swallowing the margins, and
    // PDF text markup is defined per quad anyway.
    std::vector<Annotation> added;
    added.reserve(quads.size());
    for (const auto& [page, box] : quads) {
        Annotation a;
        a.kind = kind;
        a.textLinked = true;
        a.page = page;
        a.box = box;
        // The ink the Highlight tool is set to, not a hard-coded yellow.
        // These used to be separate: changing the highlighter's colour did
        // nothing to the colour you got from selecting text and pressing
        // Highlight, which is indefensible when both are called "highlight".
        a.color = kToolInks[static_cast<std::size_t>(
            std::clamp(markupColorIdx_, 0, kInkCount - 1))];
        a.opacity = markupOpacity_;
        a.width = 2.f;
        a.id = annots_.add(a);
        added.push_back(a);
    }
    // Tie the lines together. The first id doubles as the group id: it is
    // already unique and already allocated, so no second counter can drift
    // out of step with it.
    if (added.size() > 1) {
        const std::uint64_t g = added.front().id;
        for (Annotation& a : added) {
            a.group = g;
            annots_.replace(a.id, a);   // keep the store and the undo copy equal
        }
    }
    // One entry, not one per line: Ctrl+Z after marking a paragraph must
    // take the whole paragraph back.
    edits_.push(std::make_unique<AddManyCommand>(annots_, std::move(added),
                                                 "markup"));
    persistAnnotations();
    syncUndoButtons();
    const char* what = kind == AnnotKind::Highlight     ? "Highlighted"
                       : kind == AnnotKind::Underline   ? "Underlined"
                                                        : "Struck through";
    OverlayHost::current().showToast(
        std::string(what) + " " + std::to_string(quads.size()) +
            (quads.size() == 1 ? " line" : " lines"),
        now());
    stage_->clearSelection();
    closeSelectionPopup();
}

void ReaderScreen::dropDanglingSelection() {
    // Undoing a *move* should leave the annotation selected - you probably
    // want to try again. Undoing an *add* destroys it, and handles drawn
    // around an id that no longer resolves are a crash waiting to happen.
    const std::uint64_t id = stage_->selectedAnnot();
    if (id && !annots_.get(id)) stage_->deselectAnnot();
}

void ReaderScreen::reloadAfterUndo(int toPage) {
    // A page-op undo swapped the file on disk. The page source holds an open
    // handle and a raster cache keyed on page index, both of which are now
    // describing a document that no longer exists, so this has to be a full
    // reopen rather than an invalidate.
#if FOLIO_HAVE_PDFIUM_SOURCE
    auto* pdf = dynamic_cast<folio::engine::PdfiumPageSource*>(pageSource_.get());
    if (!pdf) return;
    const std::string path = pdf->path();
    annots_.clear();
    persistAnnotations();
    if (openFile(path, now()))
        stage_->goToPage(std::clamp(toPage, 1, std::max(1, pageCount_)), now());
#else
    (void)toPage;
#endif
}

void ReaderScreen::doUndo() {
    if (!edits_.undo()) {
        OverlayHost::current().showToast("Nothing to undo", now());
        return;
    }
    dropDanglingSelection();
    persistAnnotations();
    syncUndoButtons();
    OverlayHost::current().showToast("Undo", now());
}

void ReaderScreen::doRedo() {
    if (!edits_.redo()) {
        OverlayHost::current().showToast("Nothing to redo", now());
        return;
    }
    dropDanglingSelection();
    persistAnnotations();
    syncUndoButtons();
    OverlayHost::current().showToast("Redo", now());
}

void ReaderScreen::syncUndoButtons() {
    // Greying these out is the only feedback that says "you are at the
    // beginning of history" before the user clicks and gets a toast.
    for (Widget* w : cmdItems_) {
        auto* b = dynamic_cast<Button*>(w);
        if (!b) continue;
        if (b->tooltip.rfind("Undo", 0) == 0) b->enabled = edits_.canUndo();
        else if (b->tooltip.rfind("Redo", 0) == 0) b->enabled = edits_.canRedo();
    }
    if (undoPanelBtn_) undoPanelBtn_->enabled = edits_.canUndo();
    if (redoPanelBtn_) redoPanelBtn_->enabled = edits_.canRedo();
}

void ReaderScreen::syncSchemeButtons() {
    const bool dark = state_.theme.scheme() == ColorScheme::Dark;
    if (dayBtn_) dayBtn_->toggled = !dark;
    if (nightBtn_) nightBtn_->toggled = dark;
    if (themeBtn_) themeBtn_->icon = dark ? "moon" : "sun";
}

void ReaderScreen::chooseScheme(ColorScheme s, Widget* from) {
    // The reveal grows from the button that was pressed, so the change
    // reads as caused by that button rather than as a global flash.
    folio::render::Point c{0.f, 0.f};
    if (from) c = from->toWindow({from->frame.w * 0.5f, from->frame.h * 0.5f});
    state_.theme.setScheme(s, now(), c.x, c.y);
    syncSchemeButtons();
}

void ReaderScreen::openDocument(const std::string& title, int pages, double now) {
    docTitle_ = title;
    if (docBtn_) docBtn_->label = docTitle_;
    pageCount_ = std::max(1, pages);
    if (tabs_->count() == 0) {
        tabs_->addTab(title, now);
    }
    thumbs_->setPageCount(pageCount_);
    outline_->setPageCount(pageCount_);
    auto src = std::make_shared<PlaceholderPageSource>(pageCount_);
    thumbs_->setSource(src.get());
    pageSource_ = src;                       // keep it alive for the thumbnails
#if FOLIO_HAVE_PDFIUM_SOURCE
    if (auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(src))
        pdf->setWakeCallback(onWorkerWake);
#endif
    stage_->setSource(std::move(src), now);
    // Open fit-to-width, not at a fixed 1.25x. The prototype's 125% was a
    // fixed-size mock; a real reader has to cope with A4, Letter and the
    // odd A5 preprint, and "125%" means something different for each. Fit
    // mode is also what lets the page reclaim the space when a side panel
    // collapses - a fixed zoom just leaves grey margin there.
    stage_->setZoomMode(ZoomMode::FitWidth, now);
    syncPageLabel();
    // A fresh document has nothing to undo, and must not inherit the last
    // one's history. Page-op undo works by writing saved bytes back to a
    // stored path, so a stale entry would not merely undo the wrong thing -
    // it would silently rewrite a file the user is no longer looking at.
    edits_.clear();
    syncUndoButtons();
}

#if FOLIO_HAVE_PODOFO_SOURCE || FOLIO_HAVE_PDFIUM_SOURCE
namespace {
/// Translate the UI's annotation into the engine's write-side description.
/// The two structs are deliberately separate: folio-ui must not depend on
/// PoDoFo, and the engine must not depend on the widget layer.
folio::engine::AnnotSpec toSpec(const Annotation& a) {
    using K = folio::engine::AnnotSpec::Kind;
    folio::engine::AnnotSpec s;
    switch (a.kind) {
        case AnnotKind::Highlight:     s.kind = K::Highlight; break;
        case AnnotKind::Underline:     s.kind = K::Underline; break;
        case AnnotKind::Strikethrough: s.kind = K::StrikeOut; break;
        case AnnotKind::Ink:
        case AnnotKind::Signature:     s.kind = K::Ink;       break;
        case AnnotKind::Note:      s.kind = K::Note;      break;
        case AnnotKind::Text:      s.kind = K::FreeText;  break;
        case AnnotKind::Stamp:     s.kind = K::Stamp;     break;
        case AnnotKind::Box:       s.kind = K::Box;       break;
        case AnnotKind::Line:
        case AnnotKind::Arrow:     s.kind = K::Ink;       break;
        default:                   s.kind = K::Ellipse;   break;
    }
    s.page = a.page;
    s.sourceIndex = a.sourceIndex;
    s.sourceSubtype = a.sourceSubtype;
    s.sourceFingerprint = a.sourceFingerprint;
    s.deleted = a.deleted;
    s.l = a.box.x;
    s.t = a.box.y;
    s.r = a.box.x + a.box.w;
    s.b = a.box.y + a.box.h;
    s.cr = a.color.r;
    s.cg = a.color.g;
    s.cb = a.color.b;
    // The overlay tints highlights rather than covering them; the exported
    // appearance stream has to do the same or saving changes how the
    // document looks.
    s.opacity = a.kind == AnnotKind::Highlight ? a.opacity * kHighlightAlpha
                                               : a.opacity;
    s.contents = a.comment;
    if (a.kind == AnnotKind::Text || a.kind == AnnotKind::Stamp) {
        s.label = a.kind == AnnotKind::Stamp && a.text.empty() ? "APPROVED" : a.text;
        // Store the complete label as /Contents too: a long/wrapped label
        // cannot be silently lost if the visible stamp bounds clip glyphs.
        if (!s.label.empty())
            s.contents = s.label + (a.comment.empty() ? "" : "\n" + a.comment);
    }
    // Free text carries its string, and the glyph height the overlay used,
    // so the exported appearance matches what was on screen. The overlay
    // sizes at 74% of the box height; a nominal 792pt page turns that
    // fraction into points.
    if (a.kind == AnnotKind::Text) {
        s.fontSize = (a.fontSize > 0.f ? a.fontSize : a.box.h * 0.74f) * 792.f;
        // The base-14 families every PDF viewer must provide, so free text
        // survives export without embedding anything.
        switch (a.font) {
            case folio::render::FontFamily::Serif: s.fontName = "Times-Roman"; break;
            case folio::render::FontFamily::Mono:  s.fontName = "Courier";     break;
            default:                               s.fontName = "Helvetica";   break;
        }
    }
    // Annotation::width is in page units x1000; PDF wants points. A nominal
    // 612pt page makes the two agree at the size the user drew at.
    s.width = std::max(0.5f, a.width * 0.612f);
    for (const auto& p : a.points) s.points.push_back({p.x, p.y});
    if (a.kind == AnnotKind::Arrow && a.points.size() >= 2) {
        const auto start = a.points.front(), end = a.points.back();
        const float dx = end.x - start.x, dy = end.y - start.y;
        const float length = std::hypot(dx, dy);
        if (length > 1e-4f) {
            const float tip = .014f, wing = .007f;
            const float ux = dx / length, uy = dy / length;
            s.points.push_back({end.x - ux * tip - uy * wing,
                                end.y - uy * tip + ux * wing});
            s.points.push_back({end.x, end.y});
            s.points.push_back({end.x - ux * tip + uy * wing,
                                end.y - uy * tip - ux * wing});
        }
    }
    if (!s.points.empty()) {
        float lo_x = s.points[0][0], hi_x = lo_x;
        float lo_y = s.points[0][1], hi_y = lo_y;
        for (const auto& p : s.points) {
            lo_x = std::min(lo_x, p[0]); hi_x = std::max(hi_x, p[0]);
            lo_y = std::min(lo_y, p[1]); hi_y = std::max(hi_y, p[1]);
        }
        const float pad = 0.008f;
        s.l = std::max(0.f, lo_x - pad);
        s.r = std::min(1.f, hi_x + pad);
        s.t = std::max(0.f, lo_y - pad);
        s.b = std::min(1.f, hi_y + pad);
    }
    return s;
}
}  // namespace
#endif

bool ReaderScreen::pollExport() {
#if FOLIO_HAVE_PDFIUM_SOURCE
    bool searchChanged = false;
    auto* searchPdf = dynamic_cast<folio::engine::PdfiumPageSource*>(pageSource_.get());
    if (searchPdf) {
        searchChanged = searchPdf->tick();
        searchPdf->annotationsForPage(std::max(0, stage_->currentPage() - 1));
        if (lastOutlineRevision_ != searchPdf->outlineRevision()) {
            lastOutlineRevision_ = searchPdf->outlineRevision();
            std::vector<OutlineList::Entry> toc;
            for (const auto& b : searchPdf->outline())
                toc.push_back(OutlineList::Entry{b.title, b.page, b.depth});
            outline_->setEntries(std::move(toc));
            searchChanged = true;
        }
    }
    if (searchPdf && !searchNeedle_.empty() &&
        searchCursor_ < searchPdf->pageCount()) {
        // Keep at most one extraction queued. Process a few already resident
        // pages per frame, then let visible raster jobs run first.
        for (int budget = 0; budget < 4 && searchCursor_ < searchPdf->pageCount();
             ++budget) {
            auto text = searchPdf->pageText(searchCursor_);
            if (!text) break;
            const int page = searchCursor_;
            std::size_t at = 0;
            while ((at = text->folded.find(searchNeedle_, at)) != std::u32string::npos) {
                searchHits_.push_back(PageStage::TextRun{
                    page, static_cast<int>(at), static_cast<int>(searchNeedle_.size())});
                at += searchNeedle_.size();
            }
            text.reset();
            searchPdf->releaseText(page);
            ++searchCursor_;
            searchChanged = true;
        }
        if (searchChanged) {
            if (searchIndex_ < 0 && !searchHits_.empty()) searchIndex_ = 0;
            stage_->setSearchHits(searchHits_, searchIndex_);
            syncFindLabel();
        }
    }
    if (searchPdf && notes_ && notes_->visible && notesScanCursor_ < pageCount_) {
        // One PDF page per worker wake; never scan all metadata before first
        // paint and never index when Notes is hidden or the app is idle.
        if (const auto* list = searchPdf->annotationsForPage(notesScanCursor_)) {
            const auto beforeNotes = pdfNotesRefs_.size();
            for (const auto& info : *list) {
                const bool highlight = info.subtype == FPDF_ANNOT_HIGHLIGHT;
                const bool sticky = info.subtype == FPDF_ANNOT_TEXT;
                if ((!highlight && !sticky && info.contents.empty()) ||
                    pdfNotesRefs_.size() >= 20000) continue;
                const std::string type = highlight ? "PDF highlight" :
                    sticky ? "PDF note" : "PDF annotation";
                std::string title = type +
                    (info.editable ? " · Tap to edit" : " · Preserved");
                if (!info.contents.empty()) title += " · " + info.contents.substr(0, 80);
                pdfNotesEntries_.push_back({std::move(title), notesScanCursor_, 0});
                pdfNotesRefs_.push_back({notesScanCursor_, info.index});
                pdfNotesInfo_.push_back(info);
            }
            if (pdfNotesRefs_.size() != beforeNotes &&
                (beforeNotes == 0 || pdfNotesRefs_.size() / 64 != beforeNotes / 64 ||
                 notesScanCursor_ + 1 == pageCount_)) {
                notesRevision_ = ~std::uint64_t{0};
                searchChanged = true;
            }
            if (notesScanCursor_ != stage_->currentPage() - 1)
                searchPdf->releaseAnnotations(notesScanCursor_);
            ++notesScanCursor_;
            if (notesScanCursor_ == pageCount_) {
                notesRevision_ = ~std::uint64_t{0};
                searchChanged = true;
            } else {
                searchPdf->annotationsForPage(notesScanCursor_);
            }
        }
    }
    if (!exportSource_) return searchChanged;
    exportSource_->tick();
    auto result = exportSource_->takeExportResult();
    if (!result) return searchChanged;
    auto completedSource = std::move(exportSource_);
#if defined(__ANDROID__)
    if (!result->error.empty() || result->exportPath.empty()) {
        exportPending_ = false;
        OverlayHost::current().showToast("Export failed: " + result->error, now());
    } else if (!(sharingExport_ ? onSharePdf : onExportPdf) ||
               !(sharingExport_ ? onSharePdf : onExportPdf)(result->exportPath,
                                                            "Folio-annotated.pdf")) {
        std::error_code ec;
        std::filesystem::remove(nativePath(result->exportPath), ec);
        exportPending_ = false;
        OverlayHost::current().showToast("Export failed: Android share/picker unavailable", now());
    }
    // Share copies the staging file asynchronously; keep this export busy
    // until Java has finished copying, so a second Save cannot replace it.
    if (!exportPending_) sharingExport_ = false;
#else
    std::error_code ec;
    if (!result->error.empty() || result->exportPath.empty()) {
        std::filesystem::remove(nativePath(desktopStagingPath_), ec);
        OverlayHost::current().showToast("Save As failed: " + result->error, now());
    } else if (std::filesystem::exists(nativePath(desktopExportPath_), ec) || ec) {
        std::filesystem::remove(nativePath(result->exportPath), ec);
        OverlayHost::current().showToast("Save As failed: destination already exists", now());
    } else {
        std::filesystem::rename(nativePath(result->exportPath),
                                nativePath(desktopExportPath_), ec);
        if (ec) {
            std::error_code ignored;
            std::filesystem::remove(nativePath(result->exportPath), ignored);
            OverlayHost::current().showToast("Save As failed: " + ec.message(), now());
        } else {
            OverlayHost::current().showToast("Saved PDF copy; original remains open", now());
        }
    }
    desktopExportPath_.clear();
    desktopStagingPath_.clear();
    exportPending_ = false;
#endif
    retireClosedSource(std::move(completedSource));
    return true;
#else
    return false;
#endif
}

void ReaderScreen::exportFinished(int status, const std::string& message) {
#if defined(__ANDROID__) && FOLIO_HAVE_PDFIUM_SOURCE
    if (!exportPending_) return;
    const bool wasShare = sharingExport_;
    exportPending_ = false;
    sharingExport_ = false;
    if (status > 0)
        OverlayHost::current().showToast(
            wasShare ? "Android share sheet opened; original remains open"
                     : "PDF exported; original remains open", now());
    else if (status < 0)
        OverlayHost::current().showToast("Export failed: " + message, now());
    else
        OverlayHost::current().showToast("Export cancelled; edits remain here", now());
#else
    (void)status; (void)message;
#endif
}

bool ReaderScreen::shareDocument() {
#if defined(__ANDROID__) && FOLIO_HAVE_PDFIUM_SOURCE
    if (exportPending_) return false;
    sharingExport_ = true;
    if (saveDocument()) return true;
    sharingExport_ = false;
    return false;
#else
    OverlayHost::current().showToast("Use Save As to share a PDF copy", now());
    return false;
#endif
}

bool ReaderScreen::saveDocument() {
    commitComment();
#if defined(__ANDROID__) && FOLIO_HAVE_PDFIUM_SOURCE
    if (exportPending_) {
        OverlayHost::current().showToast("Export already in progress", now());
        return false;
    }
    auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(pageSource_);
    if (!pdf || !pdf->isOpen()) {
        OverlayHost::current().showToast("Open a PDF before exporting", now());
        return false;
    }
    std::vector<folio::engine::AnnotSpec> specs;
    specs.reserve(annots_.count());
    for (const auto& a : annots_.all()) specs.push_back(toSpec(a));
    // The imported PDF is kept in private durable storage. Never modify it or the provider's
    // source URI; only a successful ACTION_CREATE_DOCUMENT publishes bytes.
    const std::string staged = pdf->path() + ".folio-export.pdf";
    if (!pdf->requestExport(std::move(specs), staged)) {
        OverlayHost::current().showToast("Could not queue PDF export", now());
        return false;
    }
    exportSource_ = std::move(pdf);
    exportPending_ = true;
    OverlayHost::current().showToast("Preparing PDF for export...", now());
    return true;
#elif FOLIO_HAVE_PODOFO_SOURCE && FOLIO_HAVE_PDFIUM_SOURCE
    auto* pdf = dynamic_cast<folio::engine::PdfiumPageSource*>(pageSource_.get());
    if (!pdf || !pdf->isOpen()) {
        OverlayHost::current().showToast("Nothing to save", now());
        return false;
    }
    const std::string path = pdf->path();

    folio::engine::PodofoDocument doc;
    if (!doc.open(path)) {
        OverlayHost::current().showToast("Save failed: " + doc.error(), now());
        return false;
    }

    std::vector<folio::engine::AnnotSpec> specs;
    specs.reserve(annots_.all().size());
    for (const auto& a : annots_.all()) specs.push_back(toSpec(a));

    if (!doc.writeAnnotations(specs) || !doc.saveIncremental()) {
        OverlayHost::current().showToast("Save failed: " + doc.error(), now());
        return false;
    }

    // Native PDF now owns the marks; remove Folio's private overlays to
    // avoid drawing them twice on reopen.
    const int page = stage_->currentPage();
    annots_.clear();
    persistAnnotations();
    if (openFile(path, now())) stage_->goToPage(page, now());

    OverlayHost::current().showToast(
        "Saved " + std::to_string(specs.size()) + " annotation" +
            (specs.size() == 1 ? "" : "s"),
        now());
    return true;
#elif FOLIO_HAVE_PDFIUM_SOURCE
    if (exportPending_) {
        OverlayHost::current().showToast("Save As already in progress", now());
        return false;
    }
    auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(pageSource_);
    if (!pdf || !pdf->isOpen() || !onPickSavePath) {
        OverlayHost::current().showToast("Open a PDF before saving a copy", now());
        return false;
    }
    std::vector<folio::engine::AnnotSpec> specs;
    specs.reserve(annots_.count());
    for (const auto& a : annots_.all()) specs.push_back(toSpec(a));
    const std::string source = pdf->path();
    auto suggested = utf8Path(nativePath(source).filename());
    if (suggested.size() >= 4 && suggested.substr(suggested.size() - 4) == ".pdf")
        suggested.resize(suggested.size() - 4);
    suggested += "-annotated.pdf";
    onPickSavePath(suggested, [this, pdf, specs = std::move(specs)](std::string out) mutable {
        if (out.empty()) return; // dialog cancelled; leave edits alone
        std::filesystem::path destination = nativePath(out);
        if (destination.extension().empty()) destination += ".pdf";
        if (destination.extension() != ".pdf") {
            OverlayHost::current().showToast("Save As requires a .pdf filename", now());
            return;
        }
        std::error_code ec;
        if (std::filesystem::exists(destination, ec) || ec) {
            OverlayHost::current().showToast(
                "Choose a new filename; Save As never overwrites an existing PDF", now());
            return;
        }
        // Generate a private staging file beside the output so the final
        // rename is fast and does not copy the entire PDF on the UI thread.
        auto staging = destination;
        staging += ".folio-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".part";
        if (std::filesystem::exists(staging, ec) || ec ||
            !pdf->requestExport(std::move(specs), utf8Path(staging))) {
            OverlayHost::current().showToast("Could not queue PDF Save As", now());
            return;
        }
        desktopExportPath_ = utf8Path(destination);
        desktopStagingPath_ = utf8Path(staging);
        exportSource_ = pdf;
        exportPending_ = true;
        OverlayHost::current().showToast("Preparing PDF copy...", now());
    });
    return true;
#else
    OverlayHost::current().showToast("Built without a PDF writer", now());
    return false;
#endif
}

bool ReaderScreen::applyPageOp(const std::string& op) {
#if FOLIO_HAVE_PODOFO_SOURCE && FOLIO_HAVE_PDFIUM_SOURCE
    auto* pdf = dynamic_cast<folio::engine::PdfiumPageSource*>(pageSource_.get());
    if (!pdf || !pdf->isOpen()) {
        OverlayHost::current().showToast("Open a document first", now());
        return false;
    }
    const std::string path = pdf->path();
    const int page = stage_->currentPage() - 1;   // 0-based

    // Snapshot before touching anything. Rotate could be inverted
    // arithmetically, but Delete cannot: once the file is rewritten without
    // page 7, page 7 is gone. Bytes are the only honest undo.
    const auto beforeSnap = (op == "Extract") ? std::filesystem::path{}
                                              : snapshotFile(path);

    folio::engine::PodofoDocument doc;
    if (!doc.open(path)) {
        OverlayHost::current().showToast(op + " failed: " + doc.error(), now());
        return false;
    }

    std::string done;
    if (op == "Rotate") {
        if (!doc.rotatePage(page, 90)) { done.clear(); }
        else done = "Rotated page " + std::to_string(page + 1);
    } else if (op == "Insert") {
        if (doc.insertBlankPage(page + 1))
            done = "Inserted a blank page after " + std::to_string(page + 1);
    } else if (op == "Delete") {
        if (doc.deletePage(page))
            done = "Deleted page " + std::to_string(page + 1);
    } else if (op == "Extract") {
        // Ask where to put it. Writing silently beside the original was
        // fine for a demo and wrong for an editor: the user has no say in
        // the name, no say in the folder, and no warning before it
        // overwrites last time's attempt.
        const auto dot = path.find_last_of('.');
        const auto slash = path.find_last_of("/\\");
        const std::string stem =
            (dot == std::string::npos ? path : path.substr(0, dot));
        const std::string suggested =
            (slash == std::string::npos ? stem : stem.substr(slash + 1)) +
            "_p" + std::to_string(page + 1) + ".pdf";

        if (onPickSavePath) {
            const int pg = page;
            const std::string src = path;
            onPickSavePath(suggested, [this, pg, src](std::string out) {
                if (out.empty()) return;          // cancelled
                folio::engine::PodofoDocument d2;
                if (!d2.open(src)) {
                    OverlayHost::current().showToast(
                        "Extract failed: " + d2.error(), now());
                    return;
                }
                if (!d2.extractPages({pg}, out)) {
                    OverlayHost::current().showToast(
                        "Extract failed: " + d2.error(), now());
                    return;
                }
                const auto sl = out.find_last_of("/\\");
                OverlayHost::current().showToast(
                    "Extracted to " +
                        (sl == std::string::npos ? out : out.substr(sl + 1)),
                    now());
            });
            return true;   // the dialog owns the rest; nothing changed here
        }

        // No picker installed: fall back to the old behaviour rather than
        // refusing to extract at all.
        const std::string out = stem + "_p" + std::to_string(page + 1) + ".pdf";
        if (doc.extractPages({page}, out)) {
            const auto sl = out.find_last_of("/\\");
            done = "Extracted to " +
                   (sl == std::string::npos ? out : out.substr(sl + 1));
            OverlayHost::current().showToast(done, now());
            return true;
        }
    }

    if (done.empty()) {
        OverlayHost::current().showToast(
            op + " failed: " + (doc.error().empty() ? "not possible here"
                                                    : doc.error()),
            now());
        return false;
    }

    if (!doc.saveAs(path)) {
        OverlayHost::current().showToast("Save failed: " + doc.error(), now());
        return false;
    }

    annots_.clear();
    persistAnnotations();
    if (openFile(path, now()))
        stage_->goToPage(std::min(page + 1, pageCount_), now());

    if (!beforeSnap.empty()) {
        if (auto afterSnap = snapshotFile(path); !afterSnap.empty()) {
            edits_.push(std::make_unique<FileSnapshotCommand>(
                done, path, beforeSnap, afterSnap,
                [this](int toPage) { reloadAfterUndo(toPage); },
                std::min(page + 1, pageCount_)));
            syncUndoButtons();
        }
    }

    OverlayHost::current().showToast(done, now());
    return true;
#else
    (void)op;
    OverlayHost::current().showToast("Built without PoDoFo", now());
    return false;
#endif
}

bool ReaderScreen::loadPath(const std::string& path, double now,
                            std::shared_ptr<IPageSource> prepared) {
    closeSelectionPopup();
    searchNeedle_.clear();
    searchHits_.clear();
    searchIndex_ = -1;
    searchCursor_ = 0;
#if FOLIO_HAVE_PDFIUM_SOURCE
    auto src = prepared
        ? std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(prepared)
        : std::make_shared<folio::engine::PdfiumPageSource>();
    if (!src || (!prepared && !src->open(path))) {
        OverlayHost::current().showToast(
            "Could not open: " + (src ? src->error() : std::string("invalid PDF source")), now);
        return false;
    }
    const auto slash = path.find_last_of("/\\");
    docTitle_ = slash == std::string::npos ? path : path.substr(slash + 1);
    if (docBtn_) docBtn_->label = docTitle_;
    pageCount_ = src->pageCount();
    lastOutlineRevision_ = src->outlineRevision();
    thumbs_->setPageCount(pageCount_);
    outline_->setPageCount(pageCount_);
    {
        std::vector<OutlineList::Entry> toc;
        for (const auto& b : src->outline())
            toc.push_back(OutlineList::Entry{b.title, b.page, b.depth});
        outline_->setEntries(std::move(toc));
    }
    thumbs_->setSource(src.get());
    pageSource_ = src;
#if FOLIO_HAVE_PDFIUM_SOURCE
    if (auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(src))
        pdf->setWakeCallback(onWorkerWake);
#endif
    stage_->setSource(src, now);
    stage_->setPageTheme(state_.theme.pageTheme());
    stage_->setZoomMode(ZoomMode::FitWidth, now);
    stage_->snapZoomToTarget(now); // visible page requests at final scale on frame one
    notesScanCursor_ = 0;
    pdfNotesEntries_.clear();
    pdfNotesRefs_.clear();
#if FOLIO_HAVE_PDFIUM_SOURCE
    pdfNotesInfo_.clear();
#endif
    notesRevision_ = ~std::uint64_t{0};
    annots_.clear();
    annots_.resetVisibility();
    for (std::size_t i = 0; i < 12; ++i) layerItems_[i]->toggled = true;
    annots_.loadSidecar(sidecarPath(path), path);
#if FOLIO_HAVE_PDFIUM_SOURCE
    syncImportedSuppression();
    src->annotationsForPage(0); // first visible page; later pages on demand
#endif
    selectedAnnot_ = 0;
    commentTarget_ = 0;
    if (commentField_) { commentField_->text.clear(); commentOriginal_.clear(); }
    updateRightPanelContext();
    syncPageLabel();
    return true;
#else
    (void)path;
    (void)now;
    return false;
#endif
}

void ReaderScreen::reapRetiredSources() {
    using namespace std::chrono_literals;
    for (auto it = retiringDocs_.begin(); it != retiringDocs_.end();) {
        if (it->wait_for(0ms) == std::future_status::ready) {
            it->get();
            it = retiringDocs_.erase(it);
        } else {
            ++it;
        }
    }
}

void ReaderScreen::retireClosedSource(std::shared_ptr<IPageSource> source) {
#if FOLIO_HAVE_PDFIUM_SOURCE
    if (!source || source == pageSource_ ||
        (exportSource_ && source == exportSource_)) return;
    for (const auto& doc : docs_)
        if (doc.source == source) return;
    auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(source);
    if (!pdf) return;
    // An old worker must not post to a window after the document has closed.
    // A callback already copied by an in-flight job remains safe: these
    // futures are joined before the native window is destroyed.
    pdf->setWakeCallback({});
    source.reset(); // ensure the last reference is dropped on the retire thread
    reapRetiredSources();
    retiringDocs_.push_back(std::async(std::launch::async,
        [pdf = std::move(pdf)]() mutable { pdf.reset(); }));
#else
    (void)source;
#endif
}

void ReaderScreen::stashActiveDoc() {
    if (activeDoc_ < 0 || activeDoc_ >= static_cast<int>(docs_.size())) return;
    OpenDoc& d = docs_[static_cast<std::size_t>(activeDoc_)];
    stage_->commitText();
    commitComment();
    persistAnnotations();
    d.annots = annots_.all();
    d.source = pageSource_;
    d.view = stage_->captureViewState(now());
    d.hasView = true;
    d.pageTheme = state_.theme.pageTheme();
    d.selectedAnnot = stage_->selectedAnnot();
    d.thumbnailScroll = thumbs_->scrollY();
    d.outlineScroll = outline_->scrollY();
    d.leftTab = state_.leftTab;
    d.leftCollapsed = state_.leftPanelCollapsed;
    d.rightCollapsed = state_.rightPanelCollapsed;
    d.focusMode = state_.focusMode;
#if FOLIO_HAVE_PDFIUM_SOURCE
    // Keep the mapped document/worker for instant tab switches, not every
    // tab's decoded 4K page bitmaps and GPU image keys.
    if (auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(d.source)) {
        pdf->parkVisualCaches(stage_->currentPage() - 1);
        // Never let an unbounded tab count turn into an unbounded warm cache.
        warmVisuals_.erase(std::remove_if(warmVisuals_.begin(), warmVisuals_.end(),
            [&](const auto& w) { auto p = w.lock(); return !p || p == d.source; }),
            warmVisuals_.end());
        warmVisuals_.insert(warmVisuals_.begin(), d.source);
        while (warmVisuals_.size() > 4) {
            if (auto old = warmVisuals_.back().lock())
                if (auto cold = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(old))
                    cold->suspendVisualCaches();
            warmVisuals_.pop_back();
        }
    }
    if (onDocumentChanged) onDocumentChanged();
#endif
    d.page = stage_->currentPage();
    d.pages = pageCount_;
    state_.rememberDocument(d.path, d.pages, d.page);
}

void ReaderScreen::setWorkerWakeCallback(std::function<void()> wake) {
    onWorkerWake = std::move(wake);
#if FOLIO_HAVE_PDFIUM_SOURCE
    for (auto& d : docs_)
        if (auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(d.source))
            pdf->setWakeCallback(onWorkerWake);
    if (auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(pageSource_))
        pdf->setWakeCallback(onWorkerWake);
#endif
}

bool ReaderScreen::hasDocumentWork() const {
#if FOLIO_HAVE_PDFIUM_SOURCE
    if (auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(pageSource_))
        return pdf->busy();
#endif
    return false;
}

void ReaderScreen::trimIdleVisualCaches() {
#if FOLIO_HAVE_PDFIUM_SOURCE
    if (auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(pageSource_))
        pdf->trimIdle();
#endif
}

void ReaderScreen::releaseVisualCaches() {
#if FOLIO_HAVE_PDFIUM_SOURCE
    // Minimise discards the framebuffer and GPU resources, but keep a tiny
    // CPU preview of the current page and neighbours for a smooth restore.
    // Every background tab becomes cold; none consumes a full tile grid.
    for (auto& doc : docs_) {
        if (doc.source == pageSource_) continue;
        if (auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(doc.source))
            pdf->suspendVisualCaches();
    }
    warmVisuals_.clear();
    if (auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(pageSource_))
        pdf->parkVisualCaches(stage_->currentPage() - 1, true);
#endif
    if (onDocumentChanged) onDocumentChanged();
}

void ReaderScreen::switchToDoc(int index, double now) {
    if (index < 0 || index >= static_cast<int>(docs_.size())) return;
    if (index == activeDoc_) return;
    stashActiveDoc();
    const OpenDoc& d = docs_[static_cast<std::size_t>(index)];
    if (!loadPath(d.path, now, d.source)) return;
    activeDoc_ = index;
    warmVisuals_.erase(std::remove_if(warmVisuals_.begin(), warmVisuals_.end(),
        [&](const auto& w) { auto p = w.lock(); return !p || p == d.source; }),
        warmVisuals_.end());
    // addExisting, not add: the ids have to survive a switch or any undo
    // entry still referring to them would resolve to nothing.
    annots_.clear();
    for (const Annotation& a : d.annots) annots_.addExisting(a);
    if (d.hasView) {
        applyPageTheme(d.pageTheme);
        state_.leftTab = d.leftTab;
        leftTabs_->setSelected(d.leftTab, now);
        state_.leftPanelCollapsed = d.leftCollapsed;
        state_.rightPanelCollapsed = d.rightCollapsed;
        leftPanel_->setCollapsedImmediately(d.leftCollapsed);
        rightPanel_->setCollapsedImmediately(d.rightCollapsed);
        state_.focusMode = d.focusMode;
        focusControlsVisible_ = d.focusMode;
        focusTouches_.clear();
        focusHideAt_ = d.focusMode ? now + kFocusIdleSeconds : 0.0;
        focusT_.reset(d.focusMode ? 1.f : 0.f);
        if (focusBtn_) {
            focusBtn_->toggled = d.focusMode;
            focusBtn_->icon = d.focusMode ? "minimise-view" : "maximise-view";
        }
        if (focusExitBtn_) focusExitBtn_->visible = d.focusMode;
    stage_->setFocusScrollbarsVisible(!d.focusMode || focusControlsVisible_);
        syncPhoneFabs();
        layoutTree(); // establish the destination viewport BEFORE restoring scroll
        stage_->restoreViewState(d.view, now);
        thumbs_->restoreScrollY(d.thumbnailScroll);
        outline_->restoreScrollY(d.outlineScroll);
        selectedAnnot_ = annots_.get(d.selectedAnnot) ? d.selectedAnnot : 0;
        stage_->selectAnnot(selectedAnnot_);
        updateRightPanelContext();
        syncToolOptions();
        // Restoring a page/layout cannot be a soft jump: publish the focus
        // immediately so PDFium prioritises this tab's visible preview.
    } else {
        stage_->goToPage(d.page, now);
    }
    state_.rememberDocument(d.path, pageCount_, stage_->currentPage());
    // Undo history is per document and page-op undo writes bytes back to a
    // stored path, so carrying it across a switch could rewrite the wrong
    // file. Clearing is the conservative choice.
    edits_.clear();
    syncUndoButtons();
    tabs_->selectTab(index, now);
    syncPageLabel();
}

void ReaderScreen::closeDoc(int index, double now) {
    if (index < 0 || index >= static_cast<int>(docs_.size())) return;
    const bool wasActive = (index == activeDoc_);
    if (wasActive) stashActiveDoc();
    const OpenDoc& closing = docs_[static_cast<std::size_t>(index)];
    state_.rememberDocument(closing.path, closing.pages, closing.page);
    auto closedSource = closing.source;
    docs_.erase(docs_.begin() + index);
    if (docs_.empty()) {
        activeDoc_ = -1;
        thumbs_->setSource(nullptr);
        stage_->setSource(nullptr, now);
        pageSource_.reset();
        annots_.clear();
        retireClosedSource(std::move(closedSource));
        if (onBackToWelcome) onBackToWelcome();
        return;
    }
    if (index < activeDoc_) --activeDoc_;
    if (wasActive) {
        const int next = std::min(index, static_cast<int>(docs_.size()) - 1);
        activeDoc_ = -1;            // force switchToDoc to do the work
        switchToDoc(next, now);
    }
    retireClosedSource(std::move(closedSource));
}

bool ReaderScreen::openFile(const std::string& path, double now) {
    return openPreparedFile(path, {}, now);
}

bool ReaderScreen::openPreparedFile(const std::string& path,
                                    std::shared_ptr<IPageSource> prepared,
                                    double now) {
    // Already open: switch rather than open a second copy.
    for (int i = 0; i < static_cast<int>(docs_.size()); ++i) {
        if (docs_[static_cast<std::size_t>(i)].path == path) {
            switchToDoc(i, now);
            return true;
        }
    }
    stashActiveDoc();
    if (!loadPath(path, now, std::move(prepared))) return false;
    OpenDoc d;
    d.path = path;
    d.title = docTitle_;
    d.source = pageSource_;
    d.pages = pageCount_;
    docs_.push_back(std::move(d));
    activeDoc_ = static_cast<int>(docs_.size()) - 1;
    tabs_->addTab(docTitle_, now);
    tabs_->selectTab(activeDoc_, now);
    edits_.clear();
    syncUndoButtons();
    return true;
}

namespace {
/// Decode UTF-8 into code points, ASCII-case-folded. Mirrors the folding the
/// engine applies to the page text so the two can be compared directly.
std::u32string foldUtf8(const std::string& s) {
    std::u32string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        char32_t cp = c;
        int extra = 0;
        if (c >= 0xF0)      { cp = c & 0x07u; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0Fu; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1Fu; extra = 1; }
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3Fu);
        if (cp >= U'A' && cp <= U'Z') cp += 32;
        out.push_back(cp);
    }
    return out;
}
}  // namespace

void ReaderScreen::runSearch(const std::string& q) {
    searchHits_.clear();
    searchIndex_ = -1;
    searchCursor_ = 0;
    searchNeedle_.clear();
    if (!stage_) return;
    stage_->clearSearchHits();
    if (q.size() < 2) { syncFindLabel(); return; }

#if FOLIO_HAVE_PDFIUM_SOURCE
    auto* pdf = dynamic_cast<folio::engine::PdfiumPageSource*>(pageSource_.get());
    if (pdf && pdf->pageCount() > 0) {
        searchNeedle_ = foldUtf8(q);
        // Queue only one page, not an unbounded copy of the text of a huge
        // document. pollExport() indexes completed pages incrementally.
        (void)pdf->pageText(0);
    }
#endif
    syncFindLabel();
}

void ReaderScreen::stepSearch(int delta) {
    if (searchHits_.empty() || !stage_) return;
    const int n = static_cast<int>(searchHits_.size());
    searchIndex_ = ((searchIndex_ + delta) % n + n) % n;
    stage_->setSearchHits(searchHits_, searchIndex_);
    scrollToHit(searchIndex_);
    syncFindLabel();
}

void ReaderScreen::scrollToHit(int i) {
    if (i < 0 || i >= static_cast<int>(searchHits_.size()) || !stage_) return;
    stage_->goToPage(searchHits_[static_cast<std::size_t>(i)].page + 1, now());
    syncPageLabel();
}

void ReaderScreen::syncFindLabel() {
    if (!findCount_) return;
    if (searchHits_.empty()) {
        findCount_->text = !searchNeedle_.empty() && searchCursor_ < pageCount_
                               ? std::string("Searching…")
                               : findField_ && findField_->text.size() >= 2
                                   ? std::string("No results") : std::string();
    } else {
        findCount_->text = std::to_string(searchIndex_ + 1) + " of " +
                           std::to_string(searchHits_.size());
    }
}

void ReaderScreen::commitPageField() {
    if (!pageField_) return;
    // Take the leading digits and ignore the rest, so "12 / 24" pasted back
    // in, or a stray keystroke, still does the obvious thing.
    int n = 0;
    bool any = false;
    for (char c : pageField_->text) {
        if (c >= '0' && c <= '9') { n = n * 10 + (c - '0'); any = true; }
        else if (any) break;
    }
    if (any && pageCount_ > 0) {
        const int target = std::clamp(n, 1, pageCount_);
        stage_->goToPage(target, now());
        // Echo the clamped value back while the field still has focus, so
        // typing 112 into a 15 page document visibly lands on 15 instead
        // of leaving 112 sitting there looking accepted.
        if (pageField_->focused) pageField_->text = std::to_string(target);
    }
    syncPageLabel();
}

void ReaderScreen::applyPageTheme(PageTheme theme) {
    state_.theme.setPageTheme(theme);
    stage_->setPageTheme(theme); // the inspector previously changed only the chip
    const int chosen = static_cast<int>(theme);
    for (std::size_t i = 0; i < pageThemeSw_.size(); ++i)
        pageThemeSw_[i]->toggled = static_cast<int>(i) == chosen;
    if (pageThemeSeg_ && pageThemeSeg_->selected() != chosen)
        pageThemeSeg_->setSelected(chosen, now());
}

void ReaderScreen::syncPageLabel() {
    const bool compact = breakpointFor(frame.w) == Breakpoint::Compact;
    pageLabel_ = std::to_string(stage_->currentPage()) +
                 (compact ? "/" : " / ") + std::to_string(pageCount_);
    // While the field has focus the user is mid-edit; overwriting their
    // keystrokes with the current page would be maddening.
    if (pageField_ && !pageField_->focused) pageField_->text = pageLabel_;
}

void ReaderScreen::handleOutsideClick(Point windowPoint, double now) {
    // The thumb palette is painted by PageStage, but a press on command-bar
    // chrome or a side panel never routes through PageStage::onPointer.
    stage_->dismissThumbIfOutside(windowPoint, now);
    findPop_->dismissIfOutside(windowPoint, now);
    zoomPop_->dismissIfOutside(windowPoint, now);
    if (layoutPop_->isOpen()) {
        layoutPop_->dismissIfOutside(windowPoint, now);
        if (!layoutPop_->isOpen()) viewBtn_->chevronOpen = false;
    }
    if (modePop_->isOpen()) {
        modePop_->dismissIfOutside(windowPoint, now);
        if (!modePop_->isOpen()) modeBtn_->chevronOpen = false;
    }
    displayPop_->dismissIfOutside(windowPoint, now);
    if (shapePop_) shapePop_->dismissIfOutside(windowPoint, now);
    if (toolOverflowPop_) toolOverflowPop_->dismissIfOutside(windowPoint, now);
    if (docPop_ && docPop_->isOpen()) {
        docPop_->dismissIfOutside(windowPoint, now);
        if (!docPop_->isOpen() && docBtn_) docBtn_->chevronOpen = false;
    }

    // A bottom sheet dismisses when you tap the page above it, the way
    // every phone sheet does. Without this the only way out is the small
    // chevron inside the sheet, and the sheet covers the button that
    // opened it.
    if (sheetMode()) {
        const auto outside = [&](const SidePanel* p) {
            if (p->collapsed() || !p->visible) return false;
            const Point o = p->toWindow({0.f, 0.f});
            return !(windowPoint.x >= o.x && windowPoint.x < o.x + p->frame.w &&
                     windowPoint.y >= o.y && windowPoint.y < o.y + p->frame.h);
        };
        if (outside(leftPanel_)) {
            state_.leftPanelCollapsed = true;
            leftPanel_->setCollapsed(true, now);
            panelLayoutDirty_ = true;
            syncPhoneFabs();
        }
        if (outside(rightPanel_)) {
            state_.rightPanelCollapsed = true;
            rightPanel_->setCollapsed(true, now);
            panelLayoutDirty_ = true;
            syncPhoneFabs();
        }
    }
    // Treat selection actions and their colour picker as one popup. Tapping
    // inside either keeps the selection actions visible; tapping elsewhere
    // dismisses both. A press on the page also clears the selection via the
    // stage's ordinary pointer handling.
    auto within = [windowPoint](const Widget* w) {
        const Point o = w->toWindow({0.f, 0.f});
        return windowPoint.x >= o.x && windowPoint.x < o.x + w->frame.w &&
               windowPoint.y >= o.y && windowPoint.y < o.y + w->frame.h;
    };
    if (selPop_ && selPop_->isOpen()) {
        const bool onSelection = within(selPop_);
        const bool onColour = toolColorPop_->isOpen() && within(toolColorPop_);
        if (!onSelection && !onColour) closeSelectionPopup();
        else if (onSelection && toolColorPop_->isOpen() && !onColour)
            toolColorPop_->close(now);
    } else if (toolColorPop_ && toolColorPop_->isOpen()) {
        toolColorPop_->dismissIfOutside(windowPoint, now);
    }
}

bool ReaderScreen::onKey(const KeyEvent& e) {
    if (!e.pressed) return false;
    if (state_.focusMode && focusControlsVisible_)
        focusHideAt_ = now() + kFocusIdleSeconds;
    const bool ctrl = (e.modifiers & ModCtrl) != 0;

    // A live free-text annotation owns the keyboard. It deliberately hands
    // back anything it does not consume - modifier combinations above all -
    // so Ctrl+S still saves while a text box is open.
    if (stage_->editingText() && stage_->handleTextKey(e)) {
        syncUndoButtons();
        return true;
    }
    if (ctrl && e.key == Key::F) {
        openPopoverFor(findPop_, searchBtn_, false);
        return true;
    }
    // Undo/redo. Ctrl+Shift+Z and Ctrl+Y are both redo - the first is the
    // Mac/Linux convention, the second is Windows, and users bring their
    // habits with them.
    if (ctrl && e.key == Key::Z) {
        if (e.modifiers & ModShift) doRedo(); else doUndo();
        return true;
    }
    if (ctrl && e.key == Key::Y) { doRedo(); return true; }

    // Delete removes the selected annotation. Backspace does the same: on a
    // laptop without a Delete key it is the only option.
    if ((e.key == Key::Delete || e.key == Key::Backspace) && stage_ &&
        stage_->selectedAnnot()) {
        stage_->deleteSelectedAnnot();
        return true;
    }

    if (ctrl && e.key == Key::O) { if (onBrowse) onBrowse(); return true; }
    if (ctrl && e.key == Key::W) { if (onBackToWelcome) onBackToWelcome(); return true; }
    if (ctrl && e.key == Key::C && stage_ && stage_->hasSelection()) {
        const std::string sel = stage_->selectedText();
        if (!sel.empty()) {
            if (onCopyText) onCopyText(sel);
            // Character count rather than a bare confirmation: it is the
            // one fact that tells the user the selection was what they
            // thought it was.
            OverlayHost::current().showToast(
                "Copied " + std::to_string(sel.size()) + " characters", now());
        }
        return true;
    }
    if (ctrl && e.key == Key::A && stage_) {
        stage_->selectAll();
        return true;
    }
    if (e.key == Key::Escape && stage_ && stage_->selectedAnnot()) {
        stage_->deselectAnnot();
        return true;
    }
    if (e.key == Key::Escape && stage_ && stage_->hasSelection()) {
        stage_->clearSelection();
        closeSelectionPopup();
        return true;
    }
    if (ctrl && (e.key == Key::Plus)) { stage_->setZoom(stage_->zoom() * 1.25f, now()); return true; }
    if (ctrl && (e.key == Key::Minus)) { stage_->setZoom(stage_->zoom() / 1.25f, now()); return true; }
    if (ctrl && (e.key == Key::Digit0)) { stage_->setZoomMode(ZoomMode::FitPage, now()); return true; }
    if (e.key == Key::F && !ctrl) {
        setFocusMode(!state_.focusMode);
        return true;
    }
    if (e.key == Key::Escape) {
        if (findPop_->isOpen())    { findPop_->close(now()); return true; }
        if (displayPop_->isOpen()) { displayPop_->close(now()); return true; }
        if (zoomPop_->isOpen()) { zoomPop_->close(now()); return true; }
        if (docPop_ && docPop_->isOpen()) { docPop_->close(now()); return true; }
        if (layoutPop_->isOpen()) { layoutPop_->close(now()); return true; }
        if (modePop_->isOpen()) { modePop_->close(now()); return true; }
        if (state_.focusMode) {
            setFocusMode(false);
            return true;
        }
        if (sheetMode() && (!leftPanel_->collapsed() || !rightPanel_->collapsed())) {
            state_.leftPanelCollapsed = state_.rightPanelCollapsed = true;
            leftPanel_->setCollapsed(true, now());
            rightPanel_->setCollapsed(true, now());
            panelLayoutDirty_ = true;
            syncPhoneFabs();
            return true;
        }
#if defined(__ANDROID__)
        if (onBackToWelcome) { onBackToWelcome(); return true; }
#endif
    }
    return stage_->onKey(e);
}

void ReaderScreen::onBreakpointChanged(const LayoutMetrics& m) {
    // Crossing into a narrow frame collapses both panels: a drawer that is
    // open by default covers the document the moment the window is
    // resized, which reads as the app breaking. Crossing back out restores
    // the left panel only - the right one is a property sheet and is more
    // useful on demand.
    if (m.panelsOverlay) {
        // Go through the same state the toggles read. Collapsing the
        // widget alone leaves state_.*PanelCollapsed saying "expanded",
        // and the next rail tap then flips it to collapsed - i.e. the
        // drawer refuses to open, exactly once, which is maddening.
        state_.leftPanelCollapsed = true;
        state_.rightPanelCollapsed = true;
        leftPanel_->setCollapsed(true, now());
        rightPanel_->setCollapsed(true, now());
        if (leftToggle_) leftToggle_->toggled = false;
        if (rightToggle_) rightToggle_->toggled = false;
    } else if (m.bp == Breakpoint::Expanded) {
        state_.leftPanelCollapsed = false;
        leftPanel_->setCollapsed(false, now());
        if (leftToggle_) leftToggle_->toggled = true;
    }

    // A drawer that paints under the page is invisible: the stage is added
    // to the tree after the panels, so it wins by default. Lifting the
    // panels a layer fixes that - and unlike the raiseToFront() this
    // replaces, it is reversible, so widening the window puts them back
    // down instead of leaving them stuck above every popover for the rest
    // of the session.
    const int panelLayer = m.panelsOverlay ? zlayer::panel : zlayer::content;
    leftPanel_->setZOrder(panelLayer);
    rightPanel_->setZOrder(panelLayer);

    // On a phone the panels stop being columns and become bottom sheets.
    // Two 48px rails floating over a 390px page cost a third of the width
    // while showing nothing, so the rails go and their jobs move to the
    // two bottom corners, within thumb reach.
    const auto pres = m.panelsOverlay ? SidePanel::Presentation::Sheet
                                      : SidePanel::Presentation::Column;
    leftPanel_->setPresentation(pres);
    rightPanel_->setPresentation(pres);
    const bool sheets = pres == SidePanel::Presentation::Sheet;
    // Rounded along the top edge only: the sheet reads as having slid up
    // from off-screen rather than as a card resting near the bottom.
    if (sheets) {
        const Corners c{tokens::radius::card, tokens::radius::card, 0.f, 0.f};
        leftPanel_->setCorners(c);
        rightPanel_->setCorners(c);
    } else {
        leftPanel_->clearCorners();
        rightPanel_->clearCorners();
    }
    syncPhoneFabs();

    // Labels cost width that a phone does not have.
    if (shareBtn_) shareBtn_->label = m.compactChrome ? "" : "Share";
    if (modeBtn_) modeBtn_->label = m.compactChrome ? "" : modeName(state_.mode);
    // No layoutTree() here: this runs *from* onLayout, and the rest of that
    // pass picks the new widths up anyway.
}

void ReaderScreen::onLayout() {
    const float W = frame.w, H = frame.h;
    const float tb = tokens::size::titleBar;
    // The prototype uses two different gaps and they are not the same
    // number: #cmd and #work sit 24px (gutter) from the window edge, while
    // #L/#R are 16px (panelGap) from the canvas between them.
    // Size class first: everything below is a consequence of it. The
    // panels are told their width here rather than reading a token, which
    // is what lets the same tree lay out on a phone and a desktop.
    const LayoutMetrics m = LayoutMetrics::forViewport(W, H);
    if (W != lastOptionLayoutWidth_) {
        lastOptionLayoutWidth_ = W;
        syncToolOptions();
    }
    if (m.bp != lastBreakpoint_) {
        lastBreakpoint_ = m.bp;
        onBreakpointChanged(m);
    }
    const float g = m.gutter;
    const float pg = m.panelGap;
    // A sheet spans the window, so its content lays out at that width
    // rather than at the column width the breakpoint would hand it.
    const bool sheet = sheetMode();
    const float sheetW = W - g * 2.f;
    leftPanel_->setExpandedWidth(sheet ? sheetW : m.leftPanelW, now());
    rightPanel_->setExpandedWidth(sheet ? sheetW : m.rightPanelW, now());

    // Focus mode slides the chrome away; `f` is 1 when fully focused.
    //
    // This used to read value(0.0). Animated::value treats a `now` before
    // the animation's start as "not begun yet" and hands back the *from*
    // value, so the layout was permanently pinned to whatever the chrome
    // offset had been before the toggle - focus mode moved nothing. The
    // only visible effect was the wordmark fading, because onPaint reads
    // the same animation with the real clock.
    const float f = focusT_.value(now());
    const float chromeY = lerp(0.f, -(tb + kCmdH + g), f);

    // --- title bar ---------------------------------------------------------
    // logo (16 + 17) + label + the prototype's 14px padding-right.
    // The window controls own one edge of the title bar; everything else is
    // laid out in the space they leave. On macOS that edge is the leading
    // one, so the wordmark and tabs slide right instead.
    winCtl_->setFrame(0.f, chromeY, W, tb);
    const float ctlW = winCtl_->visible ? winCtl_->preferredWidth() : 0.f;
    const bool ctlLeft = winCtl_->visible && winCtl_->controlsOnLeft();
    chromeLead_ = ctlLeft ? ctlW : 0.f;
    const float trail = ctlLeft ? 12.f : ctlW;

    // Search sits in the middle of the title bar, the way the prototype
    // has it and the way every browser does. Centred on the window, not on
    // the space left over by the tabs, or it would drift as tabs open.
    // Search moved to the command bar, so the tabs get the whole title bar
    // back and stop only for the window controls.
    // A tab strip needs real width before it is worth anything. On a phone
    // the title bar carries the document name instead, and tabs move to
    // nothing - the document is switched from the library.
    // 86 is measured past the "Folio" wordmark - do not shrink it on
    // narrow frames or the home button lands on top of the logotype.
    const float homeX = chromeLead_ + 86.f;
    const float tabsX = homeX + 38.f;
    const float listW = 30.f;
    const float tabsW = std::max(0.f, W - tabsX - trail - listW - 12.f);
    tabs_->setFrame(tabsX, chromeY, tabsW, tb);
    tabs_->visible = m.showTabs;
    docListBtn_->setFrame(tabsX + tabsW + 3.f, chromeY + (tb - 30.f) * .5f,
                          listW, 30.f);
    docListBtn_->visible = m.showTabs;
    backBtn_->setFrame(homeX, chromeY + (tb - 28.f) * 0.5f, 28.f, 28.f);
    layOutPhoneChrome(m, W, H, chromeY, tb);

    // One compact exit icon replaces the persistent text pill. When the
    // reader hides it, the same top corner becomes an invisible 44dp wake
    // zone for a phone (Back/Escape also always exits Focus).
    if (focusExitBtn_) {
        const float sz = 44.f;
        focusExitBtn_->setFrame(W - g - sz, lerp(-sz - 8.f, g, f), sz, sz);
        focusExitBtn_->visible = state_.focusMode && focusControlsVisible_;
        focusExitBtn_->label.clear();
    }
    if (focusWakeZone_) {
        focusWakeZone_->setFrame(W - g - 44.f, g, 44.f, 44.f);
        focusWakeZone_->visible = m.panelsOverlay && state_.focusMode &&
                                  !focusControlsVisible_;
    }

    // --- command bar -------------------------------------------------------
    // Prototype: #cmd{top:52px}, i.e. 8px below the 44px title bar.
    const float cmdY = chromeY + tb + 8.f;
    cmdBar_->setFrame(g, cmdY, W - g * 2.f, kCmdH);
    {
        const float pad = 8.f;
        const float ih = 30.f;
        const float iy = (kCmdH - ih) * 0.5f;
        float x = pad;

        // Nothing here may overflow: a command bar that runs off the right
        // edge of a phone hides controls with no way to reach them. Each
        // cluster is dropped only where another affordance already covers
        // it, so no function becomes unreachable.
        const bool tiny = m.bp == Breakpoint::Compact;
        const float kModeW = m.compactChrome ? 38.f : 118.f;
        modeBtn_->setFrame(x, iy, kModeW, ih);
        x += kModeW + (m.compactChrome ? 8.f : 12.f);

        // A phone only keeps Open PDF in Read mode; the Annotate/Edit tool
        // strip needs that slot. The document menu/library still offers Open
        // in those modes. Desktop keeps its normal full file-action row.
        for (std::size_t i = 0; i < cmdItems_.size(); ++i) {
            Widget* w = cmdItems_[i];
            w->setFrame(x, iy, ih, ih);
            w->visible = !m.compactChrome || i == 1 || (i == 0 && state_.mode == Mode::Read);
            if (w->visible) x += ih + 2.f;
        }
        if (!m.compactChrome) x += 10.f;

        // One authoritative allocation pass: every button consumes space
        // from the right only if it leaves the left mode/actions and one
        // tool-dropdown slot untouched. Selected marks always put Hand and
        // Select in that dropdown instead of trying to keep two competing
        // tool strips beside the annotation property controls.
        const float step = ih + 2.f;
        const float reserve = toolBtns_.empty() ? 0.f : step + 6.f;
        float rx = cmdBar_->frame.w - pad;
        const float shareW = m.compactChrome ? ih : 94.f;
        rx -= shareW;
        shareBtn_->setFrame(rx, iy, shareW, ih);
        shareBtn_->visible = true;

        const bool showFocus = !m.compactChrome || state_.mode == Mode::Read;
        const bool showDisplay = !m.panelsOverlay;
        rightToggle_->visible = leftToggle_->visible = !tiny;
        displayBtn_->visible = showDisplay;
        if (!tiny) {
            if (showFocus && rx - ih - 6.f >= x + reserve) {
                rx -= ih + 6.f;
                focusBtn_->setFrame(rx, iy, ih, ih);
                focusBtn_->visible = true;
            } else focusBtn_->visible = false;
            rx -= ih + 2.f; rightToggle_->setFrame(rx, iy, ih, ih);
            rx -= ih + 2.f; leftToggle_->setFrame(rx, iy, ih, ih);
            if (showDisplay && rx - ih - 8.f >= x + reserve) {
                rx -= ih + 8.f; displayBtn_->setFrame(rx, iy, ih, ih);
            } else displayBtn_->visible = false;
        } else {
            if (showFocus && rx - ih - 4.f >= x + reserve + ih + 2.f) {
                rx -= ih + 4.f;
                focusBtn_->setFrame(rx, iy, ih, ih);
                focusBtn_->visible = true;
            } else focusBtn_->visible = false;
        }
        const bool wantsSearch = !(m.panelsOverlay && state_.mode != Mode::Read);
        searchBtn_->visible = wantsSearch && rx - ih - 2.f >= x + reserve;
        if (!searchBtn_->visible && findPop_->isOpen()) findPop_->close(now());
        if (searchBtn_->visible) {
            rx -= ih + 2.f;
            searchBtn_->setFrame(rx, iy, ih, ih);
        }

        // An option that cannot fit remains in the inspector sheet. No
        // negative-width tool host, painted overlap or invisible hit target.
        auto placeOption = [&](Button* b, float gap) {
            if (!b || !b->visible) return;
            if (rx - ih - gap < x + reserve) {
                b->visible = false;
                return;
            }
            rx -= ih + gap;
            b->setFrame(rx, iy, ih, ih);
        };
        placeOption(toolOptColor_, 8.f);
        placeOption(toolOptSize_, 2.f);
        placeOption(toolOptFont_, 2.f);
        placeOption(toolOptOpacity_, 2.f);

        const std::size_t total = toolBtns_.size();
        const bool editingMark = selectedAnnot_ && annots_.get(selectedAnnot_);
        const float available = std::max(0.f, rx - 6.f - x);
        std::size_t shown = editingMark ? 0 :
            std::min(total, static_cast<std::size_t>(available / step));
        if (shown < total && shown > 0) --shown; // the dropdown owns a slot
        toolHost_->setFrame(x, iy, static_cast<float>(shown) * step, ih);
        for (std::size_t i = 0; i < total; ++i) {
            toolBtns_[i]->setFrame(static_cast<float>(i) * step, 0.f, ih, ih);
            toolBtns_[i]->visible = i < shown;
        }
        toolOverflowBtn_->visible = shown < total && available >= step;
        if (toolOverflowBtn_->visible)
            toolOverflowBtn_->setFrame(x + static_cast<float>(shown) * step,
                                       iy, ih, ih);
    }

    // --- work area ---------------------------------------------------------
    // Prototype: #work{top:112px;bottom:24px} - a 12px gap under the
    // command bar, not the 24px panel gap.
    const float workY = cmdY + kCmdH + 12.f;
    const float workH = H - workY - g;
    const float lw = leftPanel_->currentWidth(now());
    const float rw = rightPanel_->currentWidth(now());
    const float lwF = lerp(lw, 0.f, f);
    const float rwF = lerp(rw, 0.f, f);
    const float lhF = leftPanel_->currentHeight(now(), workH);
    const float rhF = rightPanel_->currentHeight(now(), workH);
    const float lRail = leftPanel_->railAlpha(now());
    const float rRail = rightPanel_->railAlpha(now());
    // Body opacity tracks the panel's geometry, not the rail's delayed
    // curve - see SidePanel::contentAlpha.
    const float lBody = leftPanel_->contentAlpha(now());

    constexpr float kPad = 14.f;      // .pf padding
    constexpr float kRowH = 28.f;

    // Sheet heights. Pages/outline is a browsing task and wants room;
    // the inspector is a short list of controls and should leave as much
    // of the page visible as it can.
    const float lSheetH = H < 560.f ? std::max(0.f, H - 58.f)
                                   : std::min(workH, H * 0.70f);
    const float rSheetH = H < 560.f ? std::max(0.f, H - 58.f)
                                   : std::min(workH, H * 0.52f);
    // The sheet's actual height, not the column's animated work height,
    // bounds its scroll panes and bottom actions. Keep the Android gesture
    // navigation strip free without clipping the final row.
    const float sheetBottomReserve = sheet ? 22.f : 0.f;
    const float leftBodyH = sheet ? lSheetH : lhF;

    // ---- left panel internals (prototype .pf / .pane geometry) -----------
    if (sheet) {
        const float p = leftPanel_->sheetProgress(now());
        leftPanel_->setFrame(g, H - lSheetH * p, sheetW, lSheetH);
        leftPanel_->visible = p > 0.002f;
    } else {
        leftPanel_->setFrame(g, workY, lwF, lhF);
        leftPanel_->visible = lwF > 2.f;
    }
    {
        // Lay the content out at the panel's *expanded* width, not the
        // animated one. layoutTree() is invalidation-driven, not per
        // frame, so content laid out while the panel was a 48px rail
        // (innerW = 20) never reflowed when the panel slid open - it just
        // stayed crammed. The panel clips its content, so laying it out
        // full-size and letting the clip reveal it is both correct and
        // what makes the reveal animation cheap.
        const float innerW = std::max(lwF, leftPanel_->expandedWidth()) -
                             kPad * 2.f;
        leftTabs_->setFrame(kPad, kPad, std::max(0.f, innerW), 34.f);
        leftTabs_->visible = lBody > 0.01f && innerW > 40.f;
        leftTabs_->opacity = lBody;

        // .pan row: page count on the left, collapse chevron on the right.
        leftCollapse_->setFrame(lwF - kPad - kRowH, 60.f, kRowH, kRowH);
        leftCollapse_->visible = leftTabs_->visible;
        leftCollapse_->opacity = lBody;

        // .pane : top 102, bottom 14. The action row sits at its foot.
        const float paneY = 102.f;
        const float actH = 30.f;
        const float paneH = std::max(0.f, leftBodyH - sheetBottomReserve -
                                     paneY - kPad - actH - 8.f);
        thumbs_->setFrame(kPad, paneY, std::max(0.f, innerW), paneH);
        thumbs_->visible = leftTabs_->visible && state_.leftTab == 0;
        thumbs_->opacity = lBody;
        if (!thumbs_->visible && pageSource_)
            pageSource_->setVisibleThumbnailRange(-1, -1);

        outline_->setFrame(kPad, paneY, std::max(0.f, innerW),
                           std::max(0.f, leftBodyH - sheetBottomReserve - paneY - kPad));
        outline_->visible = leftTabs_->visible && state_.leftTab == 1;
        outline_->opacity = lBody;
        notes_->setFrame(kPad, paneY, std::max(0.f, innerW),
                         std::max(0.f, leftBodyH - sheetBottomReserve - paneY - kPad));
        notes_->visible = leftTabs_->visible && state_.leftTab == 2;
        notes_->opacity = lBody;
        layersScroll_->setFrame(kPad, paneY, std::max(0.f, innerW),
                                std::max(0.f, leftBodyH - sheetBottomReserve - paneY - kPad));
        layersScroll_->visible = leftTabs_->visible && state_.leftTab == 3;
        layersScroll_->opacity = lBody;
        const float layerH = 50.f + static_cast<float>(layerItems_.size()) * 43.f;
        layersScroll_->contentHeight = layerH;
        layersContent_->setFrame(0.f, 0.f, std::max(0.f, innerW), layerH);
        for (std::size_t i = 0; i < layerItems_.size(); ++i)
            layerItems_[i]->setFrame(2.f, 36.f + static_cast<float>(i) * 43.f,
                                    std::max(0.f, innerW - 8.f), 39.f);

        const float aw = innerW / 4.f;
        for (std::size_t i = 0; i < pageActions_.size(); ++i) {
            pageActions_[i]->setFrame(kPad + aw * static_cast<float>(i),
                                      leftBodyH - sheetBottomReserve - kPad - actH,
                                      aw, actH);
            pageActions_[i]->visible = thumbs_->visible;
            pageActions_[i]->opacity = lBody;
        }

        // Rail: a 48px column of icon buttons, 7px/6px padding, 4px gap.
        for (std::size_t i = 0; i < leftRail_.size(); ++i) {
            leftRail_[i]->setFrame(7.f, 7.f + static_cast<float>(i) * 38.f, 34.f, 34.f);
            leftRail_[i]->visible = lRail > 0.01f;
            leftRail_[i]->opacity = lRail;
        }
    }

    // ---- right panel internals -------------------------------------------
    if (sheet) {
        const float p = rightPanel_->sheetProgress(now());
        rightPanel_->setFrame(g, H - rSheetH * p, sheetW, rSheetH);
        rightPanel_->visible = p > 0.002f;
    } else {
        rightPanel_->setFrame(W - g - rwF, workY, rwF, rhF);
        rightPanel_->visible = rwF > 2.f;
    }
    {
        const float innerW = std::max(rwF, rightPanel_->expandedWidth()) -
                             kPad * 2.f;
        const float rBody = rightPanel_->contentAlpha(now());
        rightCollapse_->setFrame(rwF - kPad - kRowH, kPad, kRowH, kRowH);
        rightCollapse_->visible = rBody > 0.01f && innerW > 40.f;
        rightCollapse_->opacity = 1.f;
        focusPanelBtn_->setFrame(rwF - kPad - kRowH * 2.f - 6.f,
                                  kPad, kRowH, kRowH);
        focusPanelBtn_->visible = rightCollapse_->visible;
        focusPanelBtn_->opacity = rBody;
        focusPanelBtn_->toggled = state_.focusMode;
        undoPanelBtn_->setFrame(kPad, kPad - 3.f, 34.f, 34.f);
        redoPanelBtn_->setFrame(kPad + 38.f, kPad - 3.f, 34.f, 34.f);
        undoPanelBtn_->visible = redoPanelBtn_->visible = rightCollapse_->visible;
        undoPanelBtn_->opacity = redoPanelBtn_->opacity = rBody;
        // Fit six actions without letting rotate arrows collide with Focus
        // and Collapse on narrow desktop panels or phone sheets. Above the
        // threshold the row stays compact; below it rotation gets its own
        // 34dp row and the scroll content begins BELOW that row.
        const bool twoHeaderRows = rightPanel_->expandedWidth() < 330.f;
        const float rotateY = twoHeaderRows ? kPad + 34.f : kPad - 3.f;
        rotateLeftBtn_->setFrame(kPad + (twoHeaderRows ? 0.f : 79.f),
                                 rotateY, 34.f, 34.f);
        rotateRightBtn_->setFrame(kPad + (twoHeaderRows ? 38.f : 117.f),
                                  rotateY, 34.f, 34.f);
        rotateLeftBtn_->visible = rotateRightBtn_->visible = rightCollapse_->visible;
        rotateLeftBtn_->opacity = rotateRightBtn_->opacity = rBody;

        const bool authoring = stylingActive();
        const bool bodyVisible = rightCollapse_->visible;

        // Two stacks occupy the same space: tool properties while an
        // annotation tool is active, page-display options otherwise.
        float y = 22.f;

        annotHdr_->setFrame(kPad, twoHeaderRows ? 82.f : 50.f,
                            std::max(0.f, innerW - 38.f), 16.f);
        // Below the Focus/collapse row; never put Delete over either icon.
        deleteAnnotBtn_->setFrame(std::max(0.f, rwF - kPad - 34.f),
                                  twoHeaderRows ? 84.f : 52.f, 34.f, 34.f);
        deleteAnnotBtn_->visible = selectedAnnot_ != 0 &&
                                   annots_.get(selectedAnnot_) && bodyVisible;
        deleteAnnotBtn_->opacity = rBody;
        annotHdr_->visible = authoring && bodyVisible;
        annotHdr_->opacity = rBody;

        float inspectorTop = twoHeaderRows ? (authoring ? 106.f : 86.f)
                                           : (authoring ? 75.f : 54.f);
        if (deleteAnnotBtn_->visible)
            inspectorTop = std::max(inspectorTop, twoHeaderRows ? 126.f : 95.f);
        const float panelH = sheet ? rSheetH : rhF;
        inspectorScroll_->setFrame(0.f, inspectorTop,
                                   std::max(rwF, rightPanel_->expandedWidth()),
                                   std::max(0.f, panelH - inspectorTop - kPad - sheetBottomReserve));
        inspectorScroll_->visible = bodyVisible;
        inspectorScroll_->opacity = rBody;

        // Four rows of six, sized from the panel rather than pinned at
        // 32px - the panel is narrower at Medium and wider as a phone
        // drawer, and a fixed cell would overflow one and strand the other.
        const float swGap = 5.f;
        const float swSize = std::max(
            12.f, (innerW - swGap * static_cast<float>(kInkCols - 1)) /
                      static_cast<float>(kInkCols));
        const int inkRows = (static_cast<int>(annotColorSw_.size()) + kInkCols - 1) /
                            kInkCols;
        const bool commentable = selectedAnnot_ && annots_.get(selectedAnnot_) &&
                                 !annots_.get(selectedAnnot_)->deleted;
        commentLabel_->visible = commentField_->visible =
            saveCommentBtn_->visible = commentable && bodyVisible;
        commentLabel_->setFrame(kPad, y, innerW, 18.f);
        const float commentH = std::clamp(
            108.f + static_cast<float>(commentField_->text.size() / 42) * 21.f,
            108.f, 218.f);
        commentField_->setFrame(kPad, y + 22.f, std::max(0.f, innerW), commentH);
        saveCommentBtn_->setFrame(kPad, y + 26.f + commentH,
                                   std::max(0.f, innerW), 34.f);
        if (commentable) y += commentH + 80.f;
        for (std::size_t i = 0; i < annotColorSw_.size(); ++i) {
            const int col = static_cast<int>(i) % kInkCols;
            const int row = static_cast<int>(i) / kInkCols;
            annotColorSw_[i]->setFrame(
                kPad + static_cast<float>(col) * (swSize + swGap),
                y + static_cast<float>(row) * (swSize + swGap), swSize, swSize);
            annotColorSw_[i]->visible = authoring && bodyVisible;
            annotColorSw_[i]->opacity = 1.f;
        }
        const float inkH =
            static_cast<float>(inkRows) * (swSize + swGap) - swGap;
        const bool textTool = authoring && stylingText();
        const float widthY = y + inkH + 20.f + 22.f;
        annotWidthSeg_->setFrame(kPad, widthY, std::max(0.f, innerW), 32.f);
        annotWidthSeg_->visible = authoring && !textTool && bodyVisible;
        annotWidthSeg_->opacity = 1.f;

        // FONT and SIZE take the WIDTH slot, then push OPACITY down.
        textFontSeg_->setFrame(kPad, widthY, std::max(0.f, innerW), 32.f);
        textFontSeg_->visible = textTool && bodyVisible;
        textFontSeg_->opacity = 1.f;
        const float sizeY = widthY + 32.f + 20.f + 22.f;
        textSizeSeg_->setFrame(kPad, sizeY, std::max(0.f, innerW), 32.f);
        textSizeSeg_->visible = textTool && bodyVisible;
        textSizeSeg_->opacity = 1.f;

        const float opacY = (textTool ? sizeY : widthY) + 32.f + 20.f + 22.f;
        annotOpacity_->setFrame(kPad, opacY, std::max(0.f, innerW), 22.f);
        annotOpacity_->visible = authoring && bodyVisible;
        annotOpacity_->opacity = 1.f;

        // .rp starts at top 60. "Page theme" header, then 44px swatches.
        const float swY = y;
        for (std::size_t i = 0; i < pageThemeSw_.size(); ++i) {
            pageThemeSw_[i]->setFrame(kPad + static_cast<float>(i) * 54.f, swY, 44.f, 44.f);
            pageThemeSw_[i]->visible = !authoring && bodyVisible;
            pageThemeSw_[i]->opacity = 1.f;
        }

        const float laySegY = authoring ? opacY + 22.f + 20.f + 22.f
                                        : swY + 44.f + 20.f + 22.f;
        layoutSegR_->setFrame(kPad, laySegY, std::max(0.f, innerW), 34.f);
        layoutSegR_->visible = rightCollapse_->visible;
        layoutSegR_->opacity = 1.f;

        const float autoHideY = laySegY + 34.f + (authoring ? 22.f : 44.f);
        focusAutoHideBtn_->setFrame(kPad, autoHideY,
                                     std::max(0.f, innerW), 42.f);
        focusAutoHideBtn_->visible = bodyVisible;
        focusAutoHideBtn_->opacity = 1.f;

        selectionMagnifierBtn_->setFrame(kPad, autoHideY + 49.f,
                                         std::max(0.f, innerW), 42.f);
        selectionMagnifierBtn_->visible = bodyVisible;
        selectionMagnifierBtn_->opacity = 1.f;

        // These are content coordinates, not screen coordinates. Both the
        // touch-scrollable viewport and wheel use the same clamped range.
        inspectorScroll_->contentHeight = autoHideY + 49.f + 42.f + 20.f;
        inspectorScroll_->setScrollY(inspectorScroll_->scrollY());
        inspectorContent_->setFrame(0.f, -inspectorScroll_->scrollY(),
                                    inspectorScroll_->frame.w,
                                    inspectorScroll_->contentHeight);

        for (std::size_t i = 0; i < rightRail_.size(); ++i) {
            rightRail_[i]->setFrame(rwF - 41.f, 7.f + static_cast<float>(i) * 38.f,
                                    34.f, 34.f);
            rightRail_[i]->visible = rRail > 0.01f;
            rightRail_[i]->opacity = rRail;
        }
    }

    // Overlay mode: the drawers float above the document rather than
    // squeezing it. At 390px there is no third column to give them, and a
    // 40px-wide page is not a reading experience.
    const float stageX = m.panelsOverlay ? g : g + lwF + (lwF > 2.f ? pg : 0.f);
    const float stageR = m.panelsOverlay ? g : g + rwF + (rwF > 2.f ? pg : 0.f);
    stage_->setFrame(stageX, lerp(workY, g, f), std::max(80.f, W - stageX - stageR),
                     lerp(workH, H - g * 2.f, f));

    // --- floating pill -----------------------------------------------------
    {
        const float ih = 28.f;
        // Phone: the fit buttons go (the layout popup has both fit modes)
        // and the layout button becomes an icon, which is the difference
        // between a pill that fits in 390px and one that runs off-screen.
        const bool tinyPill = m.bp == Breakpoint::Compact;
        const float edge = tinyPill ? 8.f : 14.f;
        const float pageLblW = tinyPill ? 58.f : 54.f;
        const float zoomLblW = tinyPill ? 44.f : 48.f;
        const float viewW = tinyPill ? ih : 132.f;
        float w = edge;
        w += ih * 2.f + 4.f;               // prev / next
        w += pageLblW;
        w += tinyPill ? 6.f : 12.f;        // divider
        w += ih * 2.f + 4.f;               // zoom out / in
        w += zoomLblW;
        if (!tinyPill) w += ih * 2.f + 4.f;  // fit width / page
        w += tinyPill ? 6.f : 12.f;        // divider
        w += viewW;
        w += edge;

        const float px = (W - w) * 0.5f;
        const float py = H - kPillH - 18.f;
        pill_->setFrame(px, py, w, kPillH);

        const float iy = (kPillH - ih) * 0.5f;
        float x = edge;
        prevBtn_->setFrame(x, iy, ih, ih); x += ih + 2.f;
        nextBtn_->setFrame(x, iy, ih, ih); x += ih + 2.f;
        pageLabelRect_ = Rect{px + x, py, pageLblW, kPillH};
        // Pill-local, inset so the hover well does not touch the pill edge.
        pageField_->setFrame(x, 4.f, pageLblW, kPillH - 8.f);
        x += pageLblW + (tinyPill ? 6.f : 12.f);
        zoomOutBtn_->setFrame(x, iy, ih, ih); x += ih + 2.f;
        zoomLabelRect_ = Rect{px + x, py, zoomLblW, kPillH};
        zoomPctBtn_->setFrame(x, iy, zoomLblW, ih);
        x += zoomLblW;
        zoomInBtn_->setFrame(x, iy, ih, ih); x += ih + 2.f;
        fitWidthBtn_->visible = !tinyPill;
        fitPageBtn_->visible = !tinyPill;
        if (!tinyPill) {
            fitWidthBtn_->setFrame(x, iy, ih, ih); x += ih + 2.f;
            fitPageBtn_->setFrame(x, iy, ih, ih); x += ih + 2.f;
        }
        x += tinyPill ? 6.f : 12.f;
        viewBtn_->setFrame(x, iy - 1.f, viewW, ih + 2.f);
        viewBtn_->label = tinyPill ? "" : viewLabelFull_;
        viewBtn_->trailingChevron = !tinyPill;

        // Phone: an open drawer owns the screen. Leaving the pill up means
        // its labels - which are painted in the screen's overlay pass,
        // after every child - bleed through the drawer on top of the page
        // action row.
        const bool drawerOpen =
            m.panelsOverlay &&
            (!state_.leftPanelCollapsed || !state_.rightPanelCollapsed);
        pill_->visible = !drawerOpen &&
                         (!state_.focusMode || focusControlsVisible_);
    }

    // --- popover contents --------------------------------------------------
    {
        const float rowH = 36.f;
        for (std::size_t i = 0; i < modeItems_.size(); ++i)
            modeItems_[i]->setFrame(6.f, 6.f + static_cast<float>(i) * rowH,
                                    modePop_->frame.w - 12.f, rowH);
        for (std::size_t i = 0; i < toolOverflowItems_.size(); ++i)
            toolOverflowItems_[i]->setFrame(6.f, 6.f + static_cast<float>(i) * 40.f,
                                           toolOverflowPop_->frame.w - 12.f, 40.f);
        for (std::size_t i = 0; i < shapeItems_.size(); ++i)
            shapeItems_[i]->setFrame(6.f + static_cast<float>(i) * 46.f,
                                    7.f, 42.f, 42.f);
        const float lrH = 42.f;
        for (std::size_t i = 0; i < layoutItems_.size(); ++i)
            layoutItems_[i]->setFrame(6.f, 6.f + static_cast<float>(i) * lrH,
                                      layoutPop_->frame.w - 12.f, lrH);

        // Tool colour: two rows of six chips.
        if (toolColorPop_) {
            constexpr float cell = 32.f, gap = 2.f, edge = 8.f;
            const auto rows = static_cast<float>(
                (toolColorItems_.size() + kInkCols - 1) / kInkCols);
            for (std::size_t i = 0; i < toolColorItems_.size(); ++i) {
                const auto col = static_cast<float>(i % kInkCols);
                const auto row = static_cast<float>(i / kInkCols);
                toolColorItems_[i]->setFrame(edge + col * (cell + gap),
                                             edge + row * (cell + gap),
                                             cell, cell);
            }
            toolColorPop_->setFrame(
                toolColorPop_->frame.x, toolColorPop_->frame.y,
                edge * 2.f + static_cast<float>(kInkCols) * cell +
                    static_cast<float>(kInkCols - 1) * gap,
                edge * 2.f + rows * cell + (rows - 1.f) * gap);
        }
        // Tool weight / text size: a short list, length depends on tool.
        if (toolSizePop_) {
            constexpr float rh = 36.f, edge = 6.f;
            float y = edge;
            for (Button* b : toolSizeItems_) {
                if (!b->visible) continue;
                b->setFrame(edge, y, toolSizePop_->frame.w - edge * 2.f, rh);
                y += rh;
            }
            toolSizePop_->setFrame(toolSizePop_->frame.x, toolSizePop_->frame.y,
                                   toolSizePop_->frame.w, y + edge);
        }
        // Opacity: a row of presets over a slider.
        if (toolOpacityPop_) {
            constexpr float edge = 10.f, bh = 28.f, gap = 4.f;
            const float w = toolOpacityPop_->frame.w - edge * 2.f;
            const float bw = (w - gap * 3.f) / 4.f;
            for (std::size_t i = 0; i < toolOpacityItems_.size(); ++i)
                toolOpacityItems_[i]->setFrame(
                    edge + static_cast<float>(i) * (bw + gap), edge, bw, bh);
            if (toolOpacitySlider_)
                toolOpacitySlider_->setFrame(edge, edge + bh + 14.f, w, 20.f);
            toolOpacityPop_->setFrame(toolOpacityPop_->frame.x,
                                      toolOpacityPop_->frame.y,
                                      toolOpacityPop_->frame.w,
                                      edge * 2.f + bh + 14.f + 20.f);
        }
        // Font family.
        if (toolFontPop_) {
            constexpr float rh = 36.f, edge = 6.f;
            for (std::size_t i = 0; i < toolFontItems_.size(); ++i)
                toolFontItems_[i]->setFrame(edge,
                                            edge + static_cast<float>(i) * rh,
                                            toolFontPop_->frame.w - edge * 2.f, rh);
            toolFontPop_->setFrame(toolFontPop_->frame.x, toolFontPop_->frame.y,
                                   toolFontPop_->frame.w,
                                   edge * 2.f + 3.f * rh);
        }
    }
    {
        const float pad = tokens::space::pad;
        const float btn = 26.f;
        const float row = pad + 34.f + 8.f;
        findCount_->setFrame(pad, row, 150.f, btn);
        findNext_->setFrame(findPop_->frame.w - pad - btn, row, btn, btn);
        findPrev_->setFrame(findPop_->frame.w - pad - btn * 2.f - 4.f, row, btn,
                            btn);
    }
    findField_->setFrame(tokens::space::pad, tokens::space::pad,
                         findPop_->frame.w - tokens::space::pad * 2.f,
                         tokens::size::field + 4.f);
    if (selPop_) {
        // A single row of equal cells. Equal widths matter here: the labels
        // are different lengths, and proportional cells make the row look
        // accidental.
        const float pad = 5.f;
        const float n = static_cast<float>(selBtns_.size());
        const float cw = (kSelPopW - pad * 2.f - 4.f * (n - 1.f)) / n;
        float x = pad;
        for (Button* b : selBtns_) {
            b->setFrame(x, pad, cw, kSelPopH - pad * 2.f);
            x += cw + 4.f;
        }
    }
    {
        for (std::size_t i = 0; i < zoomPresetItems_.size(); ++i)
            zoomPresetItems_[i]->setFrame(6.f + (i % 2) * 107.f,
                                          6.f + (i / 2) * 42.f, 107.f, 42.f);
    }
    {
        const float pad = tokens::space::pad;
        const float iw = displayPop_->frame.w - pad * 2.f;
        float y = pad + 20.f;
        pageThemeSeg_->setFrame(pad, y, iw, tokens::size::field + 4.f);
        y += tokens::size::field + 4.f + 28.f;
        const float half = (iw - 8.f) * 0.5f;
        dayBtn_->setFrame(pad, y, half, tokens::size::field + 4.f);
        nightBtn_->setFrame(pad + half + 8.f, y, half, tokens::size::field + 4.f);
        y += tokens::size::field + 4.f + 28.f;
        gridSlider_->setFrame(pad, y, iw, 22.f);
    }
}

void ReaderScreen::syncFocusAutoHidePreference() {
    if (focusAutoHideBtn_)
        focusAutoHideBtn_->toggled = state_.autoHideFocusControls;
    if (selectionMagnifierBtn_)
        selectionMagnifierBtn_->toggled = state_.selectionMagnifier;
    if (stage_) stage_->setSelectionMagnifierEnabled(state_.selectionMagnifier);
}

void ReaderScreen::noteFocusInteraction(const PointerEvent& e) {
    if (!state_.focusMode) return;
    if (e.isTouch()) {
        if (e.phase == PointerPhase::Down) focusTouches_.insert(e.pointerId);
        else if (e.phase == PointerPhase::Cancel) focusTouches_.clear();
        else if (e.phase == PointerPhase::Up) focusTouches_.erase(e.pointerId);
        // Do not treat hover/enter as activity: a resting page is idle.
        if (e.phase != PointerPhase::Down && e.phase != PointerPhase::Move &&
            e.phase != PointerPhase::Up && e.phase != PointerPhase::Cancel) return;
    } else if (e.phase != PointerPhase::Down && e.phase != PointerPhase::Up &&
               e.phase != PointerPhase::Wheel) {
        return;
    }
    if (focusControlsVisible_) focusHideAt_ = e.timestamp + kFocusIdleSeconds;
}

void ReaderScreen::showFocusControls(bool show) {
    if (!state_.focusMode) return;
    focusControlsVisible_ = show;
    if (show) focusHideAt_ = now() + kFocusIdleSeconds;
    if (focusExitBtn_) focusExitBtn_->visible = show;
    stage_->setFocusScrollbarsVisible(show);
    layoutTree();
}

void ReaderScreen::toggleFocusControlsFromPage() {
    if (!state_.focusMode) return;
    // In editing modes a tap can select a mark: do not hide the controls as
    // a side effect. When hidden, a plain tap still reveals a way out.
    if (state_.mode != Mode::Read && focusControlsVisible_) return;
    showFocusControls(!focusControlsVisible_);
}

void ReaderScreen::setFocusMode(bool on) {
    state_.focusMode = on;
    focusControlsVisible_ = on;
    focusTouches_.clear();
    focusHideAt_ = on ? now() + kFocusIdleSeconds : 0.0;
    if (on && sheetMode()) {
        state_.leftPanelCollapsed = state_.rightPanelCollapsed = true;
        leftPanel_->setCollapsed(true, now());
        rightPanel_->setCollapsed(true, now());
        syncPhoneFabs();
    }
    if (focusBtn_) {
        focusBtn_->toggled = on;
        focusBtn_->icon = on ? "minimise-view" : "maximise-view";
    }
    if (focusPanelBtn_) focusPanelBtn_->toggled = on;
    if (focusExitBtn_) focusExitBtn_->visible = on;
    stage_->setFocusScrollbarsVisible(!on || focusControlsVisible_);
    layoutTree();
}

bool ReaderScreen::animating(double now) const {
    return focusT_.active(now) ||
           (state_.focusMode && state_.autoHideFocusControls &&
            focusControlsVisible_ && focusTouches_.empty() &&
            now >= focusHideAt_);
}

bool ReaderScreen::requiresFullFrame(double now) const {
    return stage_->geometryChanged() || layoutWasAnimating_ || focusT_.active(now) ||
           leftPanel_->animating(now) || rightPanel_->animating(now) ||
           tabs_->animating(now) ||
           (stage_->animating(now) && !stage_->thumbOverlayOnly(now));
}

void ReaderScreen::syncNotes() {
    if (!notes_ || notesRevision_ == annots_.revision()) return;
    notesRevision_ = annots_.revision();
    static constexpr const char* kNames[] = {
        "Highlight", "Underline", "Strikeout", "Ink", "Note", "Box",
        "Ellipse", "Text", "Stamp", "Signature", "Line", "Arrow"};
    std::vector<OutlineList::Entry> rows;
    noteIds_.clear();
    pdfNoteRefs_.clear();
    std::unordered_set<std::uint64_t> seenGroups;
    for (const auto& a : annots_.all()) {
        if (a.deleted || (a.kind != AnnotKind::Highlight && a.kind != AnnotKind::Note &&
            a.comment.empty() && a.kind != AnnotKind::Text)) continue;
        // A multiline highlight is one note, not one row per glyph line.
        if (a.group && !seenGroups.insert(a.group).second) continue;
        std::string title = kNames[static_cast<std::size_t>(a.kind)];
        const std::string& body = a.comment.empty() ? a.text : a.comment;
        if (!body.empty()) title += " · " + body.substr(0, 80);
        rows.push_back({std::move(title), a.page, 0});
        noteIds_.push_back(a.id);
        pdfNoteRefs_.push_back({-1, -1});
    }
    rows.insert(rows.end(), pdfNotesEntries_.begin(), pdfNotesEntries_.end());
    for (std::size_t i = 0; i < pdfNotesRefs_.size(); ++i) {
        noteIds_.push_back(0);
        pdfNoteRefs_.push_back(pdfNotesRefs_[i]);
    }
    notes_->setEntries(std::move(rows));
}

void ReaderScreen::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    auto* self = const_cast<ReaderScreen*>(this);
    if (notes_ && notes_->visible) self->syncNotes();

    if (state_.focusMode && state_.autoHideFocusControls &&
        focusControlsVisible_ && focusTouches_.empty() &&
        ctx.now >= focusHideAt_) {
        self->focusControlsVisible_ = false;
        self->focusExitBtn_->visible = false;
        self->stage_->setFocusScrollbarsVisible(false);
        self->layoutTree(); // one frame on deadline, not a 3-second idle loop
    }
    self->focusT_.animateTo(state_.focusMode ? 1.f : 0.f, tokens::dur::focusMode, ctx.now);
    // Layout is already invalidated by resize/tool changes. Recompute the
    // entire inspector grid only while geometry actually animates; doing it
    // on every page-scroll frame made opening the phone sheet noticeably lag.
    const bool moving = focusT_.active(ctx.now) || leftPanel_->animating(ctx.now) ||
                        rightPanel_->animating(ctx.now);
    // A short transition can finish between input and the next frame
    // (backgrounded window, busy GPU, unit-test clock jump). Consume one
    // final geometry update even when active(now) is already false.
    if (moving || layoutWasAnimating_ || panelLayoutDirty_) self->onLayout();
    self->panelLayoutDirty_ = false;
    self->layoutWasAnimating_ = moving;

#if FOLIO_HAVE_PDFIUM_SOURCE
    if (!ctx.chromeOnly)
        if (auto pdf = std::dynamic_pointer_cast<folio::engine::PdfiumPageSource>(pageSource_))
            pdf->beginVisibleFrame();
#endif
    // SkCanvas::clear ignores the clip. On a retained partial chrome frame,
    // clearing here would erase the cached PDF despite the damage rectangle.
    if (!ctx.damage.empty()) t.fillRect(localBounds(), ctx.pal.bg);
    else t.clear(ctx.pal.bg);

    const float f = focusT_.value(ctx.now);
    const float chromeA = 1.f - f;

    // Title-bar brand.
    if (chromeA > 0.01f) {
        const float tb = tokens::size::titleBar;
        const float y = lerp(0.f, -(tb + kCmdH), f);
        t.save();
        t.saveLayerAlpha(chromeA);
        drawIcon(t, "layers",
                 Rect{chromeLead_ + 16.f, y + (tb - 17.f) * 0.5f, 17.f, 17.f},
                 ctx.pal.accent, 2.f);
        t.drawTextInBox("Folio", Rect{chromeLead_ + 40.f, y, 70.f, tb},
                        styleFor(tokens::type_bodyStrong, ctx.pal.text),
                        folio::render::TextAlign::Left);
        t.restore();
        t.restore();
    }
}

/// The pill's text labels live here rather than in onPaint because the pill
/// is a child widget: anything drawn before children is painted over by the
/// card itself.
void ReaderScreen::onPaintOverlay(IPaintTarget& t, const PaintCtx& ctx) {
    if (!pill_->visible) return;

    const int pct = static_cast<int>(stage_->zoom() * 100.f + 0.5f);
    t.drawTextInBox(std::to_string(pct) + "%", zoomLabelRect_,
                    styleFor(tokens::type_small, ctx.pal.muted),
                    folio::render::TextAlign::Centre);

    // Separators between the three groups, positioned from the label rects
    // so they track any layout change automatically.
    const float x0 = pageLabelRect_.right() + 6.f;
    dividerV(t, x0, pageLabelRect_.y + 11.f, pageLabelRect_.bottom() - 11.f,
             ctx.pal.divider, ctx.dpr);
}

}  // namespace folio::ui
