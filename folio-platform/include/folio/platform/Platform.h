// folio-platform/include/folio/platform/Platform.h
//
// THE PLATFORM SEAM
// =================
// Everything that differs between Win32, Cocoa and X11/Wayland lives behind
// the four interfaces in this header. Nothing else in Folio may include a
// windowing-system header.
//
// Two rules keep this layer thin, and they are worth stating because both are
// easy to break by accident:
//
//   1. This header does not know about Skia. A backend hands out a current GL
//      context and a `swapBuffers`; building a GPU-backed canvas on top of
//      that is folio-render's job. If `IGlContext` ever grows an `SkCanvas*`
//      the seam is gone.
//
//   2. This header does not know about folio-ui. Events are described in
//      platform terms (a button index, a key code, a UTF-8 string) and the
//      shell translates them into `folio::ui::PointerEvent` / `KeyEvent`.
//      Otherwise every backend would need the widget library to compile.
//
// WINDOW DECORATIONS
// ------------------
// Folio draws its own title bar, because the brief is one identical design on
// every platform and server-side decorations are three different designs. So
// every backend creates an undecorated window and the platform layer exposes
// the three things a custom title bar cannot do for itself: start a system
// move (`beginDragMove`), start a system resize (`beginDragResize`), and run
// the real minimise / maximise / close.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(__ANDROID__)
struct android_app;  // <android_native_app_glue.h>, global scope
#endif

namespace folio::platform {

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

/// Mouse buttons, as indices rather than a bitmask: the UI layer cares which
/// button, not which combination.
enum class Button : std::uint8_t { Left = 0, Middle = 1, Right = 2, Other = 3 };

/// Modifier bits. Deliberately the same values and order as
/// `folio::ui::Modifier`, so the shell's translation is a cast rather than a
/// switch - but declared separately so neither layer includes the other.
enum Modifier : std::uint32_t {
    ModNone  = 0,
    ModShift = 1u << 0,
    ModCtrl  = 1u << 1,
    ModAlt   = 1u << 2,
    ModMeta  = 1u << 3,
};

/// Keys Folio actually binds. A backend maps its native code to one of these
/// and drops the rest; printable characters arrive separately as `text`,
/// already composed by the platform's input method, which is the only way to
/// get dead keys and CJK input right.
enum class Key : std::uint16_t {
    Unknown = 0,
    A, C, F, N, O, P, S, T, W, Z, Y,
    Digit0, Digit1, Digit2, Digit3, Digit4,
    Digit5, Digit6, Digit7, Digit8, Digit9,
    Left, Right, Up, Down,
    PageUp, PageDown, Home, End,
    Enter, Escape, Tab, Backspace, Delete, Space,
    Plus, Minus, F11,
};

/// What physically produced a pointer event. The UI layer needs this to
/// decide between behaviours that cannot all be right at once: a finger
/// wants inflated hit targets and gesture interpretation, a pen wants
/// neither and carries pressure, a mouse wants exactly what it always had.
enum class PointerSource : std::uint8_t { Mouse, Touch, Pen };

struct Event {
    enum class Type : std::uint8_t {
        None,
        Close,          ///< the user asked the window to go away
        Resize,         ///< `w`/`h` are new pixel dimensions
        Minimized,      ///< free the retained scene and all visual caches
        ScaleChange,    ///< `scale` changed (moved to a different monitor)
        SafeArea,       ///< Android: top inset still covering the native surface, in `h` pixels
        Expose,         ///< the compositor wants a frame
        PointerDown,
        PointerUp,
        PointerMove,
        PointerLeave,
        PointerCancel,  ///< touch sequence abandoned (grab stolen, device lost)
        Wheel,          ///< `wheelX`/`wheelY` in lines; positive y is "away"
        KeyDown,
        KeyUp,
        Text,           ///< composed input, UTF-8, in `text`
        FileOpened,     ///< Android SAF import finished; local path in `text`
        FileExported,   ///< Android SAF result: status in `w`, error in `text`
        StorageChanged, ///< Android source/copy operation: status in `w`, message in `text`
        FocusIn,
        FocusOut,
    };

    Type type = Type::None;

    // Pointer
    double x = 0.0, y = 0.0;         ///< logical pixels, window-relative
    double wheelX = 0.0, wheelY = 0.0;
    Button button = Button::Left;
    int clickCount = 1;              ///< 2 on a double click

    // Pointer device. `pointerId` distinguishes simultaneous contacts: it is
    // the touch sequence number for a finger and 0 for the system pointer,
    // so a single-pointer backend needs to set none of this.
    PointerSource source = PointerSource::Mouse;
    std::uint32_t pointerId = 0;
    // OS sample time in monotonic seconds (Android MotionEvent history).
    // Zero means the backend has no timestamp; the shell uses receive time.
    double inputTimeSeconds = 0.0;
    float pressure = 0.f;            ///< 0..1, pen only; 0 when unsupported
    float tiltX = 0.f, tiltY = 0.f;  ///< degrees from vertical, pen only

    // Keyboard
    Key key = Key::Unknown;
    bool repeat = false;
    std::string text;

    // Window
    int w = 0, h = 0;
    float scale = 1.f;
    std::uint32_t mods = ModNone;
};

// ---------------------------------------------------------------------------
// GL context
// ---------------------------------------------------------------------------

/// A current-able GL (or GLES) context attached to a window.
///
/// Folio asks for the lowest common denominator that Ganesh is happy with:
/// an RGBA8 default framebuffer with a stencil buffer, no depth, no MSAA
/// (Skia does its own antialiasing and MSAA on the main target is wasted
/// bandwidth on every platform we ship to).
class IGlContext {
public:
    virtual ~IGlContext() = default;

    virtual void makeCurrent() = 0;
    virtual void swapBuffers() = 0;

    /// 1 = vsync, 0 = off. Folio runs with vsync on and relies on
    /// `FolioApp::needsFrame` to idle; a spinning render loop is how you get a
    /// laptop fan in a PDF reader.
    virtual void setSwapInterval(int interval) = 0;

    /// For `GrGLMakeAssembledInterface`. Must work for core and extension
    /// entry points alike.
    using ProcAddress = void (*)();
    [[nodiscard]] virtual ProcAddress procAddress(const char* name) const = 0;

    /// Stencil bits and sample count of the default framebuffer, which
    /// Ganesh needs in order to wrap it.
    [[nodiscard]] virtual int stencilBits() const = 0;
    [[nodiscard]] virtual int sampleCount() const = 0;

    /// The FBO the window's surface is bound to. Zero everywhere except iOS,
    /// where UIKit hands you a non-zero default framebuffer.
    [[nodiscard]] virtual std::uint32_t defaultFramebuffer() const { return 0; }
};

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

enum class CursorShape : std::uint8_t {
    Arrow, Hand, IBeam, Crosshair, ResizeH, ResizeV, ResizeNWSE, ResizeNESW,
    Grab, Grabbing, Wait,
};

/// Which edge a resize drag started from, for `beginDragResize`.
enum class ResizeEdge : std::uint8_t {
    Top, Bottom, Left, Right,
    TopLeft, TopRight, BottomLeft, BottomRight,
};

struct WindowDesc {
    std::string title = "Folio";
    int width = 1440;
    int height = 900;
    int minWidth = 880;
    int minHeight = 560;
    /// Folio always wants this false - see the note at the top of the file.
    bool systemDecorations = false;
};

class IWindow {
public:
    virtual ~IWindow() = default;

    /// Pull the next event. Returns false when the queue is empty.
    virtual bool poll(Event& out) = 0;

    /// Block until an event arrives or `timeoutMs` elapses. A negative
    /// timeout blocks indefinitely. This is what makes an idle Folio cost
    /// zero CPU rather than 60 wake-ups a second.
    virtual void waitEvents(int timeoutMs) = 0;

    /// Make `waitEvents` return early from another thread. The one method
    /// here that must be thread-safe, because a worker finishing a page
    /// raster needs to wake the UI.
    virtual void wake() = 0;

    [[nodiscard]] virtual int widthPx() const = 0;
    [[nodiscard]] virtual int heightPx() const = 0;
    /// Device pixels per logical pixel. 2.0 on a Mac Retina display, 1.5 on
    /// a typical 150% Windows setup.
    [[nodiscard]] virtual float scale() const = 0;

    virtual void setTitle(std::string_view title) = 0;
    virtual void setCursor(CursorShape shape) = 0;

    // The three operations a self-drawn title bar cannot perform itself.
    virtual void minimise() = 0;
    virtual void toggleMaximise() = 0;
    virtual void requestClose() = 0;
    [[nodiscard]] virtual bool maximised() const = 0;

    /// Hand the drag to the window manager. Called on pointer-down in the
    /// title bar; the WM then owns the gesture, which is the only way to get
    /// snapping, edge tiling and multi-monitor behaviour right.
    virtual void beginDragMove() = 0;
    virtual void beginDragResize(ResizeEdge edge) = 0;

    /// The GL context for this window, created with it.
    [[nodiscard]] virtual IGlContext& gl() = 0;

    /// Present a CPU-rendered frame: BGRA8888, premultiplied, top-down.
    ///
    /// The software fallback must not go through GL. If Ganesh could not
    /// start, the reason is usually that GL itself is unusable on this
    /// machine, so presenting through it would fail the same way - just
    /// silently, with a black window instead of an error.
    virtual void presentSoftware(const void* bgra, int w, int h,
                                 int strideBytes) = 0;

    /// True once the user has closed the window.
    [[nodiscard]] virtual bool shouldClose() const = 0;
};

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

/// Text-only, which is all Folio needs today (copy from the text layer).
///
/// `text()` may block: on X11 the clipboard is a conversation with whichever
/// client currently owns the selection, and that client may be slow or gone.
/// Backends apply a short timeout rather than hanging the UI thread.
class IClipboard {
public:
    virtual ~IClipboard() = default;
    virtual bool setText(std::string_view text) = 0;
    [[nodiscard]] virtual std::string text() = 0;
};

// ---------------------------------------------------------------------------
// File dialogs
// ---------------------------------------------------------------------------

struct FileFilter {
    std::string label;                 ///< "PDF documents"
    std::vector<std::string> patterns; ///< {"*.pdf"}
};

struct FileDialogRequest {
    std::string title;
    std::string startDirectory;
    std::string suggestedName;
    std::vector<FileFilter> filters;
};

/// Blocking, modal, native. Returns nullopt when the user cancels.
///
/// These block the calling thread by design: a file dialog is the one place
/// where a modal pause matches what the user already expects, and threading
/// it buys nothing but races.
class IFileDialog {
public:
    virtual ~IFileDialog() = default;
    [[nodiscard]] virtual std::optional<std::string> openFile(
        const FileDialogRequest& req) = 0;
    [[nodiscard]] virtual std::optional<std::string> saveFile(
        const FileDialogRequest& req) = 0;
    /// False when no native dialog is reachable (a bare X session with no
    /// portal and no zenity). The caller then falls back to Folio's own
    /// in-app `OpenDialog`, which is why that component still exists.
    [[nodiscard]] virtual bool available() const = 0;
};

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

/// Create the platform's window. Returns nullptr and fills `error` when there
/// is no display server to talk to - the common case in CI and containers,
/// where it must fail cleanly rather than abort.
std::unique_ptr<IWindow> createWindow(const WindowDesc& desc, std::string* error);

IClipboard& clipboard();
IFileDialog& fileDialog();

/// Which backend was compiled in: "x11", "win32", "cocoa", or "none".
[[nodiscard]] const char* backendName() noexcept;

#if defined(__ANDROID__)
/// Hand the activity to the platform layer. Must be called by
/// `android_main` before `createWindow`, because on Android the process
/// does not own its window - the activity does, and everything here is
/// reached through it.
///
/// `::android_app` is forward-declared at global scope above: declaring it
/// here would create a distinct folio::platform::android_app that silently
/// fails to match the real one.
void setAndroidApp(::android_app* app) noexcept;
/// The document picker is asynchronous so NativeActivity can keep pumping
/// lifecycle events while the system picker is in front of it.
bool requestAndroidPdfPicker();
bool requestAndroidPdfExport(const std::string& staged, const std::string& name);
bool requestAndroidPdfShare(const std::string& staged, const std::string& name);
int openAndroidLinkedPdfFd(const std::string& stablePath);
bool requestAndroidStorageAction(const std::string& stablePath, int action);
bool androidPrefersSourceOnly();
void setAndroidPrefersSourceOnly(bool enabled);
void showAndroidKeyboard(bool visible);
void setAndroidSystemBarsDark(bool dark);
void setAndroidSystemBarsColor(int argb, bool darkIcons);
#endif

}  // namespace folio::platform
