#include "screens/WelcomeScreen.h"

#include "components/Overlays.h"
#include "folio/ui/Icons.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace folio::ui {
namespace {

constexpr float kSideW = tokens::size::panelLeft;
constexpr float kGutter = tokens::space::gutter;
constexpr float kCardW = 206.f;
constexpr float kCardH = 196.f;

class HeroContents final : public Widget {
public:
    std::function<void(IPaintTarget&, const PaintCtx&)> paint;
    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override {
        if (paint) paint(t, ctx);
    }
};

std::string localGreeting() {
    const std::time_t clock = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &clock);
#else
    localtime_r(&clock, &local);
#endif
    return local.tm_hour < 12 ? "Good morning" :
           local.tm_hour < 17 ? "Good afternoon" : "Good evening";
}

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
            onOpenDocument(state_.recents.front().path);
        }
    };

    openCompactBtn_ = emplace<Button>("", "folder-open", ButtonStyle::Primary);
    openCompactBtn_->onClick = openBtn_->onClick;
    openCompactBtn_->tooltip = "Open file";
    openCompactBtn_->visible = false;

    // The sidebar disappears on phones. Its Settings entry must not vanish
    // with it; keep a separate reachable gear beside the compact Open PDF.
    settingsCompactBtn_ = emplace<Button>("", "settings", ButtonStyle::Icon);
    settingsCompactBtn_->tooltip = "Library settings";
    settingsCompactBtn_->visible = false;
    settingsCompactBtn_->onClick = [this] { openSettings(settingsCompactBtn_); };

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

    settingsSidebarBtn_ = sidebar_->emplace<Button>(
        "Settings", "settings", ButtonStyle::ListItem);
    settingsSidebarBtn_->onClick = [this] { openSettings(settingsSidebarBtn_); };
    sideItems_.push_back(settingsSidebarBtn_);
}

void WelcomeScreen::openSettings(Button* anchor) {
    if (!settingsPop_ || !anchor) return;
    if (settingsPop_->isOpen()) { settingsPop_->close(now()); return; }
    settingsPop_->setOwner(anchor);
    settingsPop_->open(anchor->toWindow({anchor->frame.w * .5f, anchor->frame.h}),
                       now());
}

void WelcomeScreen::dismissSettingsIfOutside(Point windowPoint, double now) {
    if (settingsPop_) settingsPop_->dismissIfOutside(windowPoint, now);
    if (cardMenu_) cardMenu_->dismissIfOutside(windowPoint, now);
    if (storagePop_) storagePop_->dismissIfOutside(windowPoint, now);
}

bool WelcomeScreen::onPointer(const PointerEvent& e) {
    if (e.phase == PointerPhase::Down)
        dismissSettingsIfOutside(toWindow(e.pos), e.timestamp);
    return false; // never steal clicks from the cards or toolbar
}

void WelcomeScreen::buildMain() {
    greeting_ = emplace<Label>(localGreeting(), tokens::type_heroTitle);
    subtitle_ = emplace<Label>(
        state_.recents.empty() ? "Choose a PDF to start reading."
                               : "Pick up where you left off, or start something new.",
        tokens::type_label, TextRole::Muted);

    hero_ = emplace<CardView>(tokens::radius::cardLarge);
    auto* heroContents = hero_->emplace<HeroContents>();
    heroContent_ = heroContents;
    heroContents->paint = [this](IPaintTarget& t, const PaintCtx& ctx) {
        paintHero(t, ctx);
    };
    hero_->fill = [](const tokens::Palette& p) { return p.accentSoft; };
    hero_->elevation = [](const tokens::Palette& p) {
        return tokens::elevation::cardFlat(p);
    };

    dropzone_ = emplace<Dropzone>();
    dropzone_->onActivate = [this] { if (onBrowse) onBrowse(); };

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
    sortSeg_->onChange = [this](int) {
        rebuildGrid(now());
        layoutTree();
    };

    scroll_ = emplace<ScrollView>();
    scroll_->showScrollbar = true;
    gridHost_ = scroll_->emplace<Widget>();
    gridHost_->acceptsPointer = false;

    settingsPop_ = emplace<Popover>();
    settingsPop_->setZOrder(zlayer::popover);
    settingsPop_->setFrame(0.f, 0.f, 310.f, 300.f);
    settingsScroll_ = settingsPop_->emplace<ScrollView>();
    settingsScroll_->showScrollbar = true;
    settingsHost_ = settingsScroll_->emplace<Widget>();
    settingsHost_->acceptsPointer = false;
    for (auto [label, icon, action] : {
             std::tuple{"Light appearance", "sun", 0},
             std::tuple{"Dark appearance", "moon", 1},
             std::tuple{"Cosy density", "", 2},
             std::tuple{"Compact density", "", 3}}) {
        auto* choice = settingsHost_->emplace<Button>(label, icon, ButtonStyle::ListItem);
        choice->onClick = [this, action] {
            if (action < 2) {
                themeSeg_->setSelected(action, now());
                const auto c = settingsPop_->toWindow({settingsPop_->frame.w * .5f,
                                                       settingsPop_->frame.h * .5f});
                state_.theme.setScheme(action == 1 ? ColorScheme::Dark : ColorScheme::Light,
                                       now(), c.x, c.y);
            } else openModeSeg_->setSelected(action - 2, now());
            settingsPop_->close(now());
        };
        settingsChoices_.push_back(choice);
    }
#if defined(__ANDROID__)
    for (const char* label : {"New PDFs: Keep offline copy",
                              "New PDFs: Reference source only"}) {
        const bool linked = settingsChoices_.size() == 5;
        auto* choice = settingsHost_->emplace<Button>(label, "", ButtonStyle::ListItem);
        choice->onClick = [this, linked] {
            preferSourceOnly_ = linked;
            if (onSetPreferSourceOnly) onSetPreferSourceOnly(linked);
            settingsPop_->close(now());
        };
        settingsChoices_.push_back(choice);
    }
#endif
    auto* manage = settingsHost_->emplace<Button>(
#if defined(__ANDROID__)
        "Manage each PDF's storage...", "folder-open", ButtonStyle::ListItem);
#else
        "Manage library files...", "folder-open", ButtonStyle::ListItem);
#endif
    manage->onClick = [this] { openStorageList(); };
    settingsChoices_.push_back(manage);

    storagePop_ = emplace<Popover>();
    storagePop_->setZOrder(zlayer::popover + 1);
    storagePop_->setFrame(0.f, 0.f, 312.f, 360.f);
    storageScroll_ = storagePop_->emplace<ScrollView>();
    storageScroll_->showScrollbar = true;
    storageHost_ = storageScroll_->emplace<Widget>();
    storageHost_->acceptsPointer = false;

    cardMenu_ = emplace<Popover>();
    cardMenu_->setZOrder(zlayer::popover + 2);
    cardMenu_->setFrame(0.f, 0.f, 312.f, 302.f);
    for (const char* title : {"Open PDF", "Keep offline copy", "Use source only",
                              "Relink PDF", "Remove from library"}) {
        const int action = static_cast<int>(menuChoices_.size());
        auto* row = cardMenu_->emplace<Button>(title, "", ButtonStyle::ListItem);
        row->onClick = [this, action] { runCardAction(action); };
        menuChoices_.push_back(row);
    }
    rebuildGrid(0.0);
}

void WelcomeScreen::refreshStorageList() {
    if (!storageHost_) return;
    storageRows_.clear();
    storageHost_->removeAllChildren();
    for (const RecentDoc& doc : state_.recents) {
        const std::string path = doc.path;
        auto* row = storageHost_->emplace<Button>(
#if defined(__ANDROID__)
            doc.title + (doc.linked ? " · Source only" : " · Keep copy"),
#else
            doc.title + " · Original file",
#endif
            "file-text", ButtonStyle::ListItem);
        row->onClick = [this, path, row] {
            const Point anchor = row->toWindow({row->frame.w * .5f, row->frame.h});
            storagePop_->close(now());
            openCardMenu(path, anchor, nullptr);
        };
        storageRows_.push_back(row);
    }
    storageScroll_->contentHeight = std::max(52.f, 8.f + 50.f * storageRows_.size());
    layoutTree();
}

void WelcomeScreen::openStorageList() {
    if (!storagePop_) return;
    settingsPop_->close(now());
    refreshStorageList();
    Button* anchor = settingsCompactBtn_->visible ? settingsCompactBtn_ : settingsSidebarBtn_;
    storagePop_->setOwner(anchor);
    storagePop_->open(anchor->toWindow({anchor->frame.w * .5f, anchor->frame.h}), now());
}

void WelcomeScreen::openCardMenu(const std::string& path, Point anchor, RecentCard* card) {
    if (!cardMenu_) return;
    if (settingsPop_) settingsPop_->close(now());
    if (storagePop_) storagePop_->close(now());
    menuPath_ = path;
    cardMenu_->setOwner(card);
    const auto it = std::find_if(state_.recents.begin(), state_.recents.end(),
        [&](const RecentDoc& r) { return r.path == path; });
    const bool linked = it != state_.recents.end() && it->linked;
    menuChoices_[0]->label = linked && it->unavailable ? "Open (source unavailable)" : "Open PDF";
#if defined(__ANDROID__)
    menuChoices_[1]->enabled = linked;
    menuChoices_[2]->enabled = !linked;
    menuChoices_[3]->enabled = linked;
#else
    menuChoices_[1]->enabled = false; // Desktop already references its source path.
    menuChoices_[2]->enabled = false;
    menuChoices_[3]->enabled = false;
#endif
#if defined(__ANDROID__)
    menuChoices_[1]->label = linked ? "Keep offline copy" : "Offline copy retained";
    menuChoices_[2]->label = linked ? "Source only (active)" : "Use source only...";
    menuChoices_[3]->label = linked ? "Relink source PDF..." : "Relink unavailable PDF";
#else
    menuChoices_[1]->label = "No private PDF copy";
    menuChoices_[2]->label = "Original file (active)";
    menuChoices_[3]->label = "Relink not available";
#endif
    cardMenu_->open(anchor, now());
}

void WelcomeScreen::runCardAction(int action) {
    const std::string path = menuPath_;
    cardMenu_->close(now());
    cardMenu_->setOwner(nullptr); // a Remove action can destroy the card
    if (path.empty()) return;
    if (action == 0) { if (onOpenDocument) onOpenDocument(path); }
    else if (action == 4) { if (onRemoveDocument) onRemoveDocument(path); }
    else if (onStorageAction) onStorageAction(path, action == 1 ? 1 : action == 2 ? 2 : 3);
}

void WelcomeScreen::rebuildGrid(double now) {
    if (cardMenu_) {
        cardMenu_->close(now);
        cardMenu_->setOwner(nullptr);
    }
    cards_.clear();
    cardPaths_.clear();
    gridHost_->removeAllChildren();

    std::vector<const RecentDoc*> entries;
    for (const RecentDoc& d : state_.recents)
        if (containsFold(d.title, filter_)) entries.push_back(&d);
    if (sortSeg_->selected() == 1) {
        std::sort(entries.begin(), entries.end(), [](const RecentDoc* a, const RecentDoc* b) {
            return a->title < b->title;
        });
    } else if (sortSeg_->selected() == 2) {
        // Do not stat a potentially slow storage provider on EVERY sort
        // comparison. A missing file is size zero, not uintmax_t(-1).
        std::unordered_map<std::string, std::uintmax_t> sizes;
        for (const RecentDoc* entry : entries) {
            std::error_code ec;
            const auto bytes = std::filesystem::file_size(entry->path, ec);
            sizes[entry->path] = ec ? 0u : bytes;
        }
        std::sort(entries.begin(), entries.end(), [&](const RecentDoc* a, const RecentDoc* b) {
            return sizes.at(a->path) > sizes.at(b->path);
        });
    }
    int shown = 0;
    for (const RecentDoc* entry : entries) {
        const RecentDoc& d = *entry;
        auto* c = gridHost_->emplace<RecentCard>(d.title, d.meta, d.accent);
        c->progress = d.progress;
        c->cover = covers_.get(d.path);
        const std::string path = d.path;
        c->onMenu = [this, path, c](Point at) { openCardMenu(path, at, c); };
        c->onOpen = [this, path] {
            if (onOpenDocument) onOpenDocument(path);
        };
        c->enter(now, shown);
        cards_.push_back(c);
        cardPaths_.push_back(path);
        ++shown;
    }
}

bool WelcomeScreen::pollCovers() {
    if (!covers_.drain()) return false;
    bool changed = false;
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        auto next = covers_.get(cardPaths_[i]);
        if (next != cards_[i]->cover) {
            cards_[i]->cover = std::move(next);
            changed = true;
        }
    }
    return changed;
}

void WelcomeScreen::trimIdleCovers() {
    std::unordered_set<std::string> visible;
    for (std::size_t i = 0; i < cards_.size(); ++i) {
        const float y = cards_[i]->frame.y - scroll_->scrollY();
        if (requestCovers_ && y > -kCardH && y < scroll_->frame.h) {
            visible.insert(cardPaths_[i]);
        } else {
            cards_[i]->cover.reset();
        }
    }
    covers_.keepMemoryFor(visible);
}

void WelcomeScreen::releaseCoverMemory() {
    for (RecentCard* card : cards_) card->cover.reset();
    covers_.releaseMemory();
}

void WelcomeScreen::refreshRecents(double now) {
    subtitle_->text = state_.recents.empty()
        ? "Choose a PDF to start reading."
        : "Pick up where you left off, or start something new.";
    rebuildGrid(now);
    if (storagePop_ && storagePop_->isOpen()) refreshStorageList();
    layoutTree();
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
    const LayoutMetrics m = LayoutMetrics::forViewport(W, H);
    // On a phone the sidebar is gone, not narrow. 268px of chrome next to
    // 120px of content is not a layout, and the sidebar's contents are
    // either duplicated elsewhere (Open file) or filters that belong in a
    // sheet.
    const bool showSide = m.bp != Breakpoint::Compact;
    const float kGutterR = m.gutter;
    const float sideW = m.bp == Breakpoint::Medium ? 208.f : kSideW;

    titleBar_->setFrame(0.f, 0.f, W, tb);
    winCtl_->setFrame(0.f, 0.f, W, tb);
    // The compact layout is a phone-like viewport, not a desktop window:
    // close/minimise/maximise buttons here were dead controls on Android.
#if defined(__ANDROID__)
    winCtl_->visible = false;
#else
    // A narrow desktop window still needs native-feeling window controls.
    // Only Android hides them; width alone is not a platform signal.
    winCtl_->visible = true;
#endif
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
    settingsCompactBtn_->visible = !showSide;
    settingsCompactBtn_->setFrame(mx + mw - ocw * 2.f - 7.f,
                                  y - 4.f, ocw, 38.f);
    greeting_->setFrame(mx, y,
        mw - (showSide ? 0.f : ocw * 2.f + 15.f), 30.f);
    y += 40.f;
    // The second generic sentence competes with the PDF grid on phones.
    subtitle_->visible = false;
    subtitle_->setFrame(mx, y, mw, 0.f);

    // Side by side needs about 460px before the drop zone stops being a
    // sliver; below that the hero takes the row and the drop zone goes.
    const float heroH = m.bp == Breakpoint::Compact ? 104.f : 128.f;
    const bool twoUpHero = mw > 460.f;
    const float dzW = twoUpHero ? std::min(300.f, mw * 0.34f) : 0.f;
    hero_->setFrame(mx, y, twoUpHero ? mw - dzW - tokens::space::panelGap : mw,
                    heroH);
    hero_->visible = showSide;
    heroContent_->setFrame(0.f, 0.f, hero_->frame.w, heroH);
    dropzone_->setFrame(mx + mw - dzW, y, dzW, heroH);
    dropzone_->visible = showSide && twoUpHero;
    if (showSide) y += heroH + kGutterR;

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

    settingsPop_->windowSize = {W, H};
    settingsPop_->setFrame(settingsPop_->frame.x, settingsPop_->frame.y,
                           std::min(310.f, std::max(0.f, W - 16.f)),
                           std::min(std::max(300.f,
                                     22.f + 49.f * settingsChoices_.size()),
                                    std::max(0.f, H - 16.f)));
    settingsScroll_->setFrame(6.f, 6.f, settingsPop_->frame.w - 12.f,
                              settingsPop_->frame.h - 12.f);
    settingsScroll_->contentHeight = 8.f + 49.f * settingsChoices_.size();
    settingsHost_->setFrame(0.f, -settingsScroll_->scrollY(),
                            settingsScroll_->frame.w,
                            settingsScroll_->contentHeight);
    storagePop_->windowSize = {W, H};
    storagePop_->setFrame(storagePop_->frame.x, storagePop_->frame.y,
                          std::min(312.f, std::max(0.f, W - 16.f)),
                          std::min(360.f, std::max(0.f, H - 16.f)));
    storageScroll_->setFrame(6.f, 8.f, storagePop_->frame.w - 12.f,
                             storagePop_->frame.h - 16.f);
    storageHost_->setFrame(0.f, -storageScroll_->scrollY(),
                           storageScroll_->frame.w, storageScroll_->contentHeight);
    for (std::size_t i = 0; i < storageRows_.size(); ++i)
        storageRows_[i]->setFrame(4.f, 4.f + 50.f * static_cast<float>(i),
                                  storageHost_->frame.w - 12.f, 46.f);
    for (std::size_t i = 0; i < settingsChoices_.size(); ++i)
        settingsChoices_[i]->setFrame(8.f, 8.f + static_cast<float>(i) * 49.f,
                                     settingsHost_->frame.w - 16.f, 44.f);
    settingsChoices_[0]->toggled = state_.theme.scheme() != ColorScheme::Dark;
    settingsChoices_[1]->toggled = state_.theme.scheme() == ColorScheme::Dark;
    settingsChoices_[2]->toggled = openModeSeg_->selected() == 0;
    settingsChoices_[3]->toggled = openModeSeg_->selected() == 1;
    if (settingsChoices_.size() > 5) {
        settingsChoices_[4]->toggled = !preferSourceOnly_;
        settingsChoices_[5]->toggled = preferSourceOnly_;
    }
    cardMenu_->windowSize = {W, H};
    cardMenu_->setFrame(cardMenu_->frame.x, cardMenu_->frame.y,
                        std::min(312.f, std::max(0.f, W - 16.f)),
                        std::min(302.f, std::max(0.f, H - 16.f)));
    for (std::size_t i = 0; i < menuChoices_.size(); ++i)
        menuChoices_[i]->setFrame(8.f, 16.f + static_cast<float>(i) * 54.f,
                                  cardMenu_->frame.w - 16.f, 49.f);
    settingsPop_->constrainToWindow();
    storagePop_->constrainToWindow();
    cardMenu_->constrainToWindow();

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
    greeting_->text = localGreeting();
    if (storageHost_ && storageScroll_)
        storageHost_->frame.y = -storageScroll_->scrollY();
    if (settingsHost_ && settingsScroll_)
        settingsHost_->frame.y = -settingsScroll_->scrollY();
    // Keep the scrolled content following the scroll offset. Doing it in
    // paint rather than in the scroll handler means it also tracks the
    // inertial fling, which advances inside ScrollView::onPaint.
    gridHost_->frame.y = -scroll_->scrollY();
    if (requestCovers_) {
        // Only cards on screen (plus one imminent row) ask for work. No
        // document parser, file stat or bitmap allocation runs on this UI
        // thread; the loader sleeps when its bounded queue is empty.
        for (std::size_t i = 0; i < cards_.size(); ++i) {
            const float y = cards_[i]->frame.y - scroll_->scrollY();
            if (y > -kCardH && y < scroll_->frame.h + kCardH)
                covers_.request(cardPaths_[i]);
        }
    }

    t.clear(ctx.pal.bg);

    // Title bar: logo mark + wordmark.
    const float tb = tokens::size::titleBar;
    drawIcon(t, "layers", Rect{wordmarkX_ + 4.f, (tb - 18.f) * 0.5f, 18.f, 18.f},
             ctx.pal.accent, 2.f);
    t.drawTextInBox("Folio", Rect{wordmarkX_ + 26.f, 0.f, 120.f, tb},
                    styleFor(tokens::type_bodyStrong, ctx.pal.text),
                    folio::render::TextAlign::Left);

    // Empty-search copy belongs BELOW child popovers, like the hero copy.
    const Rect sc = scroll_->frame;
    if (cards_.empty() && !filter_.empty()) {
        t.drawTextInBox("No documents match \"" + filter_ + "\"",
                        Rect{sc.x, sc.y + 40.f, sc.w, 22.f},
                        styleFor(tokens::type_label, ctx.pal.muted),
                        folio::render::TextAlign::Centre);
    }

}

// Hero contents are a child of the card, not a screen overlay. The former
// screen-level overlay painted *after* settings and made the hero text shine
// through opaque popovers.
void WelcomeScreen::paintHero(IPaintTarget& t, const PaintCtx& ctx) {
    const float hp = heroEnter_.phase(ctx.now);
    const Rect h{0.f, 0.f, heroContent_->frame.w, heroContent_->frame.h};
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
    t.drawTextInBox(state_.recents.empty() ? "Open your first PDF" : "Continue reading",
                    Rect{h.x + pad, h.y + yLabel, h.w - pad * 2.f, 18.f},
                    styleFor(tokens::type_sectionHeader, ctx.pal.accentText),
                    folio::render::TextAlign::Left);
    if (state_.recents.empty()) {
        t.drawTextInBox("Tap the folder icon to choose a document",
                        Rect{h.x + pad, h.y + yTitle, h.w - pad * 2.f, 40.f},
                        styleFor(tokens::type_body, ctx.pal.text),
                        folio::render::TextAlign::Left);
    } else {
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
}


}  // namespace folio::ui
