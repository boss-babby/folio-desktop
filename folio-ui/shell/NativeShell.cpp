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
// `waitEvents` until the user acts or an engine worker wakes it, and only
// free-runs while `needsFrame` is true - an animation in flight or a page
// raster outstanding. "60 FPS" means 60 FPS while something is moving, not
// a permanently spinning render thread.

#include "NativeShell.h"

#include "components/OpenDialog.h"
#include "folio/platform/Platform.h"
#include "screens/FolioApp.h"
#include "skia/SkiaGpuTarget.h"
#include "skia/SkiaPaintTarget.h"
#include "skia/SkiaTextSystem.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
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
        return plat::fileDialog().available();
    }

    void requestOpen(std::function<void(std::string)> done) override {
        plat::FileDialogRequest req;
        req.title = "Open a PDF";
        req.startDirectory = defaultDirectory();
        req.filters.push_back({"PDF documents", {"*.pdf"}});
        auto chosen = plat::fileDialog().openFile(req);
        if (done) done(chosen.value_or(std::string{}));
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
};

}  // namespace

int runNativeShell(const NativeShellOptions& opts) {
    plat::WindowDesc desc;
    desc.title = "Folio";
    desc.width = opts.width;
    desc.height = opts.height;

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

    render::skia::SkiaTextSystem::instance().initialise(resolveAssetDir() +
                                                        "/fonts");

    NativePicker picker;
    FilePicker::setCurrent(&picker);

    float dpr = window->scale();
    int pxW = window->widthPx();
    int pxH = window->heightPx();

    FolioApp app(static_cast<float>(pxW) / dpr, static_cast<float>(pxH) / dpr);

    // ---- Hand the app the host's services ---------------------------------
    // These are the seams that have been declared and empty since round 3.
    FolioApp::HostServices host;
    host.minimise       = [&] { window->minimise(); };
    host.toggleMaximise = [&] { window->toggleMaximise(); };
    host.close          = [&] { window->requestClose(); };
    host.beginDragMove  = [&] { window->beginDragMove(); };
    host.isMaximised    = [&] { return window->maximised(); };
    host.copyText = [](const std::string& s) { plat::clipboard().setText(s); };
    host.pasteText = [] { return plat::clipboard().text(); };
    host.setTitle = [&](const std::string& t) { window->setTitle(t); };
    app.setHostServices(std::move(host));

    bool dirty = true;
    while (!window->shouldClose()) {
        plat::Event e;
        while (window->poll(e)) {
            switch (e.type) {
                case plat::Event::Type::Close:
                    break;

                case plat::Event::Type::Resize:
                    pxW = e.w;
                    pxH = e.h;
                    app.resize(static_cast<float>(pxW) / dpr,
                               static_cast<float>(pxH) / dpr);
                    dirty = true;
                    break;

                case plat::Event::Type::ScaleChange:
                    dpr = e.scale;
                    app.resize(static_cast<float>(pxW) / dpr,
                               static_cast<float>(pxH) / dpr);
                    dirty = true;
                    break;

                case plat::Event::Type::Expose:
                    dirty = true;
                    break;

                case plat::Event::Type::PointerDown:
                case plat::Event::Type::PointerUp:
                case plat::Event::Type::PointerMove:
                case plat::Event::Type::PointerLeave: {
                    PointerEvent pe;
                    pe.phase = e.type == plat::Event::Type::PointerDown
                                   ? PointerPhase::Down
                               : e.type == plat::Event::Type::PointerUp
                                   ? PointerPhase::Up
                               : e.type == plat::Event::Type::PointerMove
                                   ? PointerPhase::Move
                                   : PointerPhase::Leave;
                    pe.pos = {static_cast<float>(e.x) / dpr,
                              static_cast<float>(e.y) / dpr};
                    pe.windowPos = pe.pos;
                    pe.button = toUiButton(e.button);
                    pe.modifiers = e.mods;
                    pe.clickCount = e.clickCount;
                    pe.timestamp = seconds();
                    app.pointer(pe);
                    dirty = true;
                    break;
                }

                case plat::Event::Type::Wheel: {
                    PointerEvent pe;
                    pe.phase = PointerPhase::Wheel;
                    pe.pos = {static_cast<float>(e.x) / dpr,
                              static_cast<float>(e.y) / dpr};
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
                    KeyEvent ke;
                    ke.key = toUiKey(e.key);
                    ke.pressed = e.type == plat::Event::Type::KeyDown;
                    ke.repeat = e.repeat;
                    ke.modifiers = e.mods;
                    app.key(ke);
                    dirty = true;
                    break;
                }

                case plat::Event::Type::Text: {
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

        const double now = seconds();
        if (dirty || app.needsFrame(now)) {
            render::IPaintTarget* target = nullptr;
            std::optional<render::skia::SkiaPaintTarget> softTarget;

            if (useGpu) {
                if (gpu.resize(pxW, pxH, dpr)) target = gpu.begin();
                if (!target) useGpu = false;  // context lost; drop back
            }
            if (!target) {
                if (!raster || raster->width() != pxW || raster->height() != pxH) {
                    raster = SkSurfaces::Raster(SkImageInfo::Make(
                        pxW, pxH, kBGRA_8888_SkColorType, kPremul_SkAlphaType,
                        SkColorSpace::MakeSRGB()));
                }
                if (!raster) return 3;
                softTarget.emplace(raster->getCanvas());
                target = &*softTarget;
            }

            app.paint(*target, now, dpr);

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
        }

        // Zero timeout while animating - the swap's vsync is the throttle.
        // Otherwise block, so an idle Folio uses no CPU at all.
        window->waitEvents(app.needsFrame(seconds()) ? 0 : 50);
    }

    FilePicker::setCurrent(nullptr);
    return 0;
}

}  // namespace folio::ui
