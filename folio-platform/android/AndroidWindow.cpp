// folio-platform/android/AndroidWindow.cpp
//
// The Android backend: an ANativeWindow, an EGL/GLES context, and the
// translation of Android's batched multi-touch into Folio's one-event-per-
// pointer model.
//
// Three things make Android different from every desktop backend, and all
// three are structural rather than cosmetic:
//
//  1. There is no window to create. The system hands one to the activity
//     when it feels like it, takes it away on rotate or background, and may
//     never give it back. `createWindow` therefore does not create
//     anything - it waits for APP_CMD_INIT_WINDOW and then adopts what it
//     is given. Losing the surface is normal operation, not an error.
//
//  2. One MotionEvent carries every pointer. A two-finger pinch is a single
//     ACTION_MOVE holding both fingers' coordinates, plus the historical
//     samples the system batched since the last frame. Folio's event model
//     is one pointer per event, so each MotionEvent fans out.
//
//  3. The process can be killed between frames. Nothing here owns state
//     that cannot be rebuilt from the surface.
#include "folio/platform/Platform.h"

#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <string>

#define FOLIO_LOG(...) \
    __android_log_print(ANDROID_LOG_INFO, "folio", __VA_ARGS__)

namespace folio::platform {
namespace {

/// The glue's `android_app`, handed in by `android_main` before anything
/// else runs. A process has exactly one activity, so this is a genuine
/// singleton rather than a convenience global.
::android_app* g_app = nullptr;

constexpr double kPi = 3.14159265358979323846;

}  // namespace

void setAndroidApp(::android_app* app) noexcept { g_app = app; }

namespace {

// ---------------------------------------------------------------------------
// EGL
// ---------------------------------------------------------------------------

class AndroidGlContext final : public IGlContext {
public:
    bool bind(ANativeWindow* win, std::string* error) {
        if (display_ == EGL_NO_DISPLAY) {
            display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (display_ == EGL_NO_DISPLAY) {
                if (error) *error = "eglGetDisplay failed";
                return false;
            }
            if (!eglInitialize(display_, nullptr, nullptr)) {
                if (error) *error = "eglInitialize failed";
                return false;
            }
            // Stencil 8, no depth, no MSAA: exactly what Ganesh wants and
            // nothing it would have to pay for and discard.
            const EGLint attribs[] = {EGL_RENDERABLE_TYPE,
                                      EGL_OPENGL_ES3_BIT_KHR,
                                      EGL_SURFACE_TYPE,
                                      EGL_WINDOW_BIT,
                                      EGL_RED_SIZE,
                                      8,
                                      EGL_GREEN_SIZE,
                                      8,
                                      EGL_BLUE_SIZE,
                                      8,
                                      EGL_ALPHA_SIZE,
                                      8,
                                      EGL_STENCIL_SIZE,
                                      8,
                                      EGL_DEPTH_SIZE,
                                      0,
                                      EGL_NONE};
            EGLint num = 0;
            if (!eglChooseConfig(display_, attribs, &config_, 1, &num) ||
                num < 1) {
                if (error) *error = "no suitable EGL config";
                return false;
            }
        }

        // The native window must be reconfigured to the format the chosen
        // EGL config expects, or eglCreateWindowSurface fails on some
        // drivers and silently renders wrong colours on others.
        EGLint format = 0;
        eglGetConfigAttrib(display_, config_, EGL_NATIVE_VISUAL_ID, &format);
        ANativeWindow_setBuffersGeometry(win, 0, 0, format);

        surface_ = eglCreateWindowSurface(display_, config_, win, nullptr);
        if (surface_ == EGL_NO_SURFACE) {
            if (error) *error = "eglCreateWindowSurface failed";
            return false;
        }

        if (context_ == EGL_NO_CONTEXT) {
            const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                         EGL_NONE};
            context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT,
                                        ctxAttribs);
            if (context_ == EGL_NO_CONTEXT) {
                if (error) *error = "eglCreateContext failed";
                return false;
            }
        }

        if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
            if (error) *error = "eglMakeCurrent failed";
            return false;
        }
        eglQuerySurface(display_, surface_, EGL_WIDTH, &width_);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &height_);
        eglGetConfigAttrib(display_, config_, EGL_STENCIL_SIZE, &stencil_);
        eglGetConfigAttrib(display_, config_, EGL_SAMPLES, &samples_);
        return true;
    }

    /// The surface is gone (backgrounded, rotated). The *context* is kept:
    /// recreating it would throw away every texture Skia has uploaded, and
    /// Android hands back a compatible surface in the overwhelming majority
    /// of cases.
    void unbind() {
        if (display_ == EGL_NO_DISPLAY) return;
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
    }

    void teardown() {
        unbind();
        if (display_ != EGL_NO_DISPLAY) {
            if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
            eglTerminate(display_);
        }
        display_ = EGL_NO_DISPLAY;
        context_ = EGL_NO_CONTEXT;
    }

    void makeCurrent() override {
        if (surface_ != EGL_NO_SURFACE)
            eglMakeCurrent(display_, surface_, surface_, context_);
    }
    void swapBuffers() override {
        if (surface_ != EGL_NO_SURFACE) eglSwapBuffers(display_, surface_);
    }
    void setSwapInterval(int interval) override {
        if (display_ != EGL_NO_DISPLAY) eglSwapInterval(display_, interval);
    }
    [[nodiscard]] ProcAddress procAddress(const char* name) const override {
        return reinterpret_cast<ProcAddress>(eglGetProcAddress(name));
    }
    [[nodiscard]] int stencilBits() const override { return stencil_; }
    [[nodiscard]] int sampleCount() const override {
        return samples_ > 0 ? samples_ : 1;
    }

    [[nodiscard]] bool live() const noexcept {
        return surface_ != EGL_NO_SURFACE;
    }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    void resized(int w, int h) noexcept {
        width_ = w;
        height_ = h;
    }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLConfig config_ = nullptr;
    EGLint width_ = 0, height_ = 0;
    EGLint stencil_ = 8, samples_ = 0;
};

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

class AndroidWindow final : public IWindow {
public:
    static AndroidWindow* instance;

    bool create(const WindowDesc&, std::string* error) {
        if (!g_app) {
            if (error)
                *error = "no android_app - android_main must call "
                         "folio::platform::setAndroidApp() first";
            return false;
        }
        instance = this;
        g_app->userData = this;
        g_app->onAppCmd = &AndroidWindow::onCmd;
        g_app->onInputEvent = &AndroidWindow::onInput;

        if (g_app->config) {
            const int dpi = AConfiguration_getDensity(g_app->config);
            // ACONFIGURATION_DENSITY_NONE/ANY come back as absurd values;
            // 160 dpi is Android's definition of scale 1.0.
            if (dpi > 0 && dpi < 1000)
                scale_ = static_cast<float>(dpi) / 160.f;
        }

        // Block until the system gives us a surface. Until then there is
        // nothing to render into and no size to report.
        while (!gl_.live() && !shouldClose_) {
            pumpLooper(-1);
            if (g_app->destroyRequested) {
                shouldClose_ = true;
                if (error) *error = "activity destroyed before a surface arrived";
                return false;
            }
        }
        return gl_.live();
    }

    // --- IWindow ----------------------------------------------------------

    bool poll(Event& out) override {
        pumpLooper(0);
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void waitEvents(int timeoutMs) override {
        if (!queue_.empty()) return;
        pumpLooper(timeoutMs);
    }

    /// ALooper_wake is the one documented thread-safe entry point into a
    /// looper, which is exactly the contract this method needs.
    void wake() override {
        if (g_app && g_app->looper) ALooper_wake(g_app->looper);
    }

    [[nodiscard]] int widthPx() const override { return gl_.width(); }
    [[nodiscard]] int heightPx() const override { return gl_.height(); }
    [[nodiscard]] float scale() const override { return scale_; }

    // A fullscreen activity has no title bar, no minimise, no maximise and
    // no window to drag. These are not unimplemented - they are meaningless
    // here, and the UI already hides the controls that would call them.
    void setTitle(std::string_view) override {}
    void setCursor(CursorShape) override {}
    void minimise() override {}
    void toggleMaximise() override {}
    [[nodiscard]] bool maximised() const override { return true; }
    void beginDragMove() override {}
    void beginDragResize(ResizeEdge) override {}

    void requestClose() override {
        shouldClose_ = true;
        if (g_app) ANativeActivity_finish(g_app->activity);
    }
    [[nodiscard]] bool shouldClose() const override { return shouldClose_; }

    [[nodiscard]] IGlContext& gl() override { return gl_; }

    void presentSoftware(const void* bgra, int w, int h,
                         int strideBytes) override {
        if (!g_app || !g_app->window) return;
        ANativeWindow_Buffer buf;
        if (ANativeWindow_lock(g_app->window, &buf, nullptr) != 0) return;
        const auto* src = static_cast<const std::uint8_t*>(bgra);
        auto* dst = static_cast<std::uint8_t*>(buf.bits);
        const int rows = std::min(h, buf.height);
        const int cols = std::min(w, buf.width);
        for (int y = 0; y < rows; ++y) {
            const std::uint8_t* s = src + static_cast<std::size_t>(y) *
                                              static_cast<std::size_t>(strideBytes);
            std::uint8_t* d = dst + static_cast<std::size_t>(y) *
                                        static_cast<std::size_t>(buf.stride) * 4;
            // ANativeWindow hands back RGBA_8888; Folio's software path is
            // BGRA. Swapping here keeps the fallback honest rather than
            // presenting a blue-tinted page.
            for (int x = 0; x < cols; ++x) {
                d[x * 4 + 0] = s[x * 4 + 2];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + 0];
                d[x * 4 + 3] = s[x * 4 + 3];
            }
        }
        ANativeWindow_unlockAndPost(g_app->window);
    }

private:
    // --- looper -----------------------------------------------------------

    void pumpLooper(int timeoutMs) {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(timeoutMs, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(g_app, source);
            if (g_app->destroyRequested) {
                shouldClose_ = true;
                return;
            }
            // Only the first wait may block; afterwards drain what is ready
            // and return, or an idle app would never get back to its caller.
            timeoutMs = 0;
        }
    }

    static void onCmd(::android_app* app, int32_t cmd) {
        auto* self = static_cast<AndroidWindow*>(app->userData);
        if (!self) return;
        switch (cmd) {
            case APP_CMD_INIT_WINDOW: {
                if (!app->window) break;
                std::string err;
                if (!self->gl_.bind(app->window, &err)) {
                    FOLIO_LOG("EGL bind failed: %s", err.c_str());
                    break;
                }
                Event e;
                e.type = Event::Type::Resize;
                e.w = self->gl_.width();
                e.h = self->gl_.height();
                e.scale = self->scale_;
                self->queue_.push_back(std::move(e));
                break;
            }
            case APP_CMD_TERM_WINDOW:
                self->gl_.unbind();
                break;
            case APP_CMD_WINDOW_RESIZED:
            case APP_CMD_CONFIG_CHANGED: {
                if (!app->window) break;
                const int w = ANativeWindow_getWidth(app->window);
                const int h = ANativeWindow_getHeight(app->window);
                self->gl_.resized(w, h);
                Event e;
                e.type = Event::Type::Resize;
                e.w = w;
                e.h = h;
                e.scale = self->scale_;
                self->queue_.push_back(std::move(e));
                break;
            }
            case APP_CMD_GAINED_FOCUS: {
                Event e;
                e.type = Event::Type::FocusIn;
                self->queue_.push_back(std::move(e));
                break;
            }
            case APP_CMD_LOST_FOCUS: {
                // Every contact is void the moment focus goes: the finger
                // that was mid-pinch will never report an UP, and a gesture
                // tracker left holding it would misread the next touch.
                self->cancelAllPointers();
                Event e;
                e.type = Event::Type::FocusOut;
                self->queue_.push_back(std::move(e));
                break;
            }
            case APP_CMD_DESTROY:
                self->shouldClose_ = true;
                break;
            default:
                break;
        }
    }

    void cancelAllPointers() {
        Event e;
        e.type = Event::Type::PointerCancel;
        queue_.push_back(std::move(e));
    }

    // --- input ------------------------------------------------------------

    static PointerSource toolToSource(int32_t tool) {
        switch (tool) {
            case AMOTION_EVENT_TOOL_TYPE_STYLUS:
            case AMOTION_EVENT_TOOL_TYPE_ERASER:
                return PointerSource::Pen;
            case AMOTION_EVENT_TOOL_TYPE_MOUSE:
                return PointerSource::Mouse;
            default:
                return PointerSource::Touch;
        }
    }

    /// Android reports stylus orientation as a single tilt angle from
    /// vertical plus a rotation about the vertical axis. Folio, like the
    /// web and like Windows Ink, wants it decomposed onto two axes.
    static void decomposeTilt(float tilt, float orientation, float& outX,
                              float& outY) {
        const float t = std::tan(tilt);
        outX = static_cast<float>(std::atan(std::sin(orientation) * t) * 180.0 /
                                  kPi);
        outY = static_cast<float>(std::atan(std::cos(orientation) * t) * 180.0 /
                                  kPi);
    }

    void emitPointer(const AInputEvent* ev, std::size_t index,
                     Event::Type type) {
        const auto i = static_cast<std::size_t>(index);
        Event e;
        e.type = type;
        e.x = AMotionEvent_getX(ev, i);
        e.y = AMotionEvent_getY(ev, i);
        e.pointerId =
            static_cast<std::uint32_t>(AMotionEvent_getPointerId(ev, i));
        e.source = toolToSource(AMotionEvent_getToolType(ev, i));
        e.button = Button::Left;
        e.pressure = type == Event::Type::PointerUp
                         ? 0.f
                         : std::clamp(AMotionEvent_getPressure(ev, i), 0.f, 1.f);
        if (e.source == PointerSource::Pen) {
            decomposeTilt(
                AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_TILT, i),
                AMotionEvent_getOrientation(ev, i), e.tiltX, e.tiltY);
        }
        if (type == Event::Type::PointerDown) {
            const auto now = AMotionEvent_getEventTime(ev) / 1000000;  // ms
            const bool near = std::abs(e.x - lastDownX_) < 16.0 &&
                              std::abs(e.y - lastDownY_) < 16.0;
            e.clickCount = (near && now - lastDownTime_ < 400) ? 2 : 1;
            lastDownTime_ = now;
            lastDownX_ = e.x;
            lastDownY_ = e.y;
        }
        queue_.push_back(std::move(e));
    }

    int32_t handleMotion(const AInputEvent* ev) {
        const int32_t action = AMotionEvent_getAction(ev);
        const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
        const auto index = static_cast<std::size_t>(
            (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
            AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        const std::size_t count = AMotionEvent_getPointerCount(ev);

        switch (masked) {
            case AMOTION_EVENT_ACTION_DOWN:
            case AMOTION_EVENT_ACTION_POINTER_DOWN:
                emitPointer(ev, index, Event::Type::PointerDown);
                return 1;

            case AMOTION_EVENT_ACTION_UP:
            case AMOTION_EVENT_ACTION_POINTER_UP:
                emitPointer(ev, index, Event::Type::PointerUp);
                return 1;

            case AMOTION_EVENT_ACTION_MOVE: {
                // Historical samples first. Android batches everything that
                // arrived since the last frame into one event, and throwing
                // them away is what makes a fast stroke look polygonal -
                // the same reason the browser shell asks for coalesced
                // events.
                const std::size_t history = AMotionEvent_getHistorySize(ev);
                for (std::size_t h = 0; h < history; ++h) {
                    for (std::size_t p = 0; p < count; ++p) {
                        Event e;
                        e.type = Event::Type::PointerMove;
                        e.x = AMotionEvent_getHistoricalX(ev, p, h);
                        e.y = AMotionEvent_getHistoricalY(ev, p, h);
                        e.pointerId = static_cast<std::uint32_t>(
                            AMotionEvent_getPointerId(ev, p));
                        e.source =
                            toolToSource(AMotionEvent_getToolType(ev, p));
                        e.pressure = std::clamp(
                            AMotionEvent_getHistoricalPressure(ev, p, h), 0.f,
                            1.f);
                        queue_.push_back(std::move(e));
                    }
                }
                for (std::size_t p = 0; p < count; ++p)
                    emitPointer(ev, p, Event::Type::PointerMove);
                return 1;
            }

            case AMOTION_EVENT_ACTION_CANCEL:
                cancelAllPointers();
                return 1;

            case AMOTION_EVENT_ACTION_SCROLL: {
                Event e;
                e.type = Event::Type::Wheel;
                e.x = AMotionEvent_getX(ev, 0);
                e.y = AMotionEvent_getY(ev, 0);
                e.wheelX =
                    AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HSCROLL, 0);
                e.wheelY =
                    AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_VSCROLL, 0);
                queue_.push_back(std::move(e));
                return 1;
            }

            default:
                return 0;
        }
    }

    int32_t handleKey(const AInputEvent* ev) {
        const int32_t code = AKeyEvent_getKeyCode(ev);
        const int32_t action = AKeyEvent_getAction(ev);
        // Back is the one key a reader must honour: it is how Android users
        // dismiss a sheet or leave the app, and swallowing it is a review
        // one-star.
        if (code == AKEYCODE_BACK) {
            if (action == AKEY_EVENT_ACTION_UP) {
                Event e;
                e.type = Event::Type::KeyDown;
                e.key = Key::Escape;
                queue_.push_back(std::move(e));
            }
            return 1;
        }
        return 0;
    }

    static int32_t onInput(::android_app* app, AInputEvent* ev) {
        auto* self = static_cast<AndroidWindow*>(app->userData);
        if (!self) return 0;
        switch (AInputEvent_getType(ev)) {
            case AINPUT_EVENT_TYPE_MOTION:
                return self->handleMotion(ev);
            case AINPUT_EVENT_TYPE_KEY:
                return self->handleKey(ev);
            default:
                return 0;
        }
    }

    AndroidGlContext gl_;
    std::deque<Event> queue_;
    float scale_ = 1.f;
    bool shouldClose_ = false;
    std::int64_t lastDownTime_ = 0;
    double lastDownX_ = 0.0, lastDownY_ = 0.0;
};

AndroidWindow* AndroidWindow::instance = nullptr;

// ---------------------------------------------------------------------------
// Clipboard and file dialogs
//
// Both are JNI calls into the framework, and both are deliberately left as
// honest no-ops rather than half-written bridges:
//
//  * The clipboard is android.content.ClipboardManager, reachable only from
//    a thread attached to the JVM.
//  * There is no file *path* dialog on modern Android at all. Scoped
//    storage replaced it with the Storage Access Framework, which returns a
//    content:// URI and a file descriptor. That does not fit IFileDialog's
//    "give me a path" shape, and pretending otherwise by returning a path
//    that cannot be opened would be worse than returning nothing.
//
// ANDROID-READINESS.md records what each needs.
// ---------------------------------------------------------------------------

class NullClipboard final : public IClipboard {
public:
    bool setText(std::string_view) override { return false; }
    [[nodiscard]] std::string text() override { return {}; }
};

class NullFileDialog final : public IFileDialog {
public:
    [[nodiscard]] std::optional<std::string> openFile(
        const FileDialogRequest&) override {
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string> saveFile(
        const FileDialogRequest&) override {
        return std::nullopt;
    }
    /// False, so FolioApp falls back to its own in-app OpenDialog. That
    /// component browsing a sandboxed app directory is a real, usable
    /// Android experience; a native picker that returns nothing is not.
    [[nodiscard]] bool available() const override { return false; }
};

}  // namespace

std::unique_ptr<IWindow> createWindow(const WindowDesc& desc,
                                      std::string* error) {
    auto win = std::make_unique<AndroidWindow>();
    if (!win->create(desc, error)) return nullptr;
    return win;
}

IClipboard& clipboard() {
    static NullClipboard c;
    return c;
}

IFileDialog& fileDialog() {
    static NullFileDialog d;
    return d;
}

const char* backendName() noexcept { return "android"; }

}  // namespace folio::platform
