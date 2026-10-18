// folio-ui/shell/NativeShell.cpp
//
// The desktop shell. One file, every platform - which is the whole point of
// folio-platform: this translation unit contains no `#ifdef _WIN32`.
//
// Its job, in FolioApp's own words: create a surface, pump events into
// pointer/key, call paint when needsFrame says so, and present.
//
// THE IDLE RULE
// -------------
// A PDF reader doing nothing must cost nothing. The loop blocks in
// `waitEvents` until the user acts, polling PDFium completions at idle cadence.
// In-flight CPU parsing/rasterisation does not require GPU frames; completed
// results and live animations do. "60 FPS" means 60 FPS while moving, not
// a permanently spinning render thread.

#include "NativeShell.h"

#include "components/OpenDialog.h"
#include "folio/platform/Platform.h"
#include "screens/FolioApp.h"
#if FOLIO_HAVE_PDFIUM_SOURCE
#include "pdfium/PdfiumAnnotationWriter.h"
#include "pdfium/FileMappedAccess.h"
#endif
#include "skia/SkiaGpuTarget.h"
#include "skia/SkiaPaintTarget.h"
#include "skia/SkiaTextSystem.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#if defined(__GLIBC__) && !defined(__ANDROID__)
#include <malloc.h>
#endif
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

namespace folio::ui {
namespace {

namespace plat = folio::platform;

/// Where the fonts live at run time.
///
/// FOLIO_ASSET_DIR is baked in at configure time and points into the source
/// tree, which is right for a developer build and wrong for anything that
/// gets copied elsewhere - a CI artifact downloaded onto another machine
/// has no such path, and the app would start with no fonts at all. Look
/// beside the executable first, then one level up (the usual bin/ layout),
/// and only then fall back to the compiled-in path.
std::filesystem::path executablePath() {
    namespace fs = std::filesystem;
    std::error_code ec;
#if defined(_WIN32)
    std::wstring buf(1024, L'\0');
    const DWORD n =
        GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    buf.resize(n);
    return fs::path(buf);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    return fs::weakly_canonical(fs::path(buf.c_str()), ec);
#else
    return fs::canonical("/proc/self/exe", ec);
#endif
}

std::string resolveAssetDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> candidates;
    const fs::path exe = executablePath();
    if (!exe.empty()) {
        candidates.push_back(exe.parent_path() / "assets");
        candidates.push_back(exe.parent_path().parent_path() / "assets");
    }
    candidates.emplace_back(FOLIO_ASSET_DIR);
    for (const fs::path& c : candidates) {
        if (fs::is_directory(c / "fonts", ec)) return c.string();
    }
    return std::string(FOLIO_ASSET_DIR);
}

/// Platform key -> UI key. Both enums are small and hand-maintained; a
/// switch makes an omission a compiler warning rather than a silent mismap.
Key toUiKey(plat::Key k) {
    switch (k) {
        case plat::Key::A: return Key::A;
        case plat::Key::C: return Key::C;
        case plat::Key::F: return Key::F;
        case plat::Key::N: return Key::N;
        case plat::Key::O: return Key::O;
        case plat::Key::P: return Key::P;
        case plat::Key::S: return Key::S;
        case plat::Key::T: return Key::T;
        case plat::Key::W: return Key::W;
        case plat::Key::Z: return Key::Z;
        case plat::Key::Y: return Key::Y;
        case plat::Key::Left:      return Key::Left;
        case plat::Key::Right:     return Key::Right;
        case plat::Key::Up:        return Key::Up;
        case plat::Key::Down:      return Key::Down;
        case plat::Key::PageUp:    return Key::PageUp;
        case plat::Key::PageDown:  return Key::PageDown;
        case plat::Key::Home:      return Key::Home;
        case plat::Key::End:       return Key::End;
        case plat::Key::Enter:     return Key::Enter;
        case plat::Key::Escape:    return Key::Escape;
        case plat::Key::Tab:       return Key::Tab;
        case plat::Key::Backspace: return Key::Backspace;
        case plat::Key::Delete:    return Key::Delete;
        case plat::Key::Space:     return Key::Space;
        case plat::Key::Plus:      return Key::Plus;
        case plat::Key::Minus:     return Key::Minus;
        default: return Key::Unknown;
    }
}

MouseButton toUiButton(plat::Button b) {
    switch (b) {
        case plat::Button::Left:   return MouseButton::Left;
        case plat::Button::Right:  return MouseButton::Right;
        case plat::Button::Middle: return MouseButton::Middle;
        default: return MouseButton::None;
    }
}

double seconds() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// Native file picker
//
// Implements the FilePicker seam that FolioApp::requestOpenFile already
// checks. `isNative()` is false when there is no reachable host dialog, and
// the app then shows its own OpenDialog - so a bare X session without zenity
// still opens files.
// ---------------------------------------------------------------------------

class NativePicker final : public FilePicker {
public:
    [[nodiscard]] bool isNative() const override {
#if defined(__ANDROID__)
        return true; // SAF is async; never fall back to sandboxed path browsing
#else
        return plat::fileDialog().available();
#endif
    }

    void requestOpen(std::function<void(std::string)> done) override {
#if defined(__ANDROID__)
        pending_ = std::move(done);
        if (!plat::requestAndroidPdfPicker()) complete({});
        return;
#endif
        plat::FileDialogRequest req;
        req.title = "Open a PDF";
        req.startDirectory = defaultDirectory();
        req.filters.push_back({"PDF documents", {"*.pdf"}});
        auto chosen = plat::fileDialog().openFile(req);
        if (done) done(chosen.value_or(std::string{}));
    }

    void requestSave(const std::string& suggested,
                     std::function<void(std::string)> done) override {
#if defined(__ANDROID__)
        // SAF export has a separate asynchronous URI bridge, not a path.
        (void)suggested;
        if (done) done({});
#else
        plat::FileDialogRequest req;
        req.title = "Save a PDF copy";
        req.startDirectory = defaultDirectory();
        req.suggestedName = suggested;
        req.filters.push_back({"PDF documents", {"*.pdf"}});
        auto chosen = plat::fileDialog().saveFile(req);
        if (done) done(chosen.value_or(std::string{}));
#endif
    }

    [[nodiscard]] std::vector<FileEntry> browse(const std::string& dir) override {
        // Reached when isNative() is false and the in-app dialog is doing
        // the browsing. Delegate to `local()`, NOT to `current()` - this
        // object *is* current(), and that recursion segfaults instantly.
        return FilePicker::local().browse(dir);
    }

    [[nodiscard]] std::string defaultDirectory() const override {
        return FilePicker::local().defaultDirectory();
    }

    bool complete(std::string path) {
        if (!pending_) return false;
        auto callback = std::move(pending_);
        pending_ = {};
        callback(std::move(path));
        return true;
    }
private:
    std::function<void(std::string)> pending_;
};

}  // namespace

int runNativeShell(const NativeShellOptions& opts) {
    plat::WindowDesc desc;
    desc.title = "Folio";
    desc.width = opts.width;
    desc.height = opts.height;
    desc.startMaximised = opts.startMaximised;

    std::string err;
    auto window = plat::createWindow(desc, &err);
    if (!window) {
        std::fprintf(stderr,
                     "folio: cannot open a window (%s backend): %s\n"
                     "       Run folio-stream instead for a headless session.\n",
                     plat::backendName(), err.c_str());
        return 2;
    }

    // ---- GPU, with a software fallback -------------------------------------
    // Machines with no usable GL are rare but real: remote sessions, broken
    // drivers, VMs. Falling back costs frame rate; refusing to start costs
    // the user their document.
    render::SkiaGpuTarget gpu;
    bool useGpu = !opts.forceSoftware;
    if (useGpu) {
        std::string gpuErr;
        if (!gpu.initialise(window->gl(), &gpuErr)) {
            std::fprintf(stderr, "folio: GPU unavailable (%s); using software\n",
                         gpuErr.c_str());
            useGpu = false;
        }
    }
    sk_sp<SkSurface> raster;

    const std::string assetDir =
        opts.assetDir.empty() ? resolveAssetDir() : opts.assetDir;
#if defined(_WIN32)
    // A copied Folio.exe without its ZIP's assets can initially draw using
    // host fonts, disguising an incomplete installation until PDF export or
    // another feature needs the bundled fonts. Explain the broken layout.
    if (!std::filesystem::is_regular_file(
            std::filesystem::u8path(assetDir) / "fonts" / "DMSans-Variable.ttf")) {
        MessageBoxW(nullptr,
                    L"Folio's bundled fonts are missing. Extract the WHOLE "
                    L"Windows ZIP and run Folio.exe beside pdfium.dll and "
                    L"the assets folder; do not move only the EXE.",
                    L"Incomplete Folio folder", MB_OK | MB_ICONWARNING);
    }
#endif
    render::skia::SkiaTextSystem::instance().initialise(assetDir + "/fonts");
#if FOLIO_HAVE_PDFIUM_SOURCE
    engine::setPdfiumExportFontDirectory(assetDir + "/fonts");
#if defined(__ANDROID__)
    engine::setLinkedPdfOpener([](const std::string& path) {
        return plat::openAndroidLinkedPdfFd(path);
    });
#endif
#endif

    NativePicker picker;
    FilePicker::setCurrent(&picker);

    float dpr = window->scale();
    int pxW = window->widthPx();
    int pxH = window->heightPx();
    // Pixel space inset in the native EGL surface, not a second Android view
    // padding. Zero when the OS has already placed that surface below the bar.
    int safeTopPx = 0;

    FolioApp app(static_cast<float>(pxW) / dpr, static_cast<float>(pxH) / dpr);
    namespace fs = std::filesystem;
    fs::path stateRoot = opts.stateDir;
    if (stateRoot.empty()) {
#if defined(_WIN32)
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (length) {
            std::wstring buf(length, L'\0');
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf.data(), length))
                stateRoot = fs::path(buf.c_str()) / "Folio";
        }
#else
        if (const char* p = std::getenv("XDG_STATE_HOME"))
            stateRoot = fs::path(p) / "folio";
        else if (const char* p = std::getenv("HOME"))
            stateRoot = fs::path(p) / ".local/state/folio";
#endif
    }
    if (!stateRoot.empty()) {
        const auto file = (stateRoot / "recent-v1.txt").u8string();
        app.loadRecents(std::string(file.begin(), file.end()));
    }
    if (!opts.openPath.empty()) app.openFileFromHost(opts.openPath);

    // ---- Hand the app the host's services ---------------------------------
    // These are the seams that have been declared and empty since round 3.
    FolioApp::HostServices host;
    host.minimise       = [&] { window->minimise(); };
    host.wake           = [&] { window->wake(); };
    host.toggleMaximise = [&] { window->toggleMaximise(); };
    host.close          = [&] { window->requestClose(); };
    host.beginDragMove  = [&] { window->beginDragMove(); };
    host.isMaximised    = [&] { return window->maximised(); };
    host.copyText = [](const std::string& s) { plat::clipboard().setText(s); };
    host.pasteText = [] { return plat::clipboard().text(); };
    host.setTitle = [&](const std::string& t) { window->setTitle(t); };
#if defined(__ANDROID__)
    host.showKeyboard = [](bool visible) { plat::showAndroidKeyboard(visible); };
    host.exportPdf = [](const std::string& staged, const std::string& name) {
        return plat::requestAndroidPdfExport(staged, name);
    };
    host.sharePdf = [](const std::string& staged, const std::string& name) {
        return plat::requestAndroidPdfShare(staged, name);
    };
    host.storageAction = [](const std::string& path, int action) {
        return plat::requestAndroidStorageAction(path, action);
    };
    host.prefersSourceOnly = [] { return plat::androidPrefersSourceOnly(); };
    host.setPrefersSourceOnly = [](bool value) { plat::setAndroidPrefersSourceOnly(value); };
#endif
    app.setHostServices(std::move(host));

    bool dirty = true;
    bool minimized = false;
    bool idleTrimmed = false;
    double lastInput = seconds();
    double lastPresented = -1.0;
#if defined(__ANDROID__)
    bool systemBarsConfigured = false;
    int lastSystemBarsColor = -1;
    bool lastSystemBarsDark = false;
    double androidInputOrigin = 0.0;
    double androidUiOrigin = 0.0;
#endif
    auto releaseVisuals = [&] {
        app.releaseVisualCaches();
        // Clear Skia's copied image and raster caches before the GPU context
        // releases its retained full-window surface.
        if (useGpu) {
            if (auto* target = gpu.begin()) target->invalidateRasterCache();
            gpu.purgeResources();
        } else {
            render::skia::SkiaPaintTarget scratch(nullptr);
            scratch.invalidateRasterCache();
        }
        raster.reset();
        dirty = false;
        minimized = true;
        idleTrimmed = true;
        // Best effort: give the allocator unused free spans back to the OS.
#if defined(__GLIBC__) && !defined(__ANDROID__)
        malloc_trim(0);
#elif defined(_WIN32)
        HeapSetInformation(GetProcessHeap(), HeapOptimizeResources, nullptr, 0);
#endif
    };
    while (!window->shouldClose()) {
#if defined(__ANDROID__)
        const double loopStart = seconds();
#endif
        plat::Event e;
        while (window->poll(e)) {
            switch (e.type) {
                case plat::Event::Type::Minimized:
                    releaseVisuals();
                    break;
                case plat::Event::Type::Close:
                    break;

                case plat::Event::Type::Resize:
                    minimized = false;
                    idleTrimmed = false;
                    pxW = e.w;
                    pxH = e.h;
                    app.resize(static_cast<float>(pxW) / dpr,
                               static_cast<float>(std::max(1, pxH - safeTopPx)) / dpr);
                    dirty = true;
                    break;

                case plat::Event::Type::ScaleChange:
                    dpr = e.scale;
                    app.resize(static_cast<float>(pxW) / dpr,
                               static_cast<float>(std::max(1, pxH - safeTopPx)) / dpr);
                    dirty = true;
                    break;

                case plat::Event::Type::SafeArea: {
                    // The SurfaceView can already be below the status bar on
                    // some devices. Java reports only the uncovered overlap.
                    const int next = std::clamp(e.h, 0, std::max(0, pxH - 1));
                    if (safeTopPx != next) {
                        safeTopPx = next;
                        app.resize(static_cast<float>(pxW) / dpr,
                                   static_cast<float>(std::max(1, pxH - safeTopPx)) / dpr);
                        dirty = true;
                    }
                    break;
                }

                case plat::Event::Type::Expose:
                    dirty = true;
                    break;

                case plat::Event::Type::PointerDown:
                case plat::Event::Type::PointerUp:
                case plat::Event::Type::PointerMove:
                case plat::Event::Type::PointerCancel:
                case plat::Event::Type::PointerLeave: {
                    lastInput = seconds();
                    idleTrimmed = false;
                    PointerEvent pe;
                    pe.phase = e.type == plat::Event::Type::PointerDown
                                   ? PointerPhase::Down
                               : e.type == plat::Event::Type::PointerUp
                                   ? PointerPhase::Up
                               : e.type == plat::Event::Type::PointerMove
                                   ? PointerPhase::Move
                               : e.type == plat::Event::Type::PointerCancel
                                   ? PointerPhase::Cancel
                                   : PointerPhase::Leave;
                    pe.pos = {static_cast<float>(e.x) / dpr,
                              static_cast<float>(e.y - safeTopPx) / dpr};
                    pe.windowPos = pe.pos;
                    pe.button = toUiButton(e.button);
                    pe.modifiers = e.mods;
                    pe.clickCount = e.clickCount;
                    pe.pointerType =
                        e.source == plat::PointerSource::Touch ? PointerType::Touch
                        : e.source == plat::PointerSource::Pen ? PointerType::Pen
                                                               : PointerType::Mouse;
                    pe.pointerId = e.pointerId;
                    pe.pressure = e.pressure;
                    pe.tiltX = e.tiltX;
                    pe.tiltY = e.tiltY;
                    // A mouse has no pressure axis. Report full pressure on
                    // contact and none on release, so a width curve can read
                    // `pressure` without first asking what device sent it.
                    if (pe.pointerType == PointerType::Mouse) {
                        if (pe.phase == PointerPhase::Down) pe.pressure = 1.f;
                        else if (pe.phase == PointerPhase::Up) pe.pressure = 0.f;
                    }
#if defined(__ANDROID__)
                    if (e.inputTimeSeconds > 0.0) {
                        if (androidInputOrigin == 0.0) {
                            androidInputOrigin = e.inputTimeSeconds;
                            androidUiOrigin = seconds();
                        }
                        // Batched historical positions have distinct OS sample
                        // times. Assigning the same dequeue time to all of
                        // them made dt ~= 0 and manufactured a huge fling.
                        pe.timestamp = std::min(seconds(), androidUiOrigin +
                            e.inputTimeSeconds - androidInputOrigin);
                    } else
#endif
                    pe.timestamp = seconds();
                    const bool changed = app.pointer(pe);
                    if (e.type != plat::Event::Type::PointerMove || changed)
                        dirty = true;
                    break;
                }

                case plat::Event::Type::Wheel: {
                    lastInput = seconds();
                    idleTrimmed = false;
                    PointerEvent pe;
                    pe.phase = PointerPhase::Wheel;
                    pe.pos = {static_cast<float>(e.x) / dpr,
                              static_cast<float>(e.y - safeTopPx) / dpr};
                    pe.windowPos = pe.pos;
                    pe.wheelX = static_cast<float>(e.wheelX);
                    pe.wheelY = static_cast<float>(e.wheelY);
                    pe.modifiers = e.mods;
                    pe.timestamp = seconds();
                    app.pointer(pe);
                    dirty = true;
                    break;
                }

                case plat::Event::Type::KeyDown:
                case plat::Event::Type::KeyUp: {
                    lastInput = seconds();
                    idleTrimmed = false;
                    KeyEvent ke;
                    ke.key = toUiKey(e.key);
                    ke.pressed = e.type == plat::Event::Type::KeyDown;
                    ke.repeat = e.repeat;
                    ke.modifiers = e.mods;
                    app.key(ke);
                    dirty = true;
                    break;
                }

                case plat::Event::Type::StorageChanged:
                    app.storageChanged(e.w, e.text);
                    dirty = true;
                    break;
                case plat::Event::Type::FileExported:
                    app.exportFinished(e.w, e.text);
                    dirty = true;
                    break;

                case plat::Event::Type::FileOpened:
                    // Activity callbacks are delivered here on the native UI
                    // thread, never on the Java callback thread.
                    if (!picker.complete(e.text) && !e.text.empty())
                        app.openFileFromHost(e.text);
                    dirty = true;
                    break;

                case plat::Event::Type::Text: {
                    lastInput = seconds();
                    idleTrimmed = false;
                    // Composed text arrives as its own event so that dead
                    // keys and IMEs work; the key event above already fired.
                    KeyEvent ke;
                    ke.key = Key::Unknown;
                    ke.pressed = true;
                    ke.modifiers = e.mods;
                    ke.text = e.text;
                    app.key(ke);
                    dirty = true;
                    break;
                }

                default:
                    break;
            }
        }

#if defined(__ANDROID__)
        const double barNow = seconds();
        const auto& theme = app.state().theme;
        const auto bg = theme.effective(barNow).bg;
        const int barColor = static_cast<int>(0xff000000u |
            (static_cast<unsigned>(bg.r) << 16) |
            (static_cast<unsigned>(bg.g) << 8) | bg.b);
        const bool darkBars = theme.animating(barNow)
            ? (theme.scheme() == ColorScheme::Dark
                ? theme.progress(barNow) >= .5f : theme.progress(barNow) < .5f)
            : theme.scheme() == ColorScheme::Dark;
        if (!systemBarsConfigured || barColor != lastSystemBarsColor ||
            darkBars != lastSystemBarsDark) {
            plat::setAndroidSystemBarsColor(barColor, darkBars);
            lastSystemBarsColor = barColor;
            lastSystemBarsDark = darkBars;
            systemBarsConfigured = true;
        }
#endif
        const double now = seconds();
        if (!minimized && !idleTrimmed && now - lastInput >= 3.0) {
            app.trimIdleVisualCaches();
            if (useGpu) {
                if (auto* target = gpu.begin()) target->invalidateRasterCache();
            } else {
                render::skia::SkiaPaintTarget scratch(nullptr);
                scratch.invalidateRasterCache();
            }
#if defined(__GLIBC__) && !defined(__ANDROID__)
            malloc_trim(0);
#elif defined(_WIN32)
            HeapSetInformation(GetProcessHeap(), HeapOptimizeResources, nullptr, 0);
#endif
            idleTrimmed = true; // no recurring timer or periodic trim work
        }
        if (app.pollOpen()) dirty = true;
        if (app.pollExport()) dirty = true;
        // Do not rely on WGL swap interval: many Windows drivers disable it
        // globally. Keep full-window paints bounded even during animations.
#if defined(__ANDROID__)
        const bool frameDue = true; // Android has its own ~45 Hz budget below.
#else
        const bool frameDue = lastPresented < 0.0 || now - lastPresented >= 1.0 / 60.0;
#endif
        if (!minimized && (dirty || app.needsFrame(now)) && frameDue) {
            render::IPaintTarget* target = nullptr;
            std::optional<render::skia::SkiaPaintTarget> softTarget;

            if (useGpu) {
                if (gpu.resize(pxW, pxH, dpr)) target = gpu.begin();
                if (!target) {
                    useGpu = false;  // context lost; start a fresh software scene
                    app.invalidate();
                }
            }
            if (!target) {
                if (!raster || raster->width() != pxW || raster->height() != pxH) {
                    app.invalidate(); // a new backing bitmap has no prior frame
                    raster = SkSurfaces::Raster(SkImageInfo::Make(
                        pxW, pxH, kBGRA_8888_SkColorType, kPremul_SkAlphaType,
                        SkColorSpace::MakeSRGB()));
                }
                if (!raster) return 3;
                softTarget.emplace(raster->getCanvas());
                target = &*softTarget;
            }

            // The layout is in logical units (pixels / dpr) and touch input
            // is divided by dpr, but nothing scaled the canvas to match. The
            // UI was drawn at 1/dpr of its size in the top-left corner while
            // taps still landed relative to the whole screen. StreamShell
            // already does this scale; the native path never did. Save and
            // restore around it, because the GPU surface's canvas persists
            // between frames.
            target->save();
            if (dpr != 1.f) target->scale(dpr, dpr);
            if (safeTopPx > 0) {
                // The app owns a viewport BELOW the system clock even when
                // the OEM leaves NativeActivity's EGL surface at screen Y=0.
                // Clip in surface coordinates before translating the scene;
                // resize, paint damage and pointer conversion all use the
                // exact same safeTopPx value.
                const float top = static_cast<float>(safeTopPx) / dpr;
                target->fillRect({0.f, 0.f, static_cast<float>(pxW) / dpr, top},
                                 app.state().theme.effective(now).bg);
                target->clipRect({0.f, top, static_cast<float>(pxW) / dpr,
                                  static_cast<float>(pxH - safeTopPx) / dpr}, false);
                target->translate(0.f, top);
            }
            // GPU draws into a retained scene, software reuses its raster.
            // Both can repaint only dirty chrome without touching PDF pixels.
            app.paint(*target, now, dpr, true);
            target->restore();

            if (useGpu) {
                gpu.end();
                window->gl().swapBuffers();
            } else {
                // Hand the CPU pixels to the window directly. Going through
                // GL here would fail for the same reason Ganesh did.
                SkPixmap pm;
                if (raster->peekPixels(&pm)) {
                    window->presentSoftware(pm.addr(), pm.width(), pm.height(),
                                            static_cast<int>(pm.rowBytes()));
                }
            }
            dirty = false;
            lastPresented = seconds();
        }

#if defined(__ANDROID__)
        // EGL swap is not a reliable throttle on every Android driver (nor
        // in the CPU fallback). The small retained popup/chrome damage path
        // can follow a ~60 Hz cadence; full PDF/layout paints keep the
        // conservative ~45 Hz budget. Neither mode schedules idle frames.
        const int budgetMs = app.lastPaintPartial() ? 16 : 22;
        const int spentMs = static_cast<int>((seconds() - loopStart) * 1000.0);
        const double waitNow = seconds();
        window->waitEvents(minimized ? -1
            : app.needsFrame(waitNow) ? std::max(0, budgetMs - spentMs)
            : app.hasBackgroundWork() ? 100
            : idleTrimmed ? -1
            : std::max(1, static_cast<int>(std::ceil(
                  (lastInput + 3.0 - waitNow) * 1000.0))));
#else
        // Bound painting even if the driver ignores swap interval. During
        // PDFium work there is no frame to paint; poll its completion at 50ms.
        const double untilFrame = lastPresented + 1.0 / 60.0 - seconds();
        const double waitNow = seconds();
        const int waitMs = (dirty || (!minimized && app.needsFrame(waitNow)))
            ? std::max(1, static_cast<int>(std::ceil(untilFrame * 1000.0)))
            : minimized ? -1
            : app.hasBackgroundWork() ? 50 // only incomplete async open needs polling
            : idleTrimmed ? -1  // worker's thread-safe wake() delivers results
            : std::max(1, static_cast<int>(std::ceil(
                  (lastInput + 3.0 - waitNow) * 1000.0)));
        window->waitEvents(waitMs);
#endif
    }

    FilePicker::setCurrent(nullptr);
    return 0;
}

}  // namespace folio::ui
