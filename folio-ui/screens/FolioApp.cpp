#include "screens/FolioApp.h"

#include <filesystem>
#include <system_error>

#include <algorithm>
#include <cmath>

namespace folio::ui {

FolioApp::FolioApp(float w, float h) {
    state_.seedDemoContent();
    root_.setFrame(0.f, 0.f, w, h);
    root_.debugName = "root";

    welcome_ = root_.emplace<WelcomeScreen>(state_);
    reader_ = root_.emplace<ReaderScreen>(state_);
    // The dialog is added before the overlay host so tooltips and toasts
    // still paint above it.
    openDlg_ = root_.emplace<OpenDialog>();
    overlay_ = root_.emplace<OverlayHost>();
    OverlayHost::setCurrent(overlay_);

    openDlg_->onOpen = [this](const std::string& path) {
        // Real file, real engine. openFile() falls back to the placeholder
        // and reports why if PDFium refuses it, so a damaged PDF shows a
        // message rather than an empty window.
        if (reader_->openFile(path, lastNow_)) {
            showScreen(Screen::Reader, lastNow_);
            return;
        }
        std::string title = path;
        const auto slash = title.find_last_of("/\\");
        if (slash != std::string::npos) title = title.substr(slash + 1);
        reader_->openDocument(title, 24, lastNow_);
        showScreen(Screen::Reader, lastNow_);
    };

    reader_->visible = false;

    welcome_->onOpenDocument = [this](const std::string& title, int pages) {
        // The recents list is seeded with names; if a file of that name is
        // sitting in the documents folder, open the real thing.
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path p = fs::path("documents") / title;
        if (fs::exists(p, ec) && reader_->openFile(p.string(), lastNow_)) {
            showScreen(Screen::Reader, lastNow_);
            return;
        }
        reader_->openDocument(title, pages, lastNow_);
        showScreen(Screen::Reader, lastNow_);
    };
    welcome_->onBrowse = [this] { requestOpenFile(); };
    reader_->onBrowse = [this] { requestOpenFile(); };
    // Extract asks where to put the result. Routed through the app rather
    // than the screen because the dialog lives on the overlay layer.
    reader_->onPickSavePath = [this](const std::string& suggested,
                                     std::function<void(std::string)> done) {
        openDlg_->onSave = [done](const std::string& p) { if (done) done(p); };
        openDlg_->openSave(suggested, lastNow_);
    };
    reader_->onBackToWelcome = [this] {
        showScreen(Screen::Welcome, lastNow_);
        welcome_->replayEntrance(lastNow_);
    };

    resize(w, h);
    welcome_->replayEntrance(0.0);
}

FolioApp::~FolioApp() {
    if (&OverlayHost::current() == overlay_) OverlayHost::setCurrent(nullptr);
}

void FolioApp::setHostServices(HostServices s) {
    host_ = std::move(s);

    // The window controls exist on both screens and must behave the same on
    // each, so they are wired here rather than in either screen. Close is
    // left to the screens' own onQuit, which already routes through the
    // app's shutdown path.
    for (WindowControls* wc :
         {welcome_ ? welcome_->windowControls() : nullptr,
          reader_ ? reader_->windowControls() : nullptr}) {
        if (!wc) continue;
        if (host_.minimise) wc->onMinimise = host_.minimise;
        if (host_.toggleMaximise) {
            auto toggle = host_.toggleMaximise;
            auto query = host_.isMaximised;
            wc->onMaximise = [wc, toggle, query] {
                toggle();
                if (query) wc->maximised = query();
            };
        }
    }

    if (reader_ && host_.copyText) {
        auto copy = host_.copyText;
        reader_->onCopyText = [copy](const std::string& t) { copy(t); };
    }
}

void FolioApp::requestOpenFile() {
    // Prefer the host's own dialog; fall back to the in-app browser on the
    // platforms that have none (web, iOS, and this streamed demo).
    FilePicker& fp = FilePicker::current();
    if (fp.isNative()) {
        fp.requestOpen([this](std::string path) {
            if (path.empty()) return;
            if (openDlg_->onOpen) openDlg_->onOpen(path);
        });
        return;
    }
    openDlg_->open(lastNow_);
}

void FolioApp::resize(float w, float h) {
    root_.setFrame(0.f, 0.f, w, h);
    welcome_->setFrame(0.f, 0.f, w, h);
    reader_->setFrame(0.f, 0.f, w, h);
    openDlg_->setFrame(0.f, 0.f, w, h);
    overlay_->setFrame(0.f, 0.f, w, h);
    root_.layoutTree();
    forceFrames_ = 2;
}

void FolioApp::showScreen(Screen s, double now) {
    if (s == target_) return;
    target_ = s;
    state_.screen = s;
    // Both screens must be visible during the cross-fade; the one being
    // left is hidden again once the fade completes (in paint).
    welcome_->visible = true;
    reader_->visible = true;
    screenFade_.animateTo(s == Screen::Reader ? 1.f : 0.f, tokens::dur::screenSwap, now);
}

bool FolioApp::pointer(const PointerEvent& e) {
    lastNow_ = e.timestamp;
    Widget::setClock(e.timestamp);
    forceFrames_ = std::max(forceFrames_, 1);
    // A press anywhere must first give open popovers a chance to dismiss,
    // before the router hands the event to whatever is underneath.
    if (e.phase == PointerPhase::Down && target_ == Screen::Reader) {
        reader_->handleOutsideClick(e.windowPos, e.timestamp);
    }
    return router_.dispatchPointer(root_, e);
}

bool FolioApp::key(const KeyEvent& e) {
    Widget::setClock(lastNow_);
    if (openDlg_->isOpen()) return openDlg_->onKey(e);
    forceFrames_ = std::max(forceFrames_, 1);
    if (router_.dispatchKey(root_, e)) return true;
    // Unhandled keys fall through to the active screen as shortcuts.
    if (target_ == Screen::Reader) return reader_->onKey(e);
    return false;
}

bool FolioApp::needsFrame(double now) const {
    if (forceFrames_ > 0) return true;
    if (screenFade_.active(now)) return true;
    if (state_.theme.animating(now)) return true;
    return root_.treeAnimating(now);
}

FolioApp::Stats FolioApp::stats(double now) const {
    Stats s;
    const_cast<Widget&>(root_).forEach([&](Widget&) { ++s.widgets; });
    s.animating = needsFrame(now);
    s.damage = lastDamage_;
    s.partial = lastPartial_;
    s.driver = damageDriver_;
    return s;
}

namespace {

void collectDamage(const Widget& w, double now, Point origin, Rect& out, bool& any,
                   std::string* driver, float* widest) {
    const Point o{origin.x + w.frame.x, origin.y + w.frame.y};
    if (w.visible && w.animating(now)) {
        const Rect d = w.damageBounds();
        if (d.w > 0.f) {
            const Rect r{o.x + d.x, o.y + d.y, d.w, d.h};
            if (driver && widest && r.w * r.h > *widest) {
                *widest = r.w * r.h;
                *driver = w.debugName.empty() ? "(unnamed)" : w.debugName;
            }
            if (!any) {
                out = r;
                any = true;
            } else {
                const float x0 = std::min(out.x, r.x), y0 = std::min(out.y, r.y);
                const float x1 = std::max(out.right(), r.right());
                const float y1 = std::max(out.bottom(), r.bottom());
                out = Rect{x0, y0, x1 - x0, y1 - y0};
            }
        }
    }
    if (!w.visible) return;
    for (const auto& c : w.children())
        collectDamage(*c, now, o, out, any, driver, widest);
}

}  // namespace

Rect FolioApp::computeDamage(double now) const {
    Rect r{};
    bool any = false;
    float widest = 0.f;
    collectDamage(root_, now, Point{0.f, 0.f}, r, any,
                  &const_cast<FolioApp*>(this)->damageDriver_, &widest);
    if (!any) return Rect{};
    // Shadows and focus rings bleed outside a widget's own box.
    return Rect{r.x - 24.f, r.y - 24.f, r.w + 48.f, r.h + 48.f};
}

void FolioApp::paint(IPaintTarget& t, double now, float dpr) {
    lastNow_ = now;
    if (forceFrames_ > 0) --forceFrames_;

    // --- partial repaint: REMOVED -------------------------------------------
    // A damage-rectangle pass lived here. Its premise was that the set of
    // widgets reporting animating() is the set of pixels that change, and
    // that is simply false: a widget moves when some *other* widget's
    // animation drives a relayout. Collapsing a panel animates the panel,
    // but it is the stage, the pill and the thumbnails that change position,
    // and none of them are "animating", so none of them were repainted.
    // Stale rectangles everywhere layout moved - exactly as reported.
    //
    // Doing it properly means tracking each widget's previously painted rect
    // and damaging both old and new, plus forcing a full frame on every
    // layoutTree(). Worth building; not worth shipping half-built. A
    // renderer that is fast and wrong is worse than one that is correct.
    const bool partial = false;
    (void)partial;
    lastDamage_ = Rect{};
    lastPartial_ = false;

    // During a theme change the frame is painted twice: once in the
    // outgoing palette, then again in the incoming one clipped to a growing
    // circle. That is the prototype's reveal, and it costs a second tree
    // traversal for the ~0.8s the animation lasts.
    const bool revealing = state_.theme.animating(now);
    const tokens::Palette pal =
        revealing ? state_.theme.outgoing() : state_.theme.effective(now);
    const PaintCtx ctx{pal, now, dpr};

    const float f = screenFade_.value(now);

    // Hide the screen that is fully off, so it stops costing paint time and
    // stops receiving hit tests.
    if (!screenFade_.active(now)) {
        welcome_->visible = (f < 0.5f);
        reader_->visible = (f >= 0.5f);
    }

    t.clear(pal.bg);

    // Cross-fade with a slight counter-slide: the outgoing screen drifts
    // left and the incoming one arrives from the right, which reads as
    // "forward" navigation rather than a dissolve.
    const float slide = 28.f;
    if (welcome_->visible) {
        welcome_->opacity = 1.f - f;
        welcome_->frame.x = -slide * f;
    }
    if (reader_->visible) {
        reader_->opacity = f;
        reader_->frame.x = slide * (1.f - f);
    }
    overlay_->opacity = 1.f;
    overlay_->frame.x = 0.f;

    root_.paintTree(t, ctx);

    if (revealing) {
        const float width = root_.frame.w, height = root_.frame.h;
        const float p = state_.theme.progress(now);
        float ox = state_.theme.revealOriginX();
        float oy = state_.theme.revealOriginY();
        if (ox < 0.f) { ox = width - 90.f; oy = 30.f; }

        // The circle has to cover the furthest corner, or the old theme is
        // left in a sliver at the far edge when the animation ends.
        const float dx = std::max(ox, width - ox);
        const float dy = std::max(oy, height - oy);
        const float rMax = std::sqrt(dx * dx + dy * dy);
        const float r = rMax * p;

        t.save();
        t.clipRoundedRect(Rect{ox - r, oy - r, r * 2.f, r * 2.f},
                          Corners::all(r), true);
        const tokens::Palette np = state_.theme.incoming();
        const PaintCtx nctx{np, now, dpr};
        t.fillRect(Rect{0.f, 0.f, width, height}, np.bg);
        root_.paintTree(t, nctx);
        t.restore();

        // A hairline on the leading edge. Without it the boundary between
        // two similar-luminance palettes is invisible and the whole effect
        // reads as a flicker.
        t.strokeRoundedRect(Rect{ox - r, oy - r, r * 2.f, r * 2.f},
                            Corners::all(r),
                            fade(np.accent, 0.5f * (1.f - p)), 2.f);
    }
}

}  // namespace folio::ui
