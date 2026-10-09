// folio-ui/screens/WelcomeScreen.h
//
// The start screen: title bar, a sidebar of actions, and a main pane with a
// greeting, a hero card, a drop target, a filter row and the recents grid.

#pragma once

#include "components/Button.h"
#include "components/Containers.h"
#include "components/Controls.h"
#include "components/WindowChrome.h"
#include "screens/AppState.h"

#include <functional>
#include <string>
#include <vector>

namespace folio::ui {

class WelcomeScreen : public Widget {
public:
    explicit WelcomeScreen(AppState& state);

    /// Raised when a document should be opened; the app switches screens.
    std::function<void(const std::string& title, int pages)> onOpenDocument;
    /// Ask the app for a file chooser.
    std::function<void()> onBrowse;
    std::function<void()> onQuit;
    /// See ReaderScreen::windowControls.
    [[nodiscard]] WindowControls* windowControls() noexcept { return winCtl_; }

    /// Re-run the staggered card entrance (on first show, and whenever the
    /// filter changes so the grid re-animates like the prototype).
    void replayEntrance(double now);

    void onLayout() override;
    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    void onPaintOverlay(IPaintTarget& t, const PaintCtx& ctx) override;
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
    Dropzone* dropzone_ = nullptr;
    TextField* search_ = nullptr;
    SegmentedControl* sortSeg_ = nullptr;
    Widget* gridHost_ = nullptr;
    ScrollView* scroll_ = nullptr;
    std::vector<RecentCard*> cards_;

    std::string filter_;
    Timeline heroEnter_;
    double entranceAt_ = 0.0;
};

}  // namespace folio::ui
