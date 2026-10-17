// Folio - Win32 backend: window, WGL context, clipboard, native file dialogs.
//
// Design notes that are not obvious from the code:
//
//  * The window is borderless but still a *real* WS_OVERLAPPEDWINDOW. Folio
//    draws its own title bar, so the frame has to go - but dropping the style
//    bits would also drop snap layouts, Aero Snap, the taskbar thumbnail
//    menu and the resize cursors. Instead the style stays and WM_NCCALCSIZE
//    removes the frame's *area*, which is how Chrome, VS Code and Terminal
//    do it. The price is the two carve-outs below (maximised insets and the
//    hit-test border), and they are worth paying.
//
//  * Events are queued rather than dispatched. Win32 is callback-driven and
//    Folio's shell wants to pull; WndProc therefore only appends to a deque
//    that poll() drains. It also means a resize that arrives during the
//    modal loop Windows runs while dragging still reaches the app.
//
//  * Per-monitor DPI v2 is requested dynamically. Linking SetProcess-
//    DpiAwarenessContext would raise the minimum SDK for everyone building
//    this, and a GetProcAddress costs one call at startup.

// Guarded because the build system also sets these on the command line
// (NOMINMAX and WIN32_LEAN_AND_MEAN project-wide, UNICODE for this
// target). Redefining them unconditionally is warning C4005, which with
// warnings-as-errors would fail the build. They are kept here so the file
// still compiles if someone builds it outside our CMake.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// Windows 8+ unified pointer messages are required for real touchscreen and
// pen input; relying on synthesized WM_LBUTTON loses hold, pinch and IDs.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <GL/gl.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <unordered_set>

#include "folio/platform/Platform.h"

namespace folio::platform {
namespace {

// --- WGL extension plumbing --------------------------------------------------
//
// Declared here rather than pulled from wglext.h: that header is not part of
// the Windows SDK, and vendoring three typedefs is less friction than a
// dependency for three typedefs.

using PFNWGLCREATECONTEXTATTRIBSARB = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using PFNWGLSWAPINTERVALEXT = BOOL(WINAPI*)(int);

constexpr int kWglContextMajorVersionArb = 0x2091;
constexpr int kWglContextMinorVersionArb = 0x2092;
constexpr int kWglContextProfileMaskArb = 0x9126;
constexpr int kWglContextCoreProfileBit = 0x00000001;

std::wstring toWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), n);
    return out;
}

std::string toUtf8(const wchar_t* w, int len = -1) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr,
                                      nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), n, nullptr, nullptr);
    if (len == -1 && !out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

Key mapKey(WPARAM vk) {
    switch (vk) {
        case 'A': return Key::A;
        case 'C': return Key::C;
        case 'F': return Key::F;
        case 'N': return Key::N;
        case 'O': return Key::O;
        case 'P': return Key::P;
        case 'S': return Key::S;
        case 'T': return Key::T;
        case 'W': return Key::W;
        case 'Z': return Key::Z;
        case 'Y': return Key::Y;
        case '0': case VK_NUMPAD0: return Key::Digit0;
        case '1': case VK_NUMPAD1: return Key::Digit1;
        case '2': case VK_NUMPAD2: return Key::Digit2;
        case '3': case VK_NUMPAD3: return Key::Digit3;
        case '4': case VK_NUMPAD4: return Key::Digit4;
        case '5': case VK_NUMPAD5: return Key::Digit5;
        case '6': case VK_NUMPAD6: return Key::Digit6;
        case '7': case VK_NUMPAD7: return Key::Digit7;
        case '8': case VK_NUMPAD8: return Key::Digit8;
        case '9': case VK_NUMPAD9: return Key::Digit9;
        case VK_LEFT: return Key::Left;
        case VK_RIGHT: return Key::Right;
        case VK_UP: return Key::Up;
        case VK_DOWN: return Key::Down;
        case VK_PRIOR: return Key::PageUp;
        case VK_NEXT: return Key::PageDown;
        case VK_HOME: return Key::Home;
        case VK_END: return Key::End;
        case VK_RETURN: return Key::Enter;
        case VK_ESCAPE: return Key::Escape;
        case VK_TAB: return Key::Tab;
        case VK_BACK: return Key::Backspace;
        case VK_DELETE: return Key::Delete;
        case VK_SPACE: return Key::Space;
        case VK_OEM_PLUS: case VK_ADD: return Key::Plus;
        case VK_OEM_MINUS: case VK_SUBTRACT: return Key::Minus;
        case VK_F11: return Key::F11;
        default: return Key::Unknown;
    }
}

std::uint32_t currentMods() {
    std::uint32_t m = ModNone;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= ModShift;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= ModCtrl;
    if (GetKeyState(VK_MENU) & 0x8000) m |= ModAlt;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
        m |= ModMeta;
    return m;
}

LPCWSTR cursorIdFor(CursorShape s) {
    switch (s) {
        case CursorShape::Hand:       return IDC_HAND;
        case CursorShape::IBeam:      return IDC_IBEAM;
        case CursorShape::Crosshair:  return IDC_CROSS;
        case CursorShape::ResizeH:    return IDC_SIZEWE;
        case CursorShape::ResizeV:    return IDC_SIZENS;
        case CursorShape::ResizeNWSE: return IDC_SIZENWSE;
        case CursorShape::ResizeNESW: return IDC_SIZENESW;
        case CursorShape::Grab:       return IDC_HAND;
        case CursorShape::Grabbing:   return IDC_SIZEALL;
        case CursorShape::Wait:       return IDC_WAIT;
        case CursorShape::Arrow:      break;
    }
    return IDC_ARROW;
}

constexpr const wchar_t* kClassName = L"FolioWindowClass";

// --- GL context --------------------------------------------------------------

class WglContext final : public IGlContext {
public:
    bool create(HWND hwnd, std::string* error) {
        hwnd_ = hwnd;
        hdc_ = GetDC(hwnd);
        if (!hdc_) { if (error) *error = "GetDC failed"; return false; }

        // No MSAA and no depth: Skia antialiases itself, and a depth buffer
        // on the main target is bandwidth nobody reads.
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        pfd.cStencilBits = 8;
        pfd.iLayerType = PFD_MAIN_PLANE;

        const int fmt = ChoosePixelFormat(hdc_, &pfd);
        if (!fmt || !SetPixelFormat(hdc_, fmt, &pfd)) {
            if (error) *error = "no suitable pixel format";
            return false;
        }
        PIXELFORMATDESCRIPTOR got{};
        DescribePixelFormat(hdc_, fmt, sizeof(got), &got);
        stencilBits_ = got.cStencilBits;

        HGLRC legacy = wglCreateContext(hdc_);
        if (!legacy) { if (error) *error = "wglCreateContext failed"; return false; }
        wglMakeCurrent(hdc_, legacy);

        // Upgrade to a 3.3 core context when the driver offers one. Ganesh
        // is happy with the compatibility context too, so a failure here is
        // not fatal - it just means an older driver.
        auto createAttribs = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARB>(
            wglGetProcAddress("wglCreateContextAttribsARB"));
        if (createAttribs) {
            const int attribs[] = {kWglContextMajorVersionArb, 3,
                                   kWglContextMinorVersionArb, 3,
                                   kWglContextProfileMaskArb,
                                   kWglContextCoreProfileBit, 0};
            if (HGLRC core = createAttribs(hdc_, nullptr, attribs)) {
                wglMakeCurrent(nullptr, nullptr);
                wglDeleteContext(legacy);
                rc_ = core;
                wglMakeCurrent(hdc_, rc_);
            }
        }
        if (!rc_) rc_ = legacy;

        swapInterval_ = reinterpret_cast<PFNWGLSWAPINTERVALEXT>(
            wglGetProcAddress("wglSwapIntervalEXT"));
        gl_ = GetModuleHandleW(L"opengl32.dll");
        return true;
    }

    ~WglContext() override {
        if (rc_) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(rc_); }
        if (hdc_ && hwnd_) ReleaseDC(hwnd_, hdc_);
    }

    void makeCurrent() override { wglMakeCurrent(hdc_, rc_); }
    void swapBuffers() override { SwapBuffers(hdc_); }
    void setSwapInterval(int interval) override {
        if (swapInterval_) swapInterval_(interval);
    }

    [[nodiscard]] ProcAddress procAddress(const char* name) const override {
        // wglGetProcAddress only resolves extensions; the GL 1.1 core that
        // ships in opengl32.dll has to come from GetProcAddress, and Skia
        // asks for both kinds.
        if (auto p = reinterpret_cast<ProcAddress>(wglGetProcAddress(name)))
            return p;
        if (gl_)
            return reinterpret_cast<ProcAddress>(GetProcAddress(gl_, name));
        return nullptr;
    }

    [[nodiscard]] int stencilBits() const override { return stencilBits_; }
    [[nodiscard]] int sampleCount() const override { return 1; }

private:
    HWND hwnd_ = nullptr;
    HDC hdc_ = nullptr;
    HGLRC rc_ = nullptr;
    HMODULE gl_ = nullptr;
    PFNWGLSWAPINTERVALEXT swapInterval_ = nullptr;
    int stencilBits_ = 8;
};

// --- Window ------------------------------------------------------------------

class Win32Window final : public IWindow {
public:
    bool create(const WindowDesc& desc, std::string* error);
    ~Win32Window() override;

    bool poll(Event& out) override;
    void waitEvents(int timeoutMs) override;
    void wake() override { PostMessageW(hwnd_, WM_NULL, 0, 0); }

    [[nodiscard]] int widthPx() const override { return w_; }
    [[nodiscard]] int heightPx() const override { return h_; }
    [[nodiscard]] float scale() const override { return scale_; }

    void setTitle(std::string_view title) override {
        SetWindowTextW(hwnd_, toWide(title).c_str());
    }
    void setCursor(CursorShape shape) override {
        cursor_ = LoadCursorW(nullptr, cursorIdFor(shape));
        ::SetCursor(cursor_);
    }

    void minimise() override { ShowWindow(hwnd_, SW_MINIMIZE); }
    void toggleMaximise() override {
        ShowWindow(hwnd_, maximised() ? SW_RESTORE : SW_MAXIMIZE);
    }
    void requestClose() override { PostMessageW(hwnd_, WM_CLOSE, 0, 0); }
    [[nodiscard]] bool maximised() const override {
        return IsZoomed(hwnd_) != 0;
    }

    void beginDragMove() override {
        // Hand the drag back to Windows so Aero Snap, multi-monitor and the
        // shake-to-minimise gesture all keep working. Releasing capture
        // first is required or the modal move loop never starts.
        ReleaseCapture();
        SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
    void beginDragResize(ResizeEdge edge) override {
        static constexpr WPARAM kHit[] = {HTTOP,     HTBOTTOM,    HTLEFT,
                                          HTRIGHT,   HTTOPLEFT,   HTTOPRIGHT,
                                          HTBOTTOMLEFT, HTBOTTOMRIGHT};
        ReleaseCapture();
        SendMessageW(hwnd_, WM_NCLBUTTONDOWN,
                     kHit[static_cast<std::size_t>(edge)], 0);
    }

    [[nodiscard]] IGlContext& gl() override { return gl_; }

    /// The common item dialog wants an owner so it centres on Folio and
    /// blocks the right window. Exposed rather than passing the handle
    /// around because the dialog is a process-wide singleton.
    [[nodiscard]] HWND nativeHandleForDialogs() const { return hwnd_; }
    void presentSoftware(const void* bgra, int w, int h, int stride) override;
    [[nodiscard]] bool shouldClose() const override { return close_; }

private:
    static LRESULT CALLBACK wndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT wndProc(UINT msg, WPARAM wp, LPARAM lp);
    void push(const Event& e) { queue_.push_back(e); }
    void pumpMessages();
    void updateScale();

    HWND hwnd_ = nullptr;
    WglContext gl_;
    std::deque<Event> queue_;
    int w_ = 0, h_ = 0;
    int minW_ = 880, minH_ = 560;
    float scale_ = 1.f;
    bool close_ = false;
    bool tracking_ = false;
    std::unordered_set<UINT32> activePointers_;
    HCURSOR cursor_ = nullptr;
};

Win32Window* g_window = nullptr;

LRESULT CALLBACK Win32Window::wndProcThunk(HWND hwnd, UINT msg, WPARAM wp,
                                           LPARAM lp) {
    if (g_window && g_window->hwnd_ == hwnd) return g_window->wndProc(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Win32Window::updateScale() {
    UINT dpi = 96;
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        if (auto fn = reinterpret_cast<GetDpiForWindowFn>(
                GetProcAddress(u32, "GetDpiForWindow"))) {
            dpi = fn(hwnd_);
        }
    }
    if (dpi == 0) dpi = 96;
    scale_ = static_cast<float>(dpi) / 96.f;
}

LRESULT Win32Window::wndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE: {
            if (wp != TRUE) break;
            // Borderless: keep the style bits (snap, shadows, the taskbar
            // menu) but give the whole window over to the client area.
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            if (IsZoomed(hwnd_)) {
                // A maximised WS_OVERLAPPEDWINDOW is deliberately larger
                // than the monitor by the frame thickness. Without this
                // inset the top of Folio's own title bar sits off-screen.
                const int fx = GetSystemMetrics(SM_CXSIZEFRAME) +
                               GetSystemMetrics(SM_CXPADDEDBORDER);
                const int fy = GetSystemMetrics(SM_CYSIZEFRAME) +
                               GetSystemMetrics(SM_CXPADDEDBORDER);
                p->rgrc[0].left += fx;
                p->rgrc[0].right -= fx;
                p->rgrc[0].top += fy;
                p->rgrc[0].bottom -= fy;
            }
            return 0;
        }

        case WM_NCHITTEST: {
            // Resize borders by hit-test rather than by style, so the user
            // can grab an edge even though there is no visible frame. The
            // caption is left to the app: it calls beginDragMove() when the
            // pointer is over a draggable part of its own title bar, which
            // it knows and this code does not.
            if (IsZoomed(hwnd_)) return HTCLIENT;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            const int b = static_cast<int>(6.f * scale_);
            const bool left = pt.x < b, right = pt.x >= w_ - b;
            const bool top = pt.y < b, bottom = pt.y >= h_ - b;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            return HTCLIENT;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = static_cast<LONG>(minW_ * scale_);
            mmi->ptMinTrackSize.y = static_cast<LONG>(minH_ * scale_);
            return 0;
        }

        case WM_SIZE: {
            if (wp == SIZE_MINIMIZED) {
                Event e;
                e.type = Event::Type::Minimized;
                push(e);
                return 0;
            }
            w_ = LOWORD(lp);
            h_ = HIWORD(lp);
            Event e;
            e.type = Event::Type::Resize;
            e.w = w_; e.h = h_; e.scale = scale_;
            push(e);
            return 0;
        }

        case WM_DPICHANGED: {
            updateScale();
            // Windows hands us the rectangle the window should move to so
            // the physical size is preserved across the monitor change.
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Event e;
            e.type = Event::Type::ScaleChange;
            e.scale = scale_; e.w = w_; e.h = h_;
            push(e);
            return 0;
        }

        case WM_PAINT: {
            ValidateRect(hwnd_, nullptr);
            Event e; e.type = Event::Type::Expose; push(e);
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                ::SetCursor(cursor_ ? cursor_ : LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_POINTERDOWN: case WM_POINTERUPDATE: case WM_POINTERUP:
        case WM_POINTERCAPTURECHANGED: {
            const UINT32 id = GET_POINTERID_WPARAM(wp);
            if (msg == WM_POINTERCAPTURECHANGED) {
                if (activePointers_.erase(id)) {
                    Event cancel;
                    cancel.type = Event::Type::PointerCancel;
                    cancel.source = PointerSource::Touch;
                    cancel.pointerId = id;
                    push(cancel);
                }
                return 0;
            }
            POINTER_INFO info{};
            if (!GetPointerInfo(id, &info) ||
                (info.pointerType != PT_TOUCH && info.pointerType != PT_PEN))
                break;
            if (msg == WM_POINTERDOWN) activePointers_.insert(id);
            if (msg == WM_POINTERUP && !activePointers_.erase(id)) return 0;
            POINT pt = info.ptPixelLocation;
            ScreenToClient(hwnd_, &pt);
            Event e;
            e.type = msg == WM_POINTERDOWN ? Event::Type::PointerDown
                   : msg == WM_POINTERUP ? Event::Type::PointerUp
                   : msg == WM_POINTERCAPTURECHANGED
                       ? Event::Type::PointerCancel
                       : Event::Type::PointerMove;
            e.x = pt.x; e.y = pt.y;
            e.pointerId = id;
            e.source = info.pointerType == PT_PEN ? PointerSource::Pen
                                                   : PointerSource::Touch;
            e.pressure = e.type == Event::Type::PointerUp ? 0.f : 1.f;
            e.mods = currentMods();
            if (info.pointerType == PT_PEN) {
                POINTER_PEN_INFO pen{};
                if (GetPointerPenInfo(id, &pen)) {
                    e.pressure = e.type == Event::Type::PointerUp ? 0.f :
                        std::clamp(static_cast<float>(pen.pressure) / 1024.f,
                                   0.f, 1.f);
                    e.tiltX = static_cast<float>(pen.tiltX);
                    e.tiltY = static_cast<float>(pen.tiltY);
                }
            }
            push(e);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if ((static_cast<ULONG_PTR>(GetMessageExtraInfo()) & 0xFFFFFF00u)
                 == 0xFF515700u) return 0;
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            Event e;
            e.type = Event::Type::PointerMove;
            e.x = GET_X_LPARAM(lp); e.y = GET_Y_LPARAM(lp);
            e.mods = currentMods();
            push(e);
            return 0;
        }

        case WM_MOUSELEAVE: {
            tracking_ = false;
            Event e; e.type = Event::Type::PointerLeave; push(e);
            return 0;
        }

        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
            if ((static_cast<ULONG_PTR>(GetMessageExtraInfo()) & 0xFFFFFF00u)
                 == 0xFF515700u) return 0;
            SetCapture(hwnd_);
            Event e;
            e.type = Event::Type::PointerDown;
            e.x = GET_X_LPARAM(lp); e.y = GET_Y_LPARAM(lp);
            e.button = (msg == WM_RBUTTONDOWN)   ? Button::Right
                       : (msg == WM_MBUTTONDOWN) ? Button::Middle
                                                 : Button::Left;
            e.clickCount = (msg == WM_LBUTTONDBLCLK) ? 2 : 1;
            e.mods = currentMods();
            push(e);
            return 0;
        }

        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
            if ((static_cast<ULONG_PTR>(GetMessageExtraInfo()) & 0xFFFFFF00u)
                 == 0xFF515700u) return 0;
            ReleaseCapture();
            Event e;
            e.type = Event::Type::PointerUp;
            e.x = GET_X_LPARAM(lp); e.y = GET_Y_LPARAM(lp);
            e.button = (msg == WM_RBUTTONUP)   ? Button::Right
                       : (msg == WM_MBUTTONUP) ? Button::Middle
                                               : Button::Left;
            e.mods = currentMods();
            push(e);
            return 0;
        }

        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
            // Wheel messages arrive in *screen* coordinates, unlike every
            // other mouse message. Getting this wrong puts zoom-at-cursor
            // in the wrong place on every non-maximised window.
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            Event e;
            e.type = Event::Type::Wheel;
            e.x = pt.x; e.y = pt.y;
            const double d = GET_WHEEL_DELTA_WPARAM(wp) /
                             static_cast<double>(WHEEL_DELTA);
            if (msg == WM_MOUSEWHEEL) e.wheelY = d; else e.wheelX = d;
            e.mods = currentMods();
            push(e);
            return 0;
        }

        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            Event e;
            e.type = Event::Type::KeyDown;
            e.key = mapKey(wp);
            e.repeat = (lp & (1 << 30)) != 0;
            e.mods = currentMods();
            if (e.key != Key::Unknown) push(e);
            // Alt+F4 and friends still need the default handler.
            if (msg == WM_SYSKEYDOWN) break;
            return 0;
        }

        case WM_KEYUP: case WM_SYSKEYUP: {
            Event e;
            e.type = Event::Type::KeyUp;
            e.key = mapKey(wp);
            e.mods = currentMods();
            if (e.key != Key::Unknown) push(e);
            if (msg == WM_SYSKEYUP) break;
            return 0;
        }

        case WM_CHAR: {
            // Surrogate pairs arrive as two WM_CHARs; buffer the high half
            // so astral-plane input (emoji) is not split into mojibake.
            static wchar_t pendingHigh = 0;
            const auto c = static_cast<wchar_t>(wp);
            if (c < 0x20 || c == 0x7F) return 0;   // control codes are not text
            wchar_t buf[2];
            int n = 0;
            if (c >= 0xD800 && c <= 0xDBFF) { pendingHigh = c; return 0; }
            if (c >= 0xDC00 && c <= 0xDFFF && pendingHigh) {
                buf[0] = pendingHigh; buf[1] = c; n = 2; pendingHigh = 0;
            } else {
                buf[0] = c; n = 1;
            }
            Event e;
            e.type = Event::Type::Text;
            e.text = toUtf8(buf, n);
            e.mods = currentMods();
            if (!e.text.empty()) push(e);
            return 0;
        }

        case WM_SETFOCUS: { Event e; e.type = Event::Type::FocusIn; push(e); return 0; }
        case WM_KILLFOCUS: { Event e; e.type = Event::Type::FocusOut; push(e); return 0; }

        case WM_CLOSE: {
            Event e; e.type = Event::Type::Close; push(e);
            close_ = true;
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

bool Win32Window::create(const WindowDesc& desc, std::string* error) {
    // Per-monitor v2 before any window exists, or the first window is
    // created at the wrong DPI and never recovers.
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        if (auto fn = reinterpret_cast<SetCtxFn>(
                GetProcAddress(u32, "SetProcessDpiAwarenessContext"))) {
            fn(reinterpret_cast<HANDLE>(-4));  // PER_MONITOR_AWARE_V2
        } else {
            SetProcessDPIAware();
        }
    }

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS | CS_OWNDC;
    wc.lpfnWndProc = &Win32Window::wndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = nullptr;        // WM_SETCURSOR owns this
    wc.hbrBackground = nullptr;  // GL paints every pixel; no flicker fill
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    minW_ = desc.minWidth;
    minH_ = desc.minHeight;
    g_window = this;

    const DWORD style = desc.systemDecorations
                            ? WS_OVERLAPPEDWINDOW
                            : (WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN |
                               WS_CLIPSIBLINGS);
    hwnd_ = CreateWindowExW(0, kClassName, toWide(desc.title).c_str(), style,
                            CW_USEDEFAULT, CW_USEDEFAULT, desc.width,
                            desc.height, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) {
        if (error) *error = "CreateWindowEx failed";
        g_window = nullptr;
        return false;
    }

    updateScale();
    if (scale_ != 1.f) {
        // CW_USEDEFAULT sized us in pixels at the default DPI; resize so the
        // logical size the caller asked for is what they get.
        SetWindowPos(hwnd_, nullptr, 0, 0,
                     static_cast<int>(desc.width * scale_),
                     static_cast<int>(desc.height * scale_),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Tell the DWM the frame changed, or the removed non-client area is not
    // recalculated until the first manual resize.
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE);

    if (!gl_.create(hwnd_, error)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        g_window = nullptr;
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    w_ = rc.right - rc.left;
    h_ = rc.bottom - rc.top;

    cursor_ = LoadCursorW(nullptr, IDC_ARROW);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

Win32Window::~Win32Window() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (g_window == this) g_window = nullptr;
}

void Win32Window::pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { close_ = true; break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool Win32Window::poll(Event& out) {
    if (queue_.empty()) pumpMessages();
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void Win32Window::waitEvents(int timeoutMs) {
    if (!queue_.empty()) return;
    // Block in the kernel rather than spinning: this is the idle rule the
    // shell relies on to stop Folio burning a core on a static page.
    MsgWaitForMultipleObjectsEx(
        0, nullptr, timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs),
        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    pumpMessages();
}

void Win32Window::presentSoftware(const void* bgra, int w, int h, int stride) {
    HDC dc = GetDC(hwnd_);
    if (!dc) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = stride / 4;
    bi.bmiHeader.biHeight = -h;   // negative: top-down, matching our buffers
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(dc, 0, 0, w, h, 0, 0, w, h, bgra, &bi, DIB_RGB_COLORS,
                  SRCCOPY);
    ReleaseDC(hwnd_, dc);
}

// --- Clipboard ---------------------------------------------------------------

class Win32Clipboard final : public IClipboard {
public:
    bool setText(std::string_view text) override {
        if (!OpenClipboard(nullptr)) return false;
        EmptyClipboard();
        const std::wstring w = toWide(text);
        const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!h) { CloseClipboard(); return false; }
        if (void* p = GlobalLock(h)) {
            std::memcpy(p, w.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
        return true;
    }

    [[nodiscard]] std::string text() override {
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return {};
        if (!OpenClipboard(nullptr)) return {};
        std::string out;
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                out = toUtf8(p);
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        return out;
    }
};

// --- File dialogs ------------------------------------------------------------

class Win32FileDialog final : public IFileDialog {
public:
    [[nodiscard]] std::optional<std::string> openFile(
        const FileDialogRequest& req) override {
        return run(req, false);
    }
    [[nodiscard]] std::optional<std::string> saveFile(
        const FileDialogRequest& req) override {
        return run(req, true);
    }
    [[nodiscard]] bool available() const override { return true; }

private:
    static std::optional<std::string> run(const FileDialogRequest& req,
                                          bool save) {
        // Apartment-threaded: the common item dialog is a UI object and the
        // calling thread is the UI thread.
        const HRESULT init = CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(init)) return std::nullopt; // shell dialogs need STA
        const bool owns = true;

        ::IFileDialog* dlg = nullptr;
        const HRESULT hr = CoCreateInstance(
            save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
        std::optional<std::string> result;

        if (SUCCEEDED(hr) && dlg) {
            DWORD opts = 0;
            if (SUCCEEDED(dlg->GetOptions(&opts)))
                dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            if (!req.title.empty()) dlg->SetTitle(toWide(req.title).c_str());
            if (!req.suggestedName.empty())
                dlg->SetFileName(toWide(req.suggestedName).c_str());
            if (save) dlg->SetDefaultExtension(L"pdf");
            if (!req.startDirectory.empty()) {
                IShellItem* folder = nullptr;
                const auto dir = toWide(req.startDirectory);
                if (SUCCEEDED(SHCreateItemFromParsingName(
                        dir.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
                    dlg->SetDefaultFolder(folder);
                    folder->Release();
                }
            }

            // The filter spec has to outlive the call, so the wide strings
            // are kept alive in these vectors rather than built inline.
            std::vector<std::wstring> keep;
            std::vector<COMDLG_FILTERSPEC> specs;
            keep.reserve(req.filters.size() * 2);
            for (const FileFilter& f : req.filters) {
                std::wstring pat;
                for (const std::string& p : f.patterns) {
                    if (!pat.empty()) pat += L';';
                    pat += toWide(p);
                }
                keep.push_back(toWide(f.label));
                keep.push_back(pat);
                specs.push_back({keep[keep.size() - 2].c_str(),
                                 keep[keep.size() - 1].c_str()});
            }
            if (!specs.empty())
                dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());

            HWND owner = g_window ? g_window->nativeHandleForDialogs() : nullptr;
            if (SUCCEEDED(dlg->Show(owner))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        result = toUtf8(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dlg->Release();
        }
        if (owns) CoUninitialize();
        return result;
    }
};

}  // namespace

std::unique_ptr<IWindow> createWindow(const WindowDesc& desc,
                                      std::string* error) {
    auto w = std::make_unique<Win32Window>();
    if (!w->create(desc, error)) return nullptr;
    return w;
}

IClipboard& clipboard() {
    static Win32Clipboard c;
    return c;
}

IFileDialog& fileDialog() {
    static Win32FileDialog d;
    return d;
}

const char* backendName() noexcept { return "win32"; }

}  // namespace folio::platform
