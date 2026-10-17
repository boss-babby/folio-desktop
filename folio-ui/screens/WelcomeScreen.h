// folio-ui/screens/WelcomeScreen.h
//
// The start screen: title bar, a sidebar of actions, and a main pane with a
// greeting, a hero card, a drop target, a filter row and the recents grid.

#pragma once

#include "components/Button.h"
#include "components/Containers.h"
#include "components/Controls.h"
#include "components/WindowChrome.h"
#include "components/Overlays.h"
#include "screens/AppState.h"
#include "screens/RecentCovers.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace folio::ui {

class WelcomeScreen : public Widget {
public:
    explicit WelcomeScreen(AppState& state);

    /// Raised when a document should be opened; the app switches screens.
    std::function<void(const std::string& path)> onOpenDocument;
    void refreshRecents(double now);
    void setRecentStateFile(const std::string& path) { covers_.setStateFile(path); }
    void setCoverWakeCallback(std::function<void()> wake) {
        covers_.setWake(std::move(wake));
    }
    [[nodiscard]] bool coversReady() const noexcept { return covers_.hasResults(); }
    [[nodiscard]] bool coversBusy() const { return covers_.busy(); }
    bool pollCovers();
    void trimIdleCovers();
    void releaseCoverMemory();
    void setCoverRequestsEnabled(bool on) {
        requestCovers_ = on;
        if (!on) covers_.cancelQueued();
    }
    /// Ask the app for a file chooser.
    std::function<void()> onBrowse;
    std::function<void(const std::string&, int)> onStorageAction;
    std::function<void(const std::string&)> onRemoveDocument;
    std::function<void(bool)> onSetPreferSourceOnly;
    void setPreferSourceOnly(bool enabled) { preferSourceOnly_ = enabled; }
    std::function<void()> onQuit;
    /// See ReaderScreen::windowControls.
    [[nodiscard]] WindowControls* windowControls() noexcept { return winCtl_; }

    /// Re-run the staggered card entrance (on first show, and whenever the
    /// filter changes so the grid re-animates like the prototype).
    void replayEntrance(double now);

    void onLayout() override;
    bool onPointer(const PointerEvent& e) override;
    void dismissSettingsIfOutside(Point windowPoint, double now);
    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    [[nodiscard]] bool animating(double now) const override;

private:
    void buildSidebar();
    void buildMain();
    void rebuildGrid(double now);

    /// Event handlers have no clock of their own, so the last painted time
    /// is cached here. Passing a stale timestamp to an animation makes it
    /// start already finished - which is exactly what made the theme snap.

    AppState& state_;

    // Title bar
    Stack* titleBar_ = nullptr;
    WindowControls* winCtl_ = nullptr;
    float wordmarkX_ = 14.f;
    // Sidebar
    CardView* sidebar_ = nullptr;
    Button* openBtn_ = nullptr;
    /// Compact-only twin of openBtn_. The real one lives in the sidebar,
    /// and the sidebar is gone on a phone - without this there is no way
    /// to open a document that is not already in the recents grid.
    Button* openCompactBtn_ = nullptr;
    Button* settingsCompactBtn_ = nullptr;
    Button* settingsSidebarBtn_ = nullptr;
    Popover* settingsPop_ = nullptr;
    ScrollView* settingsScroll_ = nullptr;
    Widget* settingsHost_ = nullptr;
    std::vector<Button*> settingsChoices_;
    Popover* cardMenu_ = nullptr;
    Popover* storagePop_ = nullptr;
    ScrollView* storageScroll_ = nullptr;
    Widget* storageHost_ = nullptr;
    std::vector<Button*> storageRows_;
    std::vector<Button*> menuChoices_;
    std::string menuPath_;
    bool preferSourceOnly_ = false;
    void openSettings(Button* anchor);
    void openCardMenu(const std::string& path, Point anchor, RecentCard* card);
    void runCardAction(int action);
    void openStorageList();
    void refreshStorageList();
    std::vector<Button*> sideItems_;
    SegmentedControl* themeSeg_ = nullptr;
    SegmentedControl* openModeSeg_ = nullptr;
    Label* startHdr_ = nullptr;
    Label* openInHdr_ = nullptr;
    Label* themeHdr_ = nullptr;
    // Main
    Label* greeting_ = nullptr;
    Label* subtitle_ = nullptr;
    CardView* hero_ = nullptr;
    Widget* heroContent_ = nullptr;
    void paintHero(IPaintTarget& t, const PaintCtx& ctx);
    Dropzone* dropzone_ = nullptr;
    TextField* search_ = nullptr;
    SegmentedControl* sortSeg_ = nullptr;
    Widget* gridHost_ = nullptr;
    ScrollView* scroll_ = nullptr;
    std::vector<RecentCard*> cards_;
    std::vector<std::string> cardPaths_;
    RecentCovers covers_;
    bool requestCovers_ = true;

    std::string filter_;
    Timeline heroEnter_;
    double entranceAt_ = 0.0;
};

}  // namespace folio::ui
