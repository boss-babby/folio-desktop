// folio-platform/linux/X11Window.cpp
//
// X11 + EGL backend.
//
// EGL rather than GLX, for two reasons: it is the same API we will use for
// the Wayland backend (only the `eglGetDisplay` argument and the native
// window type change), and it is the same API used on Android. GLX would
// have meant a third context-creation path for no gain.
//
// THREADING
// ---------
// Every Xlib call happens on the UI thread. `wake()` is the one method a
// worker may call, and it deliberately does not touch Xlib at all - it
// writes a byte to a self-pipe that `waitEvents` selects on alongside the X
// connection. That avoids `XInitThreads()` and the lock contention it brings,
// and it is why a page raster landing off-thread can wake the UI without a
// data race.

#include "folio/platform/Platform.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#if FOLIO_HAVE_XI2
#include <X11/extensions/XInput2.h>

#include "XiValuator.h"
#endif

#include <EGL/egl.h>

// -----------------------------------------------------------------------
// X11 macro quarantine.
//
// X.h is a 1987 C header and #defines several of its event-type constants
// as bare integers. Four of those names collide with identifiers in
// Platform.h - `CursorShape`, `Expose`, `FocusIn`, `FocusOut` - and the
// preprocessor wins every one of those fights, producing errors like
// "expected ')' before numeric constant" far from the real cause.
//
// The values are captured here and the macros removed, so the rest of the
// file can use both the X11 constants and our own names.
// -----------------------------------------------------------------------
namespace folio::platform::x11 {
constexpr int kExpose   = Expose;
constexpr int kFocusIn  = FocusIn;
constexpr int kFocusOut = FocusOut;
}  // namespace folio::platform::x11

#undef CursorShape
#undef Expose
#undef FocusIn
#undef FocusOut

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <string>

namespace folio::platform {
namespace {

// ---------------------------------------------------------------------------
// Atoms
// ---------------------------------------------------------------------------

struct Atoms {
    Atom wmProtocols, wmDeleteWindow, netWmState, netWmStateMaxHorz,
        netWmStateMaxVert, netWmMoveResize, netWmName, utf8String, motifWmHints,
        clipboard, targets, incr, folioSelection, netWmPid, netWmWindowType,
        netWmWindowTypeNormal;

    void init(Display* d) {
        auto a = [d](const char* n) { return XInternAtom(d, n, False); };
        wmProtocols           = a("WM_PROTOCOLS");
        wmDeleteWindow        = a("WM_DELETE_WINDOW");
        netWmState            = a("_NET_WM_STATE");
        netWmStateMaxHorz     = a("_NET_WM_STATE_MAXIMIZED_HORZ");
        netWmStateMaxVert     = a("_NET_WM_STATE_MAXIMIZED_VERT");
        netWmMoveResize       = a("_NET_WM_MOVERESIZE");
        netWmName             = a("_NET_WM_NAME");
        utf8String            = a("UTF8_STRING");
        motifWmHints          = a("_MOTIF_WM_HINTS");
        clipboard             = a("CLIPBOARD");
        targets               = a("TARGETS");
        incr                  = a("INCR");
        folioSelection        = a("FOLIO_SELECTION");
        netWmPid              = a("_NET_WM_PID");
        netWmWindowType       = a("_NET_WM_WINDOW_TYPE");
        netWmWindowTypeNormal = a("_NET_WM_WINDOW_TYPE_NORMAL");
    }
};

/// The Motif hints struct. Still, in 2026, the only portable way to ask for
/// an undecorated window: `_GTK_FRAME_EXTENTS` and friends are toolkit-
/// specific, and every mainstream WM honours this one.
struct MotifHints {
    unsigned long flags = 2;  // MWM_HINTS_DECORATIONS
    unsigned long functions = 0;
    unsigned long decorations = 0;
    long input_mode = 0;
    unsigned long status = 0;
};

// _NET_WM_MOVERESIZE direction codes, from the EWMH spec.
constexpr long kMoveResizeSizeTopLeft     = 0;
constexpr long kMoveResizeSizeTop         = 1;
constexpr long kMoveResizeSizeTopRight    = 2;
constexpr long kMoveResizeSizeRight       = 3;
constexpr long kMoveResizeSizeBottomRight = 4;
constexpr long kMoveResizeSizeBottom      = 5;
constexpr long kMoveResizeSizeBottomLeft  = 6;
constexpr long kMoveResizeSizeLeft        = 7;
constexpr long kMoveResizeMove            = 8;

long edgeToDirection(ResizeEdge e) {
    switch (e) {
        case ResizeEdge::Top:         return kMoveResizeSizeTop;
        case ResizeEdge::Bottom:      return kMoveResizeSizeBottom;
        case ResizeEdge::Left:        return kMoveResizeSizeLeft;
        case ResizeEdge::Right:       return kMoveResizeSizeRight;
        case ResizeEdge::TopLeft:     return kMoveResizeSizeTopLeft;
        case ResizeEdge::TopRight:    return kMoveResizeSizeTopRight;
        case ResizeEdge::BottomLeft:  return kMoveResizeSizeBottomLeft;
        case ResizeEdge::BottomRight: return kMoveResizeSizeBottomRight;
    }
    return kMoveResizeSizeBottomRight;
}

unsigned int cursorFont(CursorShape s) {
    switch (s) {
        case CursorShape::Arrow:      return XC_left_ptr;
        case CursorShape::Hand:       return XC_hand2;
        case CursorShape::IBeam:      return XC_xterm;
        case CursorShape::Crosshair:  return XC_crosshair;
        case CursorShape::ResizeH:    return XC_sb_h_double_arrow;
        case CursorShape::ResizeV:    return XC_sb_v_double_arrow;
        case CursorShape::ResizeNWSE: return XC_bottom_right_corner;
        case CursorShape::ResizeNESW: return XC_bottom_left_corner;
        case CursorShape::Grab:       return XC_hand1;
        case CursorShape::Grabbing:   return XC_fleur;
        case CursorShape::Wait:       return XC_watch;
    }
    return XC_left_ptr;
}

// ---------------------------------------------------------------------------
// Key translation
// ---------------------------------------------------------------------------

Key keyFromSym(KeySym sym) {
    switch (sym) {
        case XK_a: case XK_A: return Key::A;
        case XK_c: case XK_C: return Key::C;
        case XK_f: case XK_F: return Key::F;
        case XK_n: case XK_N: return Key::N;
        case XK_o: case XK_O: return Key::O;
        case XK_p: case XK_P: return Key::P;
        case XK_s: case XK_S: return Key::S;
        case XK_t: case XK_T: return Key::T;
        case XK_w: case XK_W: return Key::W;
        case XK_z: case XK_Z: return Key::Z;
        case XK_y: case XK_Y: return Key::Y;
        case XK_0: return Key::Digit0;
        case XK_1: return Key::Digit1;
        case XK_2: return Key::Digit2;
        case XK_3: return Key::Digit3;
        case XK_4: return Key::Digit4;
        case XK_5: return Key::Digit5;
        case XK_6: return Key::Digit6;
        case XK_7: return Key::Digit7;
        case XK_8: return Key::Digit8;
        case XK_9: return Key::Digit9;
        case XK_Left:      return Key::Left;
        case XK_Right:     return Key::Right;
        case XK_Up:        return Key::Up;
        case XK_Down:      return Key::Down;
        case XK_Prior:     return Key::PageUp;
        case XK_Next:      return Key::PageDown;
        case XK_Home:      return Key::Home;
        case XK_End:       return Key::End;
        case XK_Return:
        case XK_KP_Enter:  return Key::Enter;
        case XK_Escape:    return Key::Escape;
        case XK_Tab:       return Key::Tab;
        case XK_BackSpace: return Key::Backspace;
        case XK_Delete:    return Key::Delete;
        case XK_space:     return Key::Space;
        case XK_plus:
        case XK_equal:
        case XK_KP_Add:    return Key::Plus;
        case XK_minus:
        case XK_KP_Subtract: return Key::Minus;
        case XK_F11:       return Key::F11;
        default: return Key::Unknown;
    }
}

std::uint32_t modsFromState(unsigned int state) {
    std::uint32_t m = ModNone;
    if (state & ShiftMask)   m |= ModShift;
    if (state & ControlMask) m |= ModCtrl;
    if (state & Mod1Mask)    m |= ModAlt;
    if (state & Mod4Mask)    m |= ModMeta;
    return m;
}

// ---------------------------------------------------------------------------
// Shared display
//
// The window and the clipboard both need the Display, and the clipboard
// outlives any individual window (you can copy, close the document, and
// paste). One lazily-opened connection owned by the module.
// ---------------------------------------------------------------------------

struct Shared {
    Display* dpy = nullptr;
    Atoms atoms{};
    bool tried = false;

    static Shared& get() {
        static Shared s;
        if (!s.tried) {
            s.tried = true;
            s.dpy = XOpenDisplay(nullptr);
            if (s.dpy) s.atoms.init(s.dpy);
        }
        return s;
    }
};

/// Read Xft.dpi out of the X resource database. This is what every toolkit
/// uses as the authoritative scale on X11; RANDR's physical size is routinely
/// wrong and the environment variables are not set by most desktops.
float readScale(Display* dpy) {
    if (const char* env = std::getenv("FOLIO_SCALE")) {
        const float v = std::strtof(env, nullptr);
        if (v > 0.1f && v < 10.f) return v;
    }
    float scale = 1.f;
    if (char* rms = XResourceManagerString(dpy)) {
        XrmDatabase db = XrmGetStringDatabase(rms);
        if (db) {
            char* type = nullptr;
            XrmValue val{};
            if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &val) &&
                val.addr) {
                const float dpi = std::strtof(val.addr, nullptr);
                if (dpi > 40.f) scale = dpi / 96.f;
            }
            XrmDestroyDatabase(db);
        }
    }
    return scale;
}

// ---------------------------------------------------------------------------
// EGL context
// ---------------------------------------------------------------------------

class EglContext final : public IGlContext {
public:
    bool create(Display* dpy, Window win, EGLint visualIdWanted, std::string* err);
    ~EglContext() override;

    void makeCurrent() override {
        if (dpy_ != EGL_NO_DISPLAY) eglMakeCurrent(dpy_, surf_, surf_, ctx_);
    }
    void swapBuffers() override {
        if (dpy_ != EGL_NO_DISPLAY) eglSwapBuffers(dpy_, surf_);
    }
    void setSwapInterval(int interval) override {
        if (dpy_ != EGL_NO_DISPLAY) eglSwapInterval(dpy_, interval);
    }
    [[nodiscard]] ProcAddress procAddress(const char* name) const override {
        return reinterpret_cast<ProcAddress>(eglGetProcAddress(name));
    }
    [[nodiscard]] int stencilBits() const override { return stencil_; }
    [[nodiscard]] int sampleCount() const override { return samples_; }

    /// Chosen config's X visual, needed before the window exists.
    static bool pickConfig(EGLDisplay d, EGLConfig* out, EGLint* visualId);

private:
    EGLDisplay dpy_ = EGL_NO_DISPLAY;
    EGLSurface surf_ = EGL_NO_SURFACE;
    EGLContext ctx_ = EGL_NO_CONTEXT;
    int stencil_ = 8;
    int samples_ = 1;
};

bool EglContext::pickConfig(EGLDisplay d, EGLConfig* out, EGLint* visualId) {
    // No depth, no MSAA, 8 bits of stencil. Skia does its own AA; asking the
    // driver for MSAA on the main target costs bandwidth on every frame and
    // changes nothing on screen.
    const EGLint attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_STENCIL_SIZE,    8,
        EGL_DEPTH_SIZE,      0,
        EGL_SAMPLES,         0,
        EGL_NONE,
    };
    EGLint count = 0;
    if (!eglChooseConfig(d, attribs, out, 1, &count) || count < 1) return false;
    return eglGetConfigAttrib(d, *out, EGL_NATIVE_VISUAL_ID, visualId) == EGL_TRUE;
}

bool EglContext::create(Display* xdpy, Window win, EGLint, std::string* err) {
    dpy_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(xdpy));
    if (dpy_ == EGL_NO_DISPLAY) {
        if (err) *err = "eglGetDisplay failed";
        return false;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy_, &major, &minor)) {
        if (err) *err = "eglInitialize failed";
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        if (err) *err = "this driver has no desktop GL (EGL_OPENGL_API)";
        return false;
    }

    EGLConfig cfg{};
    EGLint visualId = 0;
    if (!pickConfig(dpy_, &cfg, &visualId)) {
        if (err) *err = "no EGL config with RGBA8 + stencil8";
        return false;
    }

    // Ask for 3.3 core, which is comfortably below what Ganesh needs and
    // above what it will reject. Drivers that cannot give a core profile
    // hand back a compatibility context, which Skia also accepts.
    const EGLint ctxAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_NONE,
    };
    ctx_ = eglCreateContext(dpy_, cfg, EGL_NO_CONTEXT, ctxAttribs);
    if (ctx_ == EGL_NO_CONTEXT) {
        ctx_ = eglCreateContext(dpy_, cfg, EGL_NO_CONTEXT, nullptr);
    }
    if (ctx_ == EGL_NO_CONTEXT) {
        if (err) *err = "eglCreateContext failed";
        return false;
    }

    surf_ = eglCreateWindowSurface(
        dpy_, cfg, static_cast<EGLNativeWindowType>(win), nullptr);
    if (surf_ == EGL_NO_SURFACE) {
        if (err) *err = "eglCreateWindowSurface failed";
        return false;
    }

    eglMakeCurrent(dpy_, surf_, surf_, ctx_);
    EGLint v = 0;
    if (eglGetConfigAttrib(dpy_, cfg, EGL_STENCIL_SIZE, &v)) stencil_ = v;
    if (eglGetConfigAttrib(dpy_, cfg, EGL_SAMPLES, &v)) samples_ = std::max(1, v);
    eglSwapInterval(dpy_, 1);
    return true;
}

EglContext::~EglContext() {
    if (dpy_ == EGL_NO_DISPLAY) return;
    eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surf_ != EGL_NO_SURFACE) eglDestroySurface(dpy_, surf_);
    if (ctx_ != EGL_NO_CONTEXT) eglDestroyContext(dpy_, ctx_);
    eglTerminate(dpy_);
}

// ---------------------------------------------------------------------------
// Clipboard
//
// X11 has no clipboard. It has a protocol for asking whichever client last
// claimed ownership to send you some bytes, which means:
//   * copying = claiming ownership and then answering requests forever, and
//   * pasting = sending a request and pumping events until a reply lands.
// Both halves live here. The owner half has to keep running while the app
// runs, so `pumpSelectionEvent` is called from the window's event loop.
// ---------------------------------------------------------------------------

class X11Clipboard final : public IClipboard {
public:
    bool setText(std::string_view text) override;
    [[nodiscard]] std::string text() override;

    /// Called by the window loop for SelectionRequest / SelectionClear.
    /// Returns true when the event was ours.
    bool handle(const XEvent& ev);

    void setOwnerWindow(Window w) { owner_ = w; }

private:
    std::string data_;
    Window owner_ = None;
    bool owns_ = false;
};

bool X11Clipboard::setText(std::string_view text) {
    Shared& s = Shared::get();
    if (!s.dpy || owner_ == None) return false;
    data_.assign(text);
    XSetSelectionOwner(s.dpy, s.atoms.clipboard, owner_, CurrentTime);
    if (XGetSelectionOwner(s.dpy, s.atoms.clipboard) != owner_) return false;
    owns_ = true;
    XFlush(s.dpy);
    return true;
}

bool X11Clipboard::handle(const XEvent& ev) {
    Shared& s = Shared::get();
    if (!s.dpy) return false;

    if (ev.type == SelectionClear) {
        // Someone else copied. Drop the data; we are no longer the owner.
        if (ev.xselectionclear.selection == s.atoms.clipboard) {
            owns_ = false;
            data_.clear();
            return true;
        }
        return false;
    }

    if (ev.type != SelectionRequest) return false;
    const XSelectionRequestEvent& req = ev.xselectionrequest;
    if (req.selection != s.atoms.clipboard) return false;

    XSelectionEvent resp{};
    resp.type = SelectionNotify;
    resp.display = req.display;
    resp.requestor = req.requestor;
    resp.selection = req.selection;
    resp.target = req.target;
    resp.time = req.time;
    resp.property = None;  // "refused", unless we fill it in below

    if (req.target == s.atoms.targets) {
        // Advertise what we can convert to. Listing TARGETS itself is part
        // of the convention and some clients check for it.
        const Atom offered[] = {s.atoms.targets, s.atoms.utf8String, XA_STRING};
        XChangeProperty(s.dpy, req.requestor, req.property, XA_ATOM, 32,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char*>(offered),
                        static_cast<int>(sizeof(offered) / sizeof(offered[0])));
        resp.property = req.property;
    } else if (req.target == s.atoms.utf8String || req.target == XA_STRING) {
        XChangeProperty(s.dpy, req.requestor, req.property, req.target, 8,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char*>(data_.data()),
                        static_cast<int>(data_.size()));
        resp.property = req.property;
    }

    XSendEvent(s.dpy, req.requestor, False, 0, reinterpret_cast<XEvent*>(&resp));
    XFlush(s.dpy);
    return true;
}

std::string X11Clipboard::text() {
    Shared& s = Shared::get();
    if (!s.dpy || owner_ == None) return {};
    if (owns_) return data_;  // we are the owner; no round trip needed

    XConvertSelection(s.dpy, s.atoms.clipboard, s.atoms.utf8String,
                      s.atoms.folioSelection, owner_, CurrentTime);
    XFlush(s.dpy);

    // Pump for a bounded time. A dead or wedged selection owner must not
    // hang the UI thread, so this gives up after 200 ms and returns empty.
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(200);
    while (clock::now() < deadline) {
        XEvent ev;
        while (XPending(s.dpy)) {
            XNextEvent(s.dpy, &ev);
            if (ev.type != SelectionNotify) {
                handle(ev);  // keep answering others while we wait
                continue;
            }
            if (ev.xselection.property == None) return {};

            Atom actualType = None;
            int actualFormat = 0;
            unsigned long nitems = 0, bytesAfter = 0;
            unsigned char* prop = nullptr;
            if (XGetWindowProperty(s.dpy, owner_, s.atoms.folioSelection, 0,
                                   (~0L), True, AnyPropertyType, &actualType,
                                   &actualFormat, &nitems, &bytesAfter,
                                   &prop) != Success) {
                return {};
            }
            // INCR means the data arrives in chunks via PropertyNotify.
            // Folio copies a text selection, not a book; refusing the
            // transfer is better than a half-implemented state machine.
            if (actualType == s.atoms.incr) {
                if (prop) XFree(prop);
                return {};
            }
            std::string out;
            if (prop) {
                out.assign(reinterpret_cast<char*>(prop), nitems);
                XFree(prop);
            }
            return out;
        }
        usleep(2000);
    }
    return {};
}

X11Clipboard& theClipboard() {
    static X11Clipboard c;
    return c;
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

class X11Window final : public IWindow {
public:
    bool create(const WindowDesc& desc, std::string* error);
    ~X11Window() override;

    bool poll(Event& out) override;
    void waitEvents(int timeoutMs) override;
    void wake() override;

    [[nodiscard]] int widthPx() const override { return w_; }
    [[nodiscard]] int heightPx() const override { return h_; }
    [[nodiscard]] float scale() const override { return scale_; }

    void setTitle(std::string_view title) override;
    void setCursor(CursorShape shape) override;
    void minimise() override;
    void toggleMaximise() override;
    void requestClose() override { shouldClose_ = true; }
    [[nodiscard]] bool maximised() const override { return maximised_; }
    void beginDragMove() override { startMoveResize(kMoveResizeMove); }
    void beginDragResize(ResizeEdge e) override {
        startMoveResize(edgeToDirection(e));
    }
    [[nodiscard]] IGlContext& gl() override { return egl_; }
    void presentSoftware(const void* bgra, int w, int h, int stride) override;
    [[nodiscard]] bool shouldClose() const override { return shouldClose_; }

private:
    void drainX();
    void translate(const XEvent& ev);
    void startMoveResize(long direction);

    Display* dpy_ = nullptr;
    Window win_ = None;
    XIM im_ = nullptr;
    XIC ic_ = nullptr;
    Cursor cursor_ = None;
    GC softGc_ = nullptr;
    CursorShape cursorShape_ = CursorShape::Arrow;
    EglContext egl_;

    int w_ = 0, h_ = 0;
    float scale_ = 1.f;
    bool maximised_ = false;
    bool shouldClose_ = false;
    int wakePipe_[2] = {-1, -1};

    std::deque<Event> queue_;
    // Double-click detection, which X11 does not do for you.
    Time lastClickTime_ = 0;
    int lastClickX_ = 0, lastClickY_ = 0;

#if FOLIO_HAVE_XI2
    // --- XInput2 ----------------------------------------------------------
    // What a physical device can tell us, resolved once per device id and
    // cached. Valuator *numbers* are per-device and have no fixed meaning,
    // so the axis a stylus reports pressure on has to be looked up by its
    // label atom before any event from it can be read.
    struct XiAxis {
        int number = -1;
        double min = 0.0, max = 0.0;
        [[nodiscard]] bool valid() const noexcept { return number >= 0 && max > min; }
        [[nodiscard]] double norm(double v) const noexcept {
            return (v - min) / (max - min);
        }
    };
    struct XiDevice {
        PointerSource source = PointerSource::Mouse;
        XiAxis pressure, tiltX, tiltY;
    };

    void initXI2();
    bool translateXI(XGenericEventCookie& cookie);
    const XiDevice& deviceFor(int sourceId);

    int xiOpcode_ = -1;
    bool xiActive_ = false;
    std::unordered_map<int, XiDevice> xiDevices_;
#endif
};

bool X11Window::create(const WindowDesc& desc, std::string* error) {
    Shared& s = Shared::get();
    if (!s.dpy) {
        if (error) *error = "cannot open an X display (is DISPLAY set?)";
        return false;
    }
    dpy_ = s.dpy;
    scale_ = readScale(dpy_);
    w_ = static_cast<int>(static_cast<float>(desc.width) * scale_);
    h_ = static_cast<int>(static_cast<float>(desc.height) * scale_);

    // The visual has to match the EGL config, so EGL is initialised far
    // enough to choose one before the window exists.
    EGLDisplay edpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(dpy_));
    if (edpy == EGL_NO_DISPLAY || !eglInitialize(edpy, nullptr, nullptr)) {
        if (error) *error = "no EGL display";
        return false;
    }
    eglBindAPI(EGL_OPENGL_API);
    EGLConfig cfg{};
    EGLint visualId = 0;
    if (!EglContext::pickConfig(edpy, &cfg, &visualId)) {
        if (error) *error = "no suitable EGL config";
        return false;
    }

    const int screen = DefaultScreen(dpy_);
    XVisualInfo tmpl{};
    tmpl.visualid = static_cast<VisualID>(visualId);
    int nVis = 0;
    XVisualInfo* vis = XGetVisualInfo(dpy_, VisualIDMask, &tmpl, &nVis);
    Visual* visual = vis && nVis > 0 ? vis[0].visual : DefaultVisual(dpy_, screen);
    const int depth = vis && nVis > 0 ? vis[0].depth : DefaultDepth(dpy_, screen);

    XSetWindowAttributes swa{};
    swa.colormap = XCreateColormap(dpy_, RootWindow(dpy_, screen), visual,
                                   AllocNone);
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask |
                     KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | LeaveWindowMask | FocusChangeMask |
                     PropertyChangeMask;

    win_ = XCreateWindow(dpy_, RootWindow(dpy_, screen), 0, 0,
                         static_cast<unsigned>(w_), static_cast<unsigned>(h_), 0,
                         depth, InputOutput, visual,
                         CWColormap | CWEventMask | CWBackPixel | CWBorderPixel,
                         &swa);
    if (vis) XFree(vis);
    if (!win_) {
        if (error) *error = "XCreateWindow failed";
        return false;
    }

#if FOLIO_HAVE_XI2
    // Must run before the window is mapped: it replaces the core pointer
    // event mask chosen above, and doing that after the first events have
    // been delivered would drop them.
    initXI2();
#endif

    if (!desc.systemDecorations) {
        MotifHints hints{};
        XChangeProperty(dpy_, win_, s.atoms.motifWmHints, s.atoms.motifWmHints,
                        32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&hints), 5);
    }

    Atom del = s.atoms.wmDeleteWindow;
    XSetWMProtocols(dpy_, win_, &del, 1);

    XSizeHints* sh = XAllocSizeHints();
    sh->flags = PMinSize;
    sh->min_width = static_cast<int>(static_cast<float>(desc.minWidth) * scale_);
    sh->min_height = static_cast<int>(static_cast<float>(desc.minHeight) * scale_);
    XSetWMNormalHints(dpy_, win_, sh);
    XFree(sh);

    const long pid = getpid();
    XChangeProperty(dpy_, win_, s.atoms.netWmPid, XA_CARDINAL, 32,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&pid), 1);
    XChangeProperty(dpy_, win_, s.atoms.netWmWindowType, XA_ATOM, 32,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(
                        &s.atoms.netWmWindowTypeNormal), 1);

    setTitle(desc.title);

    // Input method, for composed text. Without this, dead keys and any IME
    // produce nothing and CJK input is impossible.
    XSetLocaleModifiers("");
    im_ = XOpenIM(dpy_, nullptr, nullptr, nullptr);
    if (im_) {
        ic_ = XCreateIC(im_, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                        XNClientWindow, win_, XNFocusWindow, win_, nullptr);
        if (ic_) XSetICFocus(ic_);
    }

    XMapWindow(dpy_, win_);
    XFlush(dpy_);

    std::string eglErr;
    if (!egl_.create(dpy_, win_, visualId, &eglErr)) {
        if (error) *error = eglErr;
        return false;
    }

    if (pipe(wakePipe_) != 0) { wakePipe_[0] = wakePipe_[1] = -1; }
    theClipboard().setOwnerWindow(win_);
    setCursor(CursorShape::Arrow);
    return true;
}

X11Window::~X11Window() {
    if (ic_) XDestroyIC(ic_);
    if (im_) XCloseIM(im_);
    if (softGc_ && dpy_) XFreeGC(dpy_, softGc_);
    if (cursor_ != None && dpy_) XFreeCursor(dpy_, cursor_);
    if (win_ != None && dpy_) XDestroyWindow(dpy_, win_);
    for (int fd : wakePipe_) if (fd >= 0) ::close(fd);
}

void X11Window::setTitle(std::string_view title) {
    if (!dpy_ || win_ == None) return;
    Shared& s = Shared::get();
    const std::string t(title);
    XChangeProperty(dpy_, win_, s.atoms.netWmName, s.atoms.utf8String, 8,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(t.c_str()),
                    static_cast<int>(t.size()));
    XStoreName(dpy_, win_, t.c_str());
}

void X11Window::setCursor(CursorShape shape) {
    if (!dpy_ || win_ == None) return;
    if (cursor_ != None && shape == cursorShape_) return;
    Cursor c = XCreateFontCursor(dpy_, cursorFont(shape));
    XDefineCursor(dpy_, win_, c);
    if (cursor_ != None) XFreeCursor(dpy_, cursor_);
    cursor_ = c;
    cursorShape_ = shape;
}

void X11Window::minimise() {
    if (dpy_ && win_ != None) XIconifyWindow(dpy_, win_, DefaultScreen(dpy_));
}

void X11Window::toggleMaximise() {
    if (!dpy_ || win_ == None) return;
    Shared& s = Shared::get();
    XClientMessageEvent ev{};
    ev.type = ClientMessage;
    ev.window = win_;
    ev.message_type = s.atoms.netWmState;
    ev.format = 32;
    ev.data.l[0] = maximised_ ? 0 : 1;  // _NET_WM_STATE_REMOVE / _ADD
    ev.data.l[1] = static_cast<long>(s.atoms.netWmStateMaxHorz);
    ev.data.l[2] = static_cast<long>(s.atoms.netWmStateMaxVert);
    ev.data.l[3] = 1;  // normal application
    XSendEvent(dpy_, RootWindow(dpy_, DefaultScreen(dpy_)), False,
               SubstructureNotifyMask | SubstructureRedirectMask,
               reinterpret_cast<XEvent*>(&ev));
    XFlush(dpy_);
    maximised_ = !maximised_;
}

void X11Window::startMoveResize(long direction) {
    if (!dpy_ || win_ == None) return;
    Shared& s = Shared::get();

    // The WM needs the pointer's root-relative position and the button that
    // is down, and it needs us to let go of the pointer grab first.
    Window root = None, child = None;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    XQueryPointer(dpy_, win_, &root, &child, &rx, &ry, &wx, &wy, &mask);
    XUngrabPointer(dpy_, CurrentTime);

    XClientMessageEvent ev{};
    ev.type = ClientMessage;
    ev.window = win_;
    ev.message_type = s.atoms.netWmMoveResize;
    ev.format = 32;
    ev.data.l[0] = rx;
    ev.data.l[1] = ry;
    ev.data.l[2] = direction;
    ev.data.l[3] = 1;  // left button
    ev.data.l[4] = 1;  // normal application
    XSendEvent(dpy_, RootWindow(dpy_, DefaultScreen(dpy_)), False,
               SubstructureNotifyMask | SubstructureRedirectMask,
               reinterpret_cast<XEvent*>(&ev));
    XFlush(dpy_);
}

void X11Window::presentSoftware(const void* bgra, int w, int h, int stride) {
    if (!dpy_ || win_ == None || !bgra || w <= 0 || h <= 0) return;
    if (!softGc_) softGc_ = XCreateGC(dpy_, win_, 0, nullptr);
    if (!softGc_) return;

    // XImage does not take ownership and does not copy: it reads straight
    // out of Skia's surface. The const_cast is safe because XPutImage only
    // reads, but XImage's `data` member is not const-qualified.
    XImage img{};
    img.width = w;
    img.height = h;
    img.format = ZPixmap;
    img.data = const_cast<char*>(static_cast<const char*>(bgra));
    img.byte_order = LSBFirst;
    img.bitmap_unit = 32;
    img.bitmap_bit_order = LSBFirst;
    img.bitmap_pad = 32;
    img.depth = 24;          // the X server ignores the alpha byte
    img.bytes_per_line = stride;
    img.bits_per_pixel = 32;
    img.red_mask = 0x00ff0000;
    img.green_mask = 0x0000ff00;
    img.blue_mask = 0x000000ff;
    if (!XInitImage(&img)) return;

    XPutImage(dpy_, win_, softGc_, &img, 0, 0, 0, 0,
              static_cast<unsigned>(w), static_cast<unsigned>(h));
    XFlush(dpy_);
}

void X11Window::wake() {
    // Thread-safe on purpose: one byte down a pipe, no Xlib involved.
    if (wakePipe_[1] >= 0) {
        const char b = 1;
        ssize_t n = ::write(wakePipe_[1], &b, 1);
        (void)n;
    }
}

void X11Window::translate(const XEvent& ev) {
    Shared& s = Shared::get();
    Event e;

    switch (ev.type) {
        case ClientMessage:
            if (ev.xclient.message_type == s.atoms.wmProtocols &&
                static_cast<Atom>(ev.xclient.data.l[0]) == s.atoms.wmDeleteWindow) {
                e.type = Event::Type::Close;
                shouldClose_ = true;
                queue_.push_back(std::move(e));
            }
            return;

        case ConfigureNotify:
            if (ev.xconfigure.width != w_ || ev.xconfigure.height != h_) {
                w_ = ev.xconfigure.width;
                h_ = ev.xconfigure.height;
                e.type = Event::Type::Resize;
                e.w = w_;
                e.h = h_;
                e.scale = scale_;
                queue_.push_back(std::move(e));
            }
            return;

        case x11::kExpose:
            if (ev.xexpose.count == 0) {
                e.type = Event::Type::Expose;
                queue_.push_back(std::move(e));
            }
            return;

        case x11::kFocusIn:
            if (ic_) XSetICFocus(ic_);
            e.type = Event::Type::FocusIn;
            queue_.push_back(std::move(e));
            return;

        case x11::kFocusOut:
            if (ic_) XUnsetICFocus(ic_);
            e.type = Event::Type::FocusOut;
            queue_.push_back(std::move(e));
            return;

        case LeaveNotify:
            e.type = Event::Type::PointerLeave;
            queue_.push_back(std::move(e));
            return;

        case MotionNotify:
            e.type = Event::Type::PointerMove;
            e.x = ev.xmotion.x;
            e.y = ev.xmotion.y;
            e.mods = modsFromState(ev.xmotion.state);
            queue_.push_back(std::move(e));
            return;

        case ButtonPress:
        case ButtonRelease: {
            const XButtonEvent& b = ev.xbutton;
            // X11 reports wheel notches as presses of buttons 4-7.
            if (b.button >= 4 && b.button <= 7) {
                if (ev.type == ButtonRelease) return;  // ignore the pair
                e.type = Event::Type::Wheel;
                e.x = b.x;
                e.y = b.y;
                e.wheelY = b.button == 4 ? 1.0 : (b.button == 5 ? -1.0 : 0.0);
                e.wheelX = b.button == 6 ? -1.0 : (b.button == 7 ? 1.0 : 0.0);
                e.mods = modsFromState(b.state);
                queue_.push_back(std::move(e));
                return;
            }
            e.type = ev.type == ButtonPress ? Event::Type::PointerDown
                                            : Event::Type::PointerUp;
            e.x = b.x;
            e.y = b.y;
            e.mods = modsFromState(b.state);
            e.button = b.button == Button1   ? Button::Left
                       : b.button == Button2 ? Button::Middle
                       : b.button == Button3 ? Button::Right
                                             : Button::Other;
            if (ev.type == ButtonPress) {
                const bool near = std::abs(b.x - lastClickX_) < 4 &&
                                  std::abs(b.y - lastClickY_) < 4;
                e.clickCount =
                    (near && b.time - lastClickTime_ < 400) ? 2 : 1;
                lastClickTime_ = b.time;
                lastClickX_ = b.x;
                lastClickY_ = b.y;
            }
            queue_.push_back(std::move(e));
            return;
        }

        case KeyPress: {
            XKeyEvent key = ev.xkey;
            KeySym sym = NoSymbol;
            char buf[64] = {};
            int len = 0;
            Status status = 0;
            if (ic_) {
                len = Xutf8LookupString(ic_, &key, buf, sizeof(buf) - 1, &sym,
                                        &status);
                if (status == XBufferOverflow) len = 0;
            } else {
                len = XLookupString(&key, buf, sizeof(buf) - 1, &sym, nullptr);
            }

            Event k;
            k.type = Event::Type::KeyDown;
            k.key = keyFromSym(sym);
            k.mods = modsFromState(key.state);
            queue_.push_back(std::move(k));

            // Composed text is a separate event. Control chords never
            // produce text, and neither do the C0 characters that
            // XLookupString happily returns for Return/Tab/Backspace.
            if (len > 0 && !(key.state & ControlMask) &&
                static_cast<unsigned char>(buf[0]) >= 0x20 &&
                static_cast<unsigned char>(buf[0]) != 0x7f) {
                Event t;
                t.type = Event::Type::Text;
                t.text.assign(buf, static_cast<std::size_t>(len));
                t.mods = k.mods;
                queue_.push_back(std::move(t));
            }
            return;
        }

        case KeyRelease: {
            KeySym sym = XkbKeycodeToKeysym(dpy_, ev.xkey.keycode, 0, 0);
            e.type = Event::Type::KeyUp;
            e.key = keyFromSym(sym);
            e.mods = modsFromState(ev.xkey.state);
            queue_.push_back(std::move(e));
            return;
        }

        case PropertyNotify:
            // Track maximise changes the WM made without being asked
            // (double-clicking the frame, a keyboard shortcut, tiling).
            if (ev.xproperty.atom == s.atoms.netWmState) {
                Atom type = None;
                int fmt = 0;
                unsigned long n = 0, after = 0;
                unsigned char* prop = nullptr;
                if (XGetWindowProperty(dpy_, win_, s.atoms.netWmState, 0, 32,
                                       False, XA_ATOM, &type, &fmt, &n, &after,
                                       &prop) == Success &&
                    prop) {
                    const Atom* atoms = reinterpret_cast<Atom*>(prop);
                    bool horz = false, vert = false;
                    for (unsigned long i = 0; i < n; ++i) {
                        if (atoms[i] == s.atoms.netWmStateMaxHorz) horz = true;
                        if (atoms[i] == s.atoms.netWmStateMaxVert) vert = true;
                    }
                    maximised_ = horz && vert;
                    XFree(prop);
                }
            }
            return;

        default:
            return;
    }
}

#if FOLIO_HAVE_XI2
// ---------------------------------------------------------------------------
// XInput2: multi-touch and pen pressure
//
// Core X11 gives one pointer, one position and no pressure. Everything a
// tablet or a touchscreen knows beyond that arrives only through XI2, as
// GenericEvents carrying a per-device valuator set.
//
// Two things make this more than a translation layer:
//
//  * Valuator numbers are per-device. "Axis 2" is pressure on one stylus
//    and tilt on another, so each device's axes are resolved by their
//    label atoms once and cached.
//  * Selecting touch events changes core delivery. Once a client selects
//    XI_TouchBegin on a window, the server stops sending it the emulated
//    core pointer events for that touch. That is exactly what we want -
//    but it means the core ButtonPress/MotionNotify masks must come off
//    at the same time, or a mouse would be delivered twice.
// ---------------------------------------------------------------------------

void X11Window::initXI2() {
    int event = 0, error = 0;
    if (!XQueryExtension(dpy_, "XInputExtension", &xiOpcode_, &event, &error))
        return;

    // 2.2 is the first version with multi-touch. Asking for it on an older
    // server returns the server's version instead of failing, so the reply
    // has to be checked rather than trusted.
    int major = 2, minor = 2;
    if (XIQueryVersion(dpy_, &major, &minor) != Success) return;
    if (major < 2 || (major == 2 && minor < 2)) return;

    unsigned char mask[XIMaskLen(XI_LASTEVENT)] = {0};
    XISetMask(mask, XI_ButtonPress);
    XISetMask(mask, XI_ButtonRelease);
    XISetMask(mask, XI_Motion);
    XISetMask(mask, XI_TouchBegin);
    XISetMask(mask, XI_TouchUpdate);
    XISetMask(mask, XI_TouchEnd);

    XIEventMask em;
    em.deviceid = XIAllMasterDevices;
    em.mask_len = sizeof(mask);
    em.mask = mask;
    if (XISelectEvents(dpy_, win_, &em, 1) != Success) return;

    xiActive_ = true;
    if (std::getenv("FOLIO_DEBUG_INPUT"))
        std::fprintf(stderr,
                     "[input] XInput2 %d.%d active: pointer, touch and pen "
                     "events arrive as XI2; core pointer masks released\n",
                     major, minor);

    // Hand pointer input over to XI2 wholesale. Leaving the core masks on
    // would deliver every mouse click twice: once as ButtonPress and once
    // as XI_ButtonPress.
    XSelectInput(dpy_, win_,
                 ExposureMask | StructureNotifyMask | KeyPressMask |
                     KeyReleaseMask | LeaveWindowMask | FocusChangeMask |
                     PropertyChangeMask);
}

const X11Window::XiDevice& X11Window::deviceFor(int sourceId) {
    if (auto it = xiDevices_.find(sourceId); it != xiDevices_.end())
        return it->second;

    XiDevice dev;
    int count = 0;
    if (XIDeviceInfo* info = XIQueryDevice(dpy_, sourceId, &count)) {
        for (int i = 0; i < count; ++i) {
            for (int c = 0; c < info[i].num_classes; ++c) {
                const XIAnyClassInfo* any = info[i].classes[c];
                if (any->type == XITouchClass) {
                    const auto* t =
                        reinterpret_cast<const XITouchClassInfo*>(any);
                    // Indirect touch is a trackpad: the OS already turns it
                    // into pointer motion and scroll, and treating it as a
                    // touchscreen would fight the desktop.
                    if (t->mode == XIDirectTouch)
                        dev.source = PointerSource::Touch;
                    continue;
                }
                if (any->type != XIValuatorClass) continue;
                const auto* v =
                    reinterpret_cast<const XIValuatorClassInfo*>(any);
                if (v->label == None) continue;
                char* label = XGetAtomName(dpy_, v->label);
                if (!label) continue;
                XiAxis axis{v->number, v->min, v->max};
                if (std::strcmp(label, "Abs Pressure") == 0) {
                    dev.pressure = axis;
                    // Only a stylus reports pressure. A touchscreen that
                    // also does was already classified above and keeps
                    // Touch, which is the behaviour we want.
                    if (dev.source == PointerSource::Mouse)
                        dev.source = PointerSource::Pen;
                } else if (std::strcmp(label, "Abs Tilt X") == 0) {
                    dev.tiltX = axis;
                } else if (std::strcmp(label, "Abs Tilt Y") == 0) {
                    dev.tiltY = axis;
                }
                XFree(label);
            }
        }
        XIFreeDeviceInfo(info);
    }
    if (std::getenv("FOLIO_DEBUG_INPUT")) {
        static const char* kSrc[] = {"mouse", "touch", "pen"};
        std::fprintf(stderr,
                     "[input] device %d: %s (pressure axis %d, tilt %d/%d)\n",
                     sourceId, kSrc[static_cast<int>(dev.source)],
                     dev.pressure.number, dev.tiltX.number, dev.tiltY.number);
    }
    return xiDevices_.emplace(sourceId, dev).first->second;
}

bool X11Window::translateXI(XGenericEventCookie& cookie) {
    const auto* de = static_cast<const XIDeviceEvent*>(cookie.data);
    const XiDevice& dev = deviceFor(de->sourceid);

    Event e;
    e.x = de->event_x;
    e.y = de->event_y;
    e.mods = modsFromState(de->mods.effective);
    e.source = dev.source;

    const auto readAxis = [&](const XiAxis& axis, float& out, bool degrees) {
        if (!axis.valid()) return;
        double raw = 0.0;
        if (!folio::platform::x11detail::valuator(
                de->valuators.mask, de->valuators.mask_len,
                de->valuators.values, axis.number, &raw))
            return;
        out = degrees ? static_cast<float>(raw)
                      : static_cast<float>(std::clamp(axis.norm(raw), 0.0, 1.0));
    };

    switch (cookie.evtype) {
        case XI_TouchBegin:
        case XI_TouchUpdate:
        case XI_TouchEnd:
            e.source = PointerSource::Touch;
            // `detail` is the touch sequence id: stable for the life of one
            // contact, which is exactly the identity a gesture tracker needs
            // to tell two fingers apart.
            e.pointerId = static_cast<std::uint32_t>(de->detail);
            e.button = Button::Left;
            e.pressure = cookie.evtype == XI_TouchEnd ? 0.f : 1.f;
            e.type = cookie.evtype == XI_TouchBegin  ? Event::Type::PointerDown
                     : cookie.evtype == XI_TouchEnd  ? Event::Type::PointerUp
                                                     : Event::Type::PointerMove;
            if (cookie.evtype == XI_TouchBegin) {
                const bool near = std::abs(e.x - lastClickX_) < 4 &&
                                  std::abs(e.y - lastClickY_) < 4;
                e.clickCount = (near && de->time - lastClickTime_ < 400) ? 2 : 1;
                lastClickTime_ = de->time;
                lastClickX_ = static_cast<int>(e.x);
                lastClickY_ = static_cast<int>(e.y);
            }
            queue_.push_back(std::move(e));
            return true;

        case XI_Motion:
            readAxis(dev.pressure, e.pressure, false);
            readAxis(dev.tiltX, e.tiltX, true);
            readAxis(dev.tiltY, e.tiltY, true);
            e.type = Event::Type::PointerMove;
            queue_.push_back(std::move(e));
            return true;

        case XI_ButtonPress:
        case XI_ButtonRelease: {
            // Wheel notches are still buttons 4-7 here, exactly as in core.
            if (de->detail >= 4 && de->detail <= 7) {
                if (cookie.evtype == XI_ButtonRelease) return true;
                e.type = Event::Type::Wheel;
                e.wheelY = de->detail == 4 ? 1.0 : (de->detail == 5 ? -1.0 : 0.0);
                e.wheelX = de->detail == 6 ? -1.0 : (de->detail == 7 ? 1.0 : 0.0);
                queue_.push_back(std::move(e));
                return true;
            }
            readAxis(dev.pressure, e.pressure, false);
            readAxis(dev.tiltX, e.tiltX, true);
            readAxis(dev.tiltY, e.tiltY, true);
            // A pen that reports no pressure on contact is a pen whose
            // driver does not expose the axis; treat the barrel switch as
            // full pressure so stroke width stays usable.
            if (dev.source == PointerSource::Pen && e.pressure <= 0.f &&
                cookie.evtype == XI_ButtonPress)
                e.pressure = 1.f;
            e.type = cookie.evtype == XI_ButtonPress ? Event::Type::PointerDown
                                                     : Event::Type::PointerUp;
            e.button = de->detail == 1   ? Button::Left
                       : de->detail == 2 ? Button::Middle
                       : de->detail == 3 ? Button::Right
                                         : Button::Other;
            if (cookie.evtype == XI_ButtonPress) {
                const bool near = std::abs(e.x - lastClickX_) < 4 &&
                                  std::abs(e.y - lastClickY_) < 4;
                e.clickCount = (near && de->time - lastClickTime_ < 400) ? 2 : 1;
                lastClickTime_ = de->time;
                lastClickX_ = static_cast<int>(e.x);
                lastClickY_ = static_cast<int>(e.y);
            }
            queue_.push_back(std::move(e));
            return true;
        }

        default:
            return false;
    }
}
#endif  // FOLIO_HAVE_XI2

void X11Window::drainX() {
    if (!dpy_) return;
    XEvent ev;
    while (XPending(dpy_)) {
        XNextEvent(dpy_, &ev);
        // Let the input method swallow what belongs to it (preedit, IME
        // candidate windows) before anything else looks at it.
        if (XFilterEvent(&ev, win_)) continue;
        if (theClipboard().handle(ev)) continue;
#if FOLIO_HAVE_XI2
        // XI2 arrives as a cookie whose payload the server owns; it is only
        // valid between XGetEventData and XFreeEventData, so it has to be
        // consumed here rather than handed on.
        if (xiActive_ && ev.type == GenericEvent &&
            ev.xcookie.extension == xiOpcode_) {
            if (XGetEventData(dpy_, &ev.xcookie)) {
                translateXI(ev.xcookie);
                XFreeEventData(dpy_, &ev.xcookie);
            }
            continue;
        }
#endif
        translate(ev);
    }
}

bool X11Window::poll(Event& out) {
    drainX();
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void X11Window::waitEvents(int timeoutMs) {
    if (!dpy_) return;
    drainX();
    if (!queue_.empty()) return;

    XFlush(dpy_);
    struct pollfd fds[2];
    fds[0].fd = ConnectionNumber(dpy_);
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = wakePipe_[0];
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    const int n = wakePipe_[0] >= 0 ? 2 : 1;

    ::poll(fds, static_cast<nfds_t>(n), timeoutMs);

    if (n == 2 && (fds[1].revents & POLLIN)) {
        char buf[64];
        while (::read(wakePipe_[0], buf, sizeof(buf)) > 0) {}
    }
    drainX();
}

// ---------------------------------------------------------------------------
// Native file dialog
//
// There is no X11 file dialog. The desktop-neutral answer is the
// xdg-desktop-portal D-Bus interface, but talking D-Bus from scratch is a
// large dependency for one feature, so this shells out to the portal's own
// CLI front ends in preference order. When none is present `available()`
// returns false and Folio falls back to its in-app OpenDialog, which is
// exactly why that component was not thrown away.
// ---------------------------------------------------------------------------

class LinuxFileDialog final : public IFileDialog {
public:
    [[nodiscard]] bool available() const override { return !tool().empty(); }
    [[nodiscard]] std::optional<std::string> openFile(
        const FileDialogRequest& req) override {
        return run(req, false);
    }
    [[nodiscard]] std::optional<std::string> saveFile(
        const FileDialogRequest& req) override {
        return run(req, true);
    }

private:
    static const std::string& tool();
    static std::optional<std::string> run(const FileDialogRequest& req, bool save);
    static std::string shellQuote(const std::string& s);
};

std::string LinuxFileDialog::shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

const std::string& LinuxFileDialog::tool() {
    static const std::string found = [] {
        for (const char* t : {"zenity", "kdialog", "qarma", "matedialog"}) {
            const std::string probe =
                "command -v " + std::string(t) + " >/dev/null 2>&1";
            if (std::system(probe.c_str()) == 0) return std::string(t);
        }
        return std::string();
    }();
    return found;
}

std::optional<std::string> LinuxFileDialog::run(const FileDialogRequest& req,
                                                bool save) {
    const std::string& t = tool();
    if (t.empty()) return std::nullopt;

    std::string cmd;
    if (t == "kdialog") {
        cmd = "kdialog ";
        cmd += save ? "--getsavefilename " : "--getopenfilename ";
        cmd += shellQuote(req.startDirectory.empty() ? "." : req.startDirectory);
        std::string pat;
        for (const auto& f : req.filters)
            for (const auto& p : f.patterns) pat += (pat.empty() ? "" : " ") + p;
        if (!pat.empty()) cmd += " " + shellQuote(pat);
        if (!req.title.empty()) cmd += " --title " + shellQuote(req.title);
    } else {
        cmd = t + " --file-selection";
        if (save) cmd += " --save --confirm-overwrite";
        if (!req.title.empty()) cmd += " --title=" + shellQuote(req.title);
        if (!req.startDirectory.empty() || !req.suggestedName.empty()) {
            std::string fn = req.startDirectory;
            if (!fn.empty() && fn.back() != '/') fn += '/';
            fn += req.suggestedName;
            cmd += " --filename=" + shellQuote(fn);
        }
        for (const auto& f : req.filters) {
            std::string spec = f.label + " |";
            for (const auto& p : f.patterns) spec += " " + p;
            cmd += " --file-filter=" + shellQuote(spec);
        }
    }
    cmd += " 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return std::nullopt;
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    const int rc = pclose(pipe);
    if (rc != 0) return std::nullopt;  // cancelled
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    if (out.empty()) return std::nullopt;
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

std::unique_ptr<IWindow> createWindow(const WindowDesc& desc, std::string* error) {
    auto w = std::make_unique<X11Window>();
    if (!w->create(desc, error)) return nullptr;
    return w;
}

IClipboard& clipboard() { return theClipboard(); }

IFileDialog& fileDialog() {
    static LinuxFileDialog d;
    return d;
}

const char* backendName() noexcept { return "x11"; }

}  // namespace folio::platform
