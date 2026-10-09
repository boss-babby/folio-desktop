#include "screens/WelcomeScreen.h"

#include "components/Overlays.h"
#include "folio/ui/Icons.h"

#include <algorithm>
#include <cmath>

namespace folio::ui {
namespace {

constexpr float kSideW = tokens::size::panelLeft;
constexpr float kGutter = tokens::space::gutter;
constexpr float kCardW = 206.f;
constexpr float kCardH = 196.f;

bool containsFold(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

}  // namespace

WelcomeScreen::WelcomeScreen(AppState& state) : state_(state) {
    debugName = "WelcomeScreen";
    buildSidebar();
    buildMain();
}

void WelcomeScreen::buildSidebar() {
    // --- title bar ---------------------------------------------------------
    titleBar_ = emplace<Stack>(Stack::Dir::Row);
    titleBar_->cross = Stack::Cross::Centre;
    titleBar_->setPadding(10.f, 0.f);
    titleBar_->spacing = 6.f;

    // Window controls live on the library screen too -- a frameless window
    // with no way to close it from its own home page is a trap. Placement is
    // the platform's business, not ours; see WindowChrome.
    winCtl_ = emplace<WindowControls>();
    winCtl_->onClose = [this] { if (onQuit) onQuit(); };

    // --- sidebar card ------------------------------------------------------
    sidebar_ = emplace<CardView>(tokens::radius::card);

    openBtn_ = sidebar_->emplace<Button>("Open file", "folder-open", ButtonStyle::Primary);
    openBtn_->onClick = [this] {
        if (onBrowse) { onBrowse(); return; }
        if (!state_.recents.empty() && onOpenDocument) {
            onOpenDocument(state_.recents.front().title, state_.recents.front().pages);
        }
    };

    openCompactBtn_ = emplace<Button>("", "folder-open", ButtonStyle::Primary);
    openCompactBtn_->onClick = openBtn_->onClick;
    openCompactBtn_->tooltip = "Open file";
    openCompactBtn_->visible = false;

    startHdr_ = sidebar_->emplace<Label>("Library", tokens::type_sectionHeader,
                                         TextRole::Muted);

    struct Row { const char* label; const char* icon; };
    static constexpr Row kRows[] = {
        {"All documents", "file-text"},
        {"Starred",       "star"},
        {"Shared",        "share"},
        {"Trash",         "trash"},
    };
    for (const Row& r : kRows) {
        auto* b = sidebar_->emplace<Button>(r.label, r.icon, ButtonStyle::ListItem);
        sideItems_.push_back(b);
    }
    if (!sideItems_.empty()) sideItems_.front()->toggled = true;

    openInHdr_ = sidebar_->emplace<Label>("Appearance", tokens::type_sectionHeader,
                                          TextRole::Muted);
    themeSeg_ = sidebar_->emplace<SegmentedControl>(
        std::vector<SegmentedControl::Item>{{"Light", "sun", "Light theme"},
                                            {"Dark", "moon", "Dark theme"}},
        0);
    themeSeg_->onChange = [this](int i) {
        // Reveal from the control itself, matching the reader.
        const auto c = themeSeg_->toWindow(
            {themeSeg_->frame.w * (i == 1 ? 0.75f : 0.25f), themeSeg_->frame.h * 0.5f});
        state_.theme.setScheme(i == 1 ? ColorScheme::Dark : ColorScheme::Light, now(),
                               c.x, c.y);
    };

    themeHdr_ = sidebar_->emplace<Label>("Density", tokens::type_sectionHeader,
                                         TextRole::Muted);
    openModeSeg_ = sidebar_->emplace<SegmentedControl>(
        std::vector<SegmentedControl::Item>{{"Cosy", "", ""}, {"Compact", "", ""}}, 0);

    auto* settings = sidebar_->emplace<Button>("Settings", "settings", ButtonStyle::ListItem);
    settings->onClick = [this] {
        OverlayHost::current().showToast("Settings are not wired up yet", now());
    };
    sideItems_.push_back(settings);
}

void WelcomeScreen::buildMain() {
    greeting_ = emplace<Label>("Good afternoon", tokens::type_heroTitle);
    subtitle_ = emplace<Label>("Pick up where you left off, or start something new.",
                               tokens::type_label, TextRole::Muted);

    hero_ = emplace<CardView>(tokens::radius::cardLarge);
    hero_->fill = [](const tokens::Palette& p) { return p.accentSoft; };
    hero_->elevation = [](const tokens::Palette& p) {
        return tokens::elevation::cardFlat(p);
    };

    dropzone_ = emplace<Dropzone>();
    dropzone_->onActivate = [this] {
        OverlayHost::current().showToast("A real build opens the system file picker here",
                                         entranceAt_);
    };

    search_ = emplace<TextField>("Search documents");
    search_->leadingIcon = "search";
    search_->onChanged = [this](const std::string& s) {
        filter_ = s;
        rebuildGrid(entranceAt_);
        layoutTree();
    };

    sortSeg_ = emplace<SegmentedControl>(
        std::vector<SegmentedControl::Item>{{"Recent", "", "Sort by last opened"},
                                            {"Name", "", "Sort alphabetically"},
                                            {"Size", "", "Sort by file size"}},
        0);

    scroll_ = emplace<ScrollView>();
    scroll_->showScrollbar = true;
    gridHost_ = scroll_->emplace<Widget>();
    gridHost_->acceptsPointer = false;

    rebuildGrid(0.0);
}

void WelcomeScreen::rebuildGrid(double now) {
    cards_.clear();
    gridHost_->removeAllChildren();

    int shown = 0;
    for (const RecentDoc& d : state_.recents) {
        if (!containsFold(d.title, filter_)) continue;
        auto* c = gridHost_->emplace<RecentCard>(d.title, d.meta, d.accent);
        c->progress = d.progress;
        const std::string title = d.title;
        const int pages = d.pages;
        c->onOpen = [this, title, pages] {
            if (onOpenDocument) onOpenDocument(title, pages);
        };
        c->enter(now, shown);
        cards_.push_back(c);
        ++shown;
    }
}

void WelcomeScreen::replayEntrance(double now) {
    entranceAt_ = now;
    heroEnter_.start(now, tokens::dur::pageEnter, 0, Curve::OutCubic);
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        cards_[i]->enter(now, static_cast<int>(i));
    }
}

void WelcomeScreen::onLayout() {
    const float W = frame.w, H = frame.h;
    const float tb = tokens::size::titleBar;
    const LayoutMetrics m = LayoutMetrics::forWidth(W);
    // On a phone the sidebar is gone, not narrow. 268px of chrome next to
    // 120px of content is not a layout, and the sidebar's contents are
    // either duplicated elsewhere (Open file) or filters that belong in a
    // sheet.
    const bool showSide = m.bp != Breakpoint::Compact;
    const float kGutterR = m.gutter;
    const float sideW = m.bp == Breakpoint::Medium ? 208.f : kSideW;

    titleBar_->setFrame(0.f, 0.f, W, tb);
    winCtl_->setFrame(0.f, 0.f, W, tb);
    // The wordmark yields to the controls when they are on the leading edge.
    wordmarkX_ = winCtl_->controlsOnLeft() ? winCtl_->preferredWidth() + 6.f : 14.f;

    const float contentTop = tb + kGutterR * 0.5f;
    sidebar_->setFrame(kGutterR, contentTop, sideW, H - contentTop - kGutterR);
    sidebar_->visible = showSide;

    // --- sidebar internals (manual: the spacing differs per group) ---------
    {
        float y = tokens::space::pad;
        const float innerW = sideW - tokens::space::pad * 2.f;
        const float x = tokens::space::pad;

        openBtn_->setFrame(x, y, innerW, tokens::size::pill);
        y += tokens::size::pill + tokens::space::gap * 1.5f;

        std::size_t item = 0;
        auto placeHeader = [&](Widget* w) {
            w->setFrame(x + 2.f, y, innerW, 18.f);
            y += 22.f;
        };
        auto placeRow = [&](Widget* w) {
            w->setFrame(x, y, innerW, tokens::size::control + 2.f);
            y += tokens::size::control + 4.f;
        };

        // Direct pointers, not children()[n]. The indexed version silently
        // mislaid every label the moment the sidebar's contents changed.
        placeHeader(startHdr_);
        for (; item < 4 && item < sideItems_.size(); ++item) placeRow(sideItems_[item]);

        const float segH = tokens::size::field + 4.f;
        const float ctrlH = tokens::size::control;

        y += tokens::space::gap;
        placeHeader(openInHdr_);                       // "Appearance"
        themeSeg_->setFrame(x, y, innerW, segH);
        y += segH + tokens::space::gap;

        placeHeader(themeHdr_);                        // "Density"
        openModeSeg_->setFrame(x, y, innerW, segH);

        // Settings pinned to the foot of the card.
        if (sideItems_.size() > 4)
            sideItems_[4]->setFrame(x, sidebar_->frame.h - tokens::space::pad - ctrlH,
                                    innerW, ctrlH);
    }

    // --- main pane ---------------------------------------------------------
    const float mx = showSide ? kGutterR + sideW + kGutterR : kGutterR;
    const float mw = W - mx - kGutterR;
    float y = contentTop + 6.f;

    // Compact: the open action rides in the greeting row, right-aligned,
    // where there is always space because the greeting is one short line.
    const float ocw = 44.f;
    openCompactBtn_->visible = !showSide;
    openCompactBtn_->setFrame(mx + mw - ocw, y - 4.f, ocw, 38.f);
    greeting_->setFrame(mx, y, mw - (showSide ? 0.f : ocw + 8.f), 30.f);
    y += 32.f;
    subtitle_->setFrame(mx, y, mw, 22.f);
    y += 32.f;

    // Side by side needs about 460px before the drop zone stops being a
    // sliver; below that the hero takes the row and the drop zone goes.
    const float heroH = m.bp == Breakpoint::Compact ? 104.f : 128.f;
    const bool twoUpHero = mw > 460.f;
    const float dzW = twoUpHero ? std::min(300.f, mw * 0.34f) : 0.f;
    hero_->setFrame(mx, y, twoUpHero ? mw - dzW - tokens::space::panelGap : mw,
                    heroH);
    dropzone_->setFrame(mx + mw - dzW, y, dzW, heroH);
    dropzone_->visible = twoUpHero;
    y += heroH + kGutterR;

    const float fieldH = tokens::size::field + 4.f;
    const bool twoUpSearch = mw > 520.f;
    const float sortW = twoUpSearch ? 230.f : mw;
    if (twoUpSearch) {
        search_->setFrame(mx, y, mw - sortW - tokens::space::panelGap, fieldH);
        sortSeg_->setFrame(mx + mw - sortW, y, sortW, fieldH);
        y += fieldH + tokens::space::panelGap;
    } else {
        search_->setFrame(mx, y, mw, fieldH);
        y += fieldH + 8.f;
        sortSeg_->setFrame(mx, y, sortW, fieldH);
        y += fieldH + tokens::space::panelGap;
    }

    const float gridH = std::max(60.f, H - y - kGutterR);
    scroll_->setFrame(mx, y, mw, gridH);

    // --- recents grid ------------------------------------------------------
    // Fit as many columns of at least kCardMinW as the pane allows. The
    // old form divided by a *fixed* card width, so a narrow pane produced
    // one column of 120px slivers instead of one readable card.
    const float kCardMinW = m.bp == Breakpoint::Compact ? 150.f : kCardW;
    const int cols = std::max(1, static_cast<int>((mw + tokens::space::panelGap) /
                                                  (kCardMinW + tokens::space::panelGap)));
    const float usable = mw - 10.f;  // room for the scrollbar
    const float cw = (usable - static_cast<float>(cols - 1) * tokens::space::panelGap) /
                     static_cast<float>(cols);
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        const int col = static_cast<int>(i) % cols;
        const int row = static_cast<int>(i) / cols;
        cards_[i]->setFrame(static_cast<float>(col) * (cw + tokens::space::panelGap),
                            static_cast<float>(row) * (kCardH + tokens::space::panelGap),
                            cw, kCardH);
    }
    const int rows = (static_cast<int>(cards_.size()) + cols - 1) / cols;
    scroll_->contentHeight =
        static_cast<float>(rows) * (kCardH + tokens::space::panelGap);
    gridHost_->setFrame(0.f, -scroll_->scrollY(), mw, scroll_->contentHeight);
}

bool WelcomeScreen::animating(double now) const {
    return heroEnter_.active(now);
}

void WelcomeScreen::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    
    // Keep the scrolled content following the scroll offset. Doing it in
    // paint rather than in the scroll handler means it also tracks the
    // inertial fling, which advances inside ScrollView::onPaint.
    gridHost_->frame.y = -scroll_->scrollY();

    t.clear(ctx.pal.bg);

    // Title bar: logo mark + wordmark.
    const float tb = tokens::size::titleBar;
    drawIcon(t, "layers", Rect{wordmarkX_ + 4.f, (tb - 18.f) * 0.5f, 18.f, 18.f},
             ctx.pal.accent, 2.f);
    t.drawTextInBox("Folio", Rect{wordmarkX_ + 26.f, 0.f, 120.f, tb},
                    styleFor(tokens::type_bodyStrong, ctx.pal.text),
                    folio::render::TextAlign::Left);

}

/// Painted after children so it lands on top of the hero CardView.
/// Drawing it in onPaint put it underneath, which is why the hero read as
/// an empty lavender rectangle.
void WelcomeScreen::onPaintOverlay(IPaintTarget& t, const PaintCtx& ctx) {
    // Hero card content, painted here because it is bespoke rather than a
    // reusable component.
    const float hp = heroEnter_.phase(ctx.now);
    const Rect h = hero_->frame;
    t.save();
    t.saveLayerAlpha(hp);
    t.translate(0.f, lerp(10.f, 0.f, hp));
    // The hero is shorter on a phone, so its rows cannot sit at fixed
    // offsets measured from both edges - at 104px the meta line and the
    // progress track landed on top of each other. Derive the flow from the
    // box that was actually handed to us.
    const bool tight = h.h < 118.f;
    const float pad = tight ? 14.f : 20.f;
    const float yLabel = tight ? 8.f : 18.f;
    const float yTitle = tight ? 24.f : 40.f;
    const float yMeta = tight ? 48.f : 64.f;
    const float upTrack = tight ? 24.f : 26.f;
    const float upPct = tight ? 17.f : 20.f;
    t.drawTextInBox("Continue reading", Rect{h.x + pad, h.y + yLabel, h.w - pad * 2.f, 18.f},
                    styleFor(tokens::type_sectionHeader, ctx.pal.accentText),
                    folio::render::TextAlign::Left);
    if (!state_.recents.empty()) {
        const RecentDoc& d = state_.recents.front();
        t.drawTextInBox(d.title, Rect{h.x + pad, h.y + yTitle, h.w - pad * 2.f, 26.f},
                        styleFor(tokens::type_cardTitle, ctx.pal.text),
                        folio::render::TextAlign::Left);
        t.drawTextInBox(d.meta, Rect{h.x + pad, h.y + yMeta, h.w - pad * 2.f, 20.f},
                        styleFor(tokens::type_small, ctx.pal.muted),
                        folio::render::TextAlign::Left);

        const Rect track{h.x + pad, h.bottom() - upTrack, h.w - pad * 2.f, 4.f};
        t.fillRoundedRect(track, Corners::all(2.f), fade(ctx.pal.accent, 0.18f));
        t.fillRoundedRect(Rect{track.x, track.y, track.w * d.progress * hp, track.h},
                          Corners::all(2.f), ctx.pal.accent);
        t.drawTextInBox(std::to_string(static_cast<int>(d.progress * 100.f)) + "% read",
                        Rect{h.x + pad, h.bottom() - upPct, h.w - pad * 2.f, 16.f},
                        styleFor(tokens::type_small, ctx.pal.muted),
                        folio::render::TextAlign::Left);
    }
    t.restore();
    t.restore();

    // Section header above the grid.
    const Rect sc = scroll_->frame;
    if (cards_.empty() && !filter_.empty()) {
        t.drawTextInBox("No documents match \"" + filter_ + "\"",
                        Rect{sc.x, sc.y + 40.f, sc.w, 22.f},
                        styleFor(tokens::type_label, ctx.pal.muted),
                        folio::render::TextAlign::Centre);
    }
}

}  // namespace folio::ui
