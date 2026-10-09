// folio-ui/screens/FolioApp.h
//
// The application root: owns state, both screens and the overlay layer, and
// presents the only API a platform shell needs.
//
// A shell's whole job is: create a surface, pump events into `pointer`/`key`,
// call `paint` when `needsFrame` says so, and present. Everything else -
// layout, animation, hit-testing, theming - happens in here, identically on
// every platform. That is what makes the SDL3 shell and the streaming shell
// ~200 lines each rather than two parallel applications.

#pragma once

#include "components/Overlays.h"
#include "screens/AppState.h"
#include "screens/ReaderScreen.h"
#include "screens/WelcomeScreen.h"
#include "components/OpenDialog.h"

#include <memory>

namespace folio::ui {

class FolioApp {
public:
    FolioApp(float width, float height);
    ~FolioApp();

    void resize(float width, float height);
    [[nodiscard]] float width() const noexcept { return root_.frame.w; }
    [[nodiscard]] float height() const noexcept { return root_.frame.h; }

    /// Paint one frame. `now` is seconds since an arbitrary epoch and must
    /// increase monotonically.
    void paint(IPaintTarget& t, double now, float dpr = 1.f);

    bool pointer(const PointerEvent& e);
    bool key(const KeyEvent& e);

    /// True when something still needs to move. A shell that respects this
    /// idles at zero CPU instead of spinning at 60 FPS over a static image.
    [[nodiscard]] bool needsFrame(double now) const;

    /// Force a repaint on the next loop (used after an external state change).
    void invalidate() noexcept { forceFrames_ = 2; }

    [[nodiscard]] AppState& state() noexcept { return state_; }
    [[nodiscard]] Widget& root() noexcept { return root_; }

    /// Switch screens with the prototype's 500 ms cross-fade.
    void showScreen(Screen s, double now);

    /// Everything the app can only do with the host's help. A shell fills in
    /// what its platform supports and leaves the rest empty; the app checks
    /// before calling, so the streaming shell (no clipboard, no window
    /// manager) simply gets no-ops rather than a different code path.
    struct HostServices {
        std::function<void()> minimise;
        std::function<void()> toggleMaximise;
        std::function<void()> close;
        /// Hand a title-bar drag to the window manager.
        std::function<void()> beginDragMove;
        std::function<bool()> isMaximised;
        std::function<void(const std::string&)> copyText;
        std::function<std::string()> pasteText;
        std::function<void(const std::string&)> setTitle;
    };
    void setHostServices(HostServices s);
    [[nodiscard]] const HostServices& host() const noexcept { return host_; }

    /// Diagnostics for the shell's status line.
    struct Stats {
        int widgets = 0;
        bool animating = false;
        Rect damage{};
        bool partial = false;
        std::string driver;   ///< debugName of the widest animating widget
    };
    [[nodiscard]] Stats stats(double now) const;

private:
    HostServices host_;
    AppState state_;
    Widget root_;
    WelcomeScreen* welcome_ = nullptr;
    ReaderScreen* reader_ = nullptr;
    OverlayHost* overlay_ = nullptr;
    OpenDialog* openDlg_ = nullptr;
    void requestOpenFile();

    /// Union of the window-space bounds of everything that is moving this
    /// frame, plus whatever was moving last frame. See paint().
    [[nodiscard]] Rect computeDamage(double now) const;
    Rect prevDamage_{0.f, 0.f, 0.f, 0.f};
    bool fullRepaint_ = true;
    Rect lastDamage_{};
    bool lastPartial_ = false;
    std::string damageDriver_;
    /// A pointer event repaints, but only near the pointer: hover tints,
    /// pressed states and tooltips are all local to the cursor. Anything
    /// that changes elsewhere either animates (and so is in the damage set)
    /// or goes through invalidate(), which forces a full frame.
    Rect pointerDamage_{};
    int pointerFrames_ = 0;
    EventRouter router_;

    AnimatedF screenFade_{0.f};  // 0 = welcome, 1 = reader
    Screen target_ = Screen::Welcome;
    int forceFrames_ = 2;
    double lastNow_ = 0.0;
};

}  // namespace folio::ui
