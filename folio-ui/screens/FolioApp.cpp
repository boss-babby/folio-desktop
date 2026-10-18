#include "screens/FolioApp.h"
#if FOLIO_HAVE_PDFIUM_SOURCE
#include "pdfium/PdfiumPageSource.h"
#endif

#include <filesystem>
#include <system_error>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <exception>

namespace folio::ui {
namespace {
Rect joinDamage(Rect a, Rect b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const float x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.right(), b.right()) - x,
                   std::max(a.bottom(), b.bottom()) - y};
}
Rect widgetDamage(const Widget* w) {
    if (!w) return {};
    const Point p = w->toWindow({0.f, 0.f});
    const Rect r = w->damageBounds();
    return {p.x + r.x - 24.f, p.y + r.y - 24.f, r.w + 48.f, r.h + 48.f};
}
}  // namespace

FolioApp::FolioApp(float w, float h) {
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
        openFileFromHost(path);
    };

    reader_->visible = false;
    welcome_->onOpenDocument = [this](const std::string& path) {
        int resumePage = 1;
        for (const RecentDoc& r : state_.recents)
            if (r.path == path) {
                resumePage = std::clamp(
                    static_cast<int>(std::lround(r.progress * r.pages)) + 1,
                    1, r.pages);
                break;
            }
        queueOpen(OpenRequest{path, resumePage, true});
    };
    welcome_->onBrowse = [this] { requestOpenFile(); };
    welcome_->onRemoveDocument = [this](const std::string& path) {
        state_.forgetDocument(path); // not source, not private copy, not sidecars
        removedFromLibrary_ = path;
        welcome_->refreshRecents(lastNow_);
        overlay_->showToast("Removed from library; PDF and annotations retained", lastNow_);
        invalidate();
    };
    welcome_->onStorageAction = [this](const std::string& path, int action) {
        if (!host_.storageAction || !host_.storageAction(path, action))
            overlay_->showToast("Storage option unavailable for this file", lastNow_);
    };
    welcome_->onSetPreferSourceOnly = [this](bool value) {
        if (host_.setPrefersSourceOnly) host_.setPrefersSourceOnly(value);
    };
    reader_->onBrowse = [this] { requestOpenFile(); };
    // Extract asks where to put the result. Routed through the app rather
    // than the screen because the dialog lives on the overlay layer.
    reader_->onPickSavePath = [this](const std::string& suggested,
                                     std::function<void(std::string)> done) {
        FilePicker& picker = FilePicker::current();
#ifndef __ANDROID__
        if (picker.isNative()) {
            picker.requestSave(suggested, std::move(done));
            return;
        }
#endif
        openDlg_->onSave = [done](const std::string& p) { if (done) done(p); };
        openDlg_->openSave(suggested, lastNow_);
    };
    reader_->onDocumentChanged = [this] {
        purgeRasterCache_ = true;
        invalidate();
    };
    reader_->onBackToWelcome = [this] {
        showScreen(Screen::Welcome, lastNow_);
        welcome_->replayEntrance(lastNow_);
    };

    resize(w, h);
    welcome_->replayEntrance(0.0);
}

FolioApp::~FolioApp() {
    if (welcome_) welcome_->setCoverWakeCallback({});
    if (target_ == Screen::Reader && reader_ && !reader_->currentFilePath().empty() &&
        reader_->currentFilePath() != removedFromLibrary_)
        state_.rememberDocument(reader_->currentFilePath(),
                                reader_->stage()->source()->pageCount(),
                                reader_->stage()->currentPage());
    if (&OverlayHost::current() == overlay_) OverlayHost::setCurrent(nullptr);
}

bool FolioApp::hasBackgroundWork() const {
    return openTask_.valid() || (reader_ && reader_->hasDocumentWork()) ||
           (target_ == Screen::Welcome && welcome_ && welcome_->coversBusy());
}

void FolioApp::trimIdleVisualCaches() {
    if (reader_) reader_->trimIdleVisualCaches();
    if (welcome_) welcome_->trimIdleCovers();
}

void FolioApp::releaseVisualCaches() {
    if (reader_) reader_->releaseVisualCaches();
    if (welcome_) welcome_->releaseCoverMemory();
    purgeRasterCache_ = true;
    invalidate();
}

bool FolioApp::pollExport() {
    const bool changed = reader_ && reader_->pollExport();
    if (changed) fullRepaint_ = true; // a page raster may have landed
    return changed;
}

void FolioApp::storageChanged(int status, const std::string& message) {
    // Re-evaluate markers without changing path-based sidecar identity.
    if (status > 0 && !state_.recentStateFile.empty())
        state_.loadRecents(state_.recentStateFile);
    welcome_->refreshRecents(lastNow_);
    if (status != 0) overlay_->showToast(message, lastNow_);
    invalidate();
}

void FolioApp::exportFinished(int status, const std::string& message) {
    if (reader_) reader_->exportFinished(status, message);
    invalidate();
}

void FolioApp::setHostServices(HostServices s) {
    host_ = std::move(s);
    if (welcome_ && host_.prefersSourceOnly)
        welcome_->setPreferSourceOnly(host_.prefersSourceOnly());
    // The two close glyphs are app-level actions, not merely decorative.
    // They were never connected to the host's WM_CLOSE request.
    if (welcome_) {
        welcome_->onQuit = host_.close;
        welcome_->setCoverWakeCallback(host_.wake);
    }
    if (reader_) {
        reader_->onQuit = host_.close;
        reader_->setWorkerWakeCallback(host_.wake);
    }
    router_.onTextFocusChange = host_.showKeyboard;
    if (reader_) {
        reader_->setTextInputCallback(host_.showKeyboard);
        reader_->onExportPdf = host_.exportPdf;
        reader_->onSharePdf = host_.sharePdf;
    }

    // The window controls exist on both screens and must behave the same on
    // each, so they are wired here rather than in either screen. Close is
    // left to the screens' own onQuit, which already routes through the
    // app's shutdown path.
    for (WindowControls* wc :
         {welcome_ ? welcome_->windowControls() : nullptr,
          reader_ ? reader_->windowControls() : nullptr}) {
        if (!wc) continue;
        if (host_.isMaximised) wc->maximised = host_.isMaximised();
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

void FolioApp::recordOpen(const std::string& path) {
    removedFromLibrary_.clear();
    auto* source = reader_->stage()->source();
    if (!source) return;
    state_.rememberDocument(path, source->pageCount(),
                            reader_->stage()->currentPage());
    recentsDirty_ = true;
    purgeRasterCache_ = true;
}

void FolioApp::loadRecents(const std::string& path) {
    state_.loadRecents(path);
    reader_->syncFocusAutoHidePreference();
    welcome_->setRecentStateFile(path);
    welcome_->refreshRecents(lastNow_);
}

void FolioApp::startOpen(OpenRequest request) {
#if FOLIO_HAVE_PDFIUM_SOURCE
    openTask_ = std::async(std::launch::async, [request = std::move(request)]() mutable {
        OpenResult result;
        result.request = std::move(request);
        try {
            auto pdf = std::make_shared<folio::engine::PdfiumPageSource>();
            if (pdf->open(result.request.path)) result.source = std::move(pdf);
            else result.error = pdf->error();
        } catch (const std::exception& e) {
            result.error = e.what();
        }
        return result;
    });
    overlay_->showToast("Opening PDF in background…", lastNow_);
    invalidate();
#else
    if (reader_->openFile(request.path, lastNow_)) {
        recordOpen(request.path);
        showScreen(Screen::Reader, lastNow_);
    }
#endif
}

void FolioApp::queueOpen(OpenRequest request) {
    if (request.path.empty()) return;
    if (openTask_.valid()) {
        // The current parse cannot be interrupted safely inside PDFium.
        // Only remember the last user choice rather than spawning a pool of
        // expensive document parsers or blocking on future destruction.
        nextOpen_ = std::move(request);
        overlay_->showToast("Opening the latest selected PDF next…", lastNow_);
        return;
    }
    startOpen(std::move(request));
}

bool FolioApp::pollOpen() {
    using namespace std::chrono_literals;
    if (!openTask_.valid() || openTask_.wait_for(0ms) != std::future_status::ready)
        return false;
    OpenResult result = openTask_.get();
    if (nextOpen_) {
        OpenRequest next = std::move(*nextOpen_);
        nextOpen_.reset();
        startOpen(std::move(next));
        return true;
    }
    if (result.source &&
        reader_->openPreparedFile(result.request.path, std::move(result.source), lastNow_)) {
        reader_->stage()->goToPage(result.request.resumePage, lastNow_);
        recordOpen(result.request.path);
        showScreen(Screen::Reader, lastNow_);
    } else {
        overlay_->showToast("Could not open PDF: " +
                            (result.error.empty() ? std::string("invalid PDF") : result.error),
                            lastNow_);
        if (result.request.recent) {
            const auto it = std::find_if(state_.recents.begin(), state_.recents.end(),
                [&](const RecentDoc& r) { return r.path == result.request.path; });
            if (it != state_.recents.end() && it->linked) {
                it->unavailable = true; // preserve recents + sidecar for relink
                it->meta += " · Unavailable · Relink";
            } else state_.forgetDocument(result.request.path);
            recentsDirty_ = true;
        }
    }
    invalidate();
    return true;
}

void FolioApp::openFileFromHost(const std::string& path) {
    queueOpen(OpenRequest{path, 1, false});
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
    fullRepaint_ = true;
    forceFrames_ = 2;
}

void FolioApp::showScreen(Screen s, double now) {
    if (s == target_) return;
    if (target_ == Screen::Reader && s == Screen::Welcome) {
        if (!reader_->currentFilePath().empty() && reader_->stage()->source())
            state_.rememberDocument(reader_->currentFilePath(),
                                    reader_->stage()->source()->pageCount(),
                                    reader_->stage()->currentPage());
        recentsDirty_ = true;
        purgeRasterCache_ = true;
    }
    overlay_->dismissTooltip();
    target_ = s;
    welcome_->setCoverRequestsEnabled(s == Screen::Welcome);
    fullRepaint_ = true;
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
    // The native Windows frame is intentionally hidden behind this UI. An
    // empty part of the integrated title bar is still a real drag handle;
    // never steal a tab, home button, window control or an open popover.
    // The host performs the OS move/resize loop (Snap still works).
    if (host_.beginDragMove && e.pointerType == PointerType::Mouse &&
        e.phase == PointerPhase::Down && e.button == MouseButton::Left &&
        e.windowPos.y >= 6.f && e.windowPos.y < tokens::size::titleBar &&
        !openDlg_->isOpen() &&
        !(target_ == Screen::Reader && state_.focusMode) &&
        root_.hitTest(e.windowPos, 0.f) == nullptr) {
        if (e.clickCount >= 2 && host_.toggleMaximise) {
            host_.toggleMaximise();
            for (WindowControls* wc : {welcome_->windowControls(),
                                       reader_->windowControls()})
                if (wc && host_.isMaximised) wc->maximised = host_.isMaximised();
        } else {
            host_.beginDragMove();
        }
        fullRepaint_ = true;
        return true;
    }
    // Do not wake the renderer for mouse moves within a stationary widget;
    // a 1000 Hz mouse otherwise repaints the entire GL framebuffer at 1000 Hz.
    // A hover transition or a consumed drag still needs a frame.
    // A press anywhere must first give open popovers a chance to dismiss,
    // before the router hands the event to whatever is underneath.
    if (target_ == Screen::Reader) {
        reader_->noteFocusInteraction(e);
        if (e.phase == PointerPhase::Down)
            reader_->handleOutsideClick(e.windowPos, e.timestamp);
    } else if (target_ == Screen::Welcome && e.phase == PointerPhase::Down) {
        welcome_->dismissSettingsIfOutside(e.windowPos, e.timestamp);
    }
    // A pointer handler can rebuild a list and delete the previously hovered
    // widget. Capture its rectangle now; never inspect that raw pointer after
    // dispatching a click/move into user callbacks.
    const Rect beforeDamage = e.phase == PointerPhase::Move && !e.isTouch()
        ? widgetDamage(router_.hovered()) : Rect{};
    const bool changed = router_.dispatchPointer(root_, e);
    if (changed || e.phase != PointerPhase::Move) {
        forceFrames_ = std::max(forceFrames_, 1);
        if (e.phase == PointerPhase::Move && e.isTouch() &&
            target_ == Screen::Reader &&
            reader_->stage()->thumbOverlayOnly(e.timestamp)) {
            // Moving across the thumb palette changes only its small HUD.
            // A page pan/zoom is never classified as thumbOverlayOnly.
            pointerDamage_ = joinDamage(pointerDamage_, widgetDamage(reader_->stage()));
        } else if (e.phase == PointerPhase::Move && !e.isTouch()) {
            pointerDamage_ = joinDamage(pointerDamage_, beforeDamage);
            pointerDamage_ = joinDamage(pointerDamage_, widgetDamage(router_.hovered()));
            if (router_.hovered() && router_.hovered()->isDocumentCanvas())
                fullRepaint_ = true; // scrollbar or page gesture is not chrome
        } else {
            fullRepaint_ = true;
        }
    }
    return changed;
}

bool FolioApp::key(const KeyEvent& e) {
    fullRepaint_ = true;
    Widget::setClock(lastNow_);
    if (openDlg_->isOpen()) return openDlg_->onKey(e);
    forceFrames_ = std::max(forceFrames_, 1);
    if (router_.dispatchKey(root_, e)) return true;
    // Unhandled keys fall through to the active screen as shortcuts.
    if (target_ == Screen::Reader) return reader_->onKey(e);
#if defined(__ANDROID__)
    // NativeActivity consumes the Android Back event. If nothing in the
    // library is dismissible, finish the activity instead of trapping Back.
    if (e.pressed && e.key == Key::Escape && host_.close) {
        host_.close();
        return true;
    }
#endif
    return false;
}

bool FolioApp::needsFrame(double now) const {
    if (target_ == Screen::Welcome && welcome_->coversReady()) return true;
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

void FolioApp::paint(IPaintTarget& t, double now, float dpr, bool retained) {
    lastNow_ = now;
    if (purgeRasterCache_) {
        t.invalidateRasterCache();
        purgeRasterCache_ = false;
    }
    if (recentsDirty_) {
        recentsDirty_ = false;
        welcome_->refreshRecents(now);
    }
    // A finished cover changes pixels without an animation or pointer event;
    // repaint the library only when one actually becomes available.
    if (welcome_->pollCovers() && target_ == Screen::Welcome) fullRepaint_ = true;
    if (forceFrames_ > 0) --forceFrames_;

    // Partial painting is safe only on a retained surface. A WGL swap buffer
    // is NOT retained; NativeShell paints into a persistent Ganesh surface
    // and blits that complete image to the swap buffer. Large/layout/page
    // animations deliberately take the full path so moved widgets cannot
    // leave stale rectangles behind.
    const Rect moving = computeDamage(now);
    Rect damage = joinDamage(joinDamage(prevDamage_, moving), pointerDamage_);
    const float x0 = std::max(0.f, damage.x), y0 = std::max(0.f, damage.y);
    const float x1 = std::min(root_.frame.w, damage.right());
    const float y1 = std::min(root_.frame.h, damage.bottom());
    damage = x1 > x0 && y1 > y0 ? Rect{x0, y0, x1 - x0, y1 - y0} : Rect{};
    // On a retained surface a bounded popup over a *static* page can
    // repaint just the covered page tiles under its changing clip. A pure
    // chrome hover still skips the document completely. Zoom/scroll/layout
    // animations remain full-frame, so no stale page pixels are exposed.
    const Point canvasOrigin = reader_->stage()->toWindow({0.f, 0.f});
    const Rect canvas{canvasOrigin.x, canvasOrigin.y,
                      reader_->stage()->frame.w, reader_->stage()->frame.h};
    const bool damageTouchesCanvas = reader_->visible && !damage.empty() &&
        damage.x < canvas.right() && damage.right() > canvas.x &&
        damage.y < canvas.bottom() && damage.bottom() > canvas.y;
    const float maxDamageFraction = damageTouchesCanvas ? .55f : .30f;
    const bool partial = retained && !fullRepaint_ && forceFrames_ <= 1 &&
        !state_.theme.animating(now) && !screenFade_.active(now) &&
        (!reader_->visible || !reader_->requiresFullFrame(now)) &&
        !damage.empty() &&
        damage.w * damage.h < root_.frame.w * root_.frame.h * maxDamageFraction;
    prevDamage_ = moving;
    pointerDamage_ = {};
    fullRepaint_ = false;
    lastPartial_ = partial;
    lastDamage_ = partial ? damage : Rect{0.f, 0.f, root_.frame.w, root_.frame.h};

    // One hard-edged clip, no ring, buffer, or gradient. Day reverses the
    // geometry: paint light first, then the old dark inside a shrinking
    // circle. Night paints light first and reveals dark outward.
    const bool revealing = state_.theme.animating(now);
    const bool dayInward = revealing &&
        state_.theme.scheme() == ColorScheme::Light;
    const tokens::Palette pal = revealing
        ? (dayInward ? state_.theme.incoming() : state_.theme.outgoing())
        : state_.theme.effective(now);
    const PaintCtx ctx{pal, now, dpr, partial ? damage : Rect{},
                       partial && !damageTouchesCanvas};
    if (partial) {
        t.save();
        t.clipRect(damage, false);
    }

    const float f = screenFade_.value(now);

    // Hide the screen that is fully off, so it stops costing paint time and
    // stops receiving hit tests.
    if (!screenFade_.active(now)) {
        welcome_->visible = (f < 0.5f);
        reader_->visible = (f >= 0.5f);
    }

    if (partial) t.fillRect({0.f, 0.f, root_.frame.w, root_.frame.h}, pal.bg);
    else t.clear(pal.bg);

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
        const float dx = std::max(ox, width - ox);
        const float dy = std::max(oy, height - oy);
        const float rMax = std::sqrt(dx * dx + dy * dy);
        const float r = rMax * (dayInward ? 1.f - p : p);
        t.save();
        t.clipRoundedRect(Rect{ox - r, oy - r, r * 2.f, r * 2.f},
                          Corners::all(r), true);
        const tokens::Palette clipPal = dayInward
            ? state_.theme.outgoing() : state_.theme.incoming();
        t.fillRect(Rect{0.f, 0.f, width, height}, clipPal.bg);
        root_.paintTree(t, PaintCtx{clipPal, now, dpr});
        t.restore();
    }
    if (partial) t.restore();
}

}  // namespace folio::ui
