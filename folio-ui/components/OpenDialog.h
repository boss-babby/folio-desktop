// folio-ui/components/OpenDialog.h
//
// "Open a PDF" - a modal file chooser.
//
// WHY THERE IS AN IN-APP PICKER AT ALL
// ------------------------------------
// The obvious answer to "open a file" is the host's own dialog:
// IFileOpenDialog on Windows, NSOpenPanel on macOS, the XDG file-chooser
// portal on Linux. Those are the right default - they know about permissions,
// recent places, cloud providers and the user's bookmarks, and no in-app
// imitation will match them.
//
// But two of the six targets have no such dialog to call. WebAssembly can
// only prompt through a hidden <input type=file>, which hands back a byte
// blob and no path; iOS has UIDocumentPickerViewController, which is a
// view controller rather than a blocking call and has no desktop analogue.
// A single code path that works everywhere therefore cannot be "call the
// native dialog" - it has to be an interface with a native implementation
// where one exists and this widget where one does not.
//
// FilePicker (below) is that interface. OpenDialog is the fallback, and also
// what runs in the streamed demo, which has no desktop session at all.

#pragma once

#include "folio/ui/Widget.h"
#include "components/Button.h"
#include "components/Containers.h"
#include "components/Controls.h"

#include <functional>
#include <string>
#include <vector>

namespace folio::ui {

/// One selectable entry.
struct FileEntry {
    std::string name;      ///< "Annual Report 2026.pdf"
    std::string path;      ///< absolute, or a scheme the shell understands
    std::string meta;      ///< "2.4 MB  ·  24 pages" - display only
    bool isDirectory = false;
};

/// How the application asks for a file. A desktop shell installs a native
/// implementation; everything else falls back to OpenDialog.
class FilePicker {
public:
    virtual ~FilePicker() = default;

    /// True when this picker can show the host's own dialog. When false the
    /// caller shows the in-app OpenDialog instead.
    [[nodiscard]] virtual bool isNative() const = 0;

    /// Begin a request. `done` is called with the chosen path, or an empty
    /// string if the user cancelled. It may be called asynchronously - on
    /// iOS and the web it always is - so callers must not assume it has run
    /// by the time this returns.
    virtual void requestOpen(std::function<void(std::string)> done) = 0;

    /// Native Save As when available. Defaults to cancellation for existing
    /// non-native pickers; callers fall back to the in-app dialog instead.
    virtual void requestSave(const std::string& suggestedName,
                             std::function<void(std::string)> done) {
        (void)suggestedName;
        if (done) done({});
    }

    /// Entries to show when falling back to the in-app dialog.
    [[nodiscard]] virtual std::vector<FileEntry> browse(const std::string& dir) = 0;

    /// Where the in-app dialog starts.
    [[nodiscard]] virtual std::string defaultDirectory() const = 0;

    /// The process-wide picker. Never null: without one installed this
    /// returns a stub whose browse() lists the bundled sample documents.
    static FilePicker& current();
    static void setCurrent(FilePicker* p);

    /// The built-in filesystem picker.
    ///
    /// A native picker that wants ordinary directory listings for the
    /// fallback dialog must delegate to THIS, never to `current()` -
    /// `current()` is very likely itself, and the recursion is an immediate
    /// stack overflow. (It was: the desktop shell segfaulted the first time
    /// anyone pressed "Open file".)
    static FilePicker& local();
};

/// Centred modal card with a scrim. Added to the overlay layer, so it paints
/// above both screens and swallows pointer events while open.
class OpenDialog : public Widget {
public:
    OpenDialog();

    /// Called with the chosen path. Not called on cancel.
    std::function<void(std::string)> onOpen;
    /// Called with the full destination path in Save mode.
    std::function<void(std::string)> onSave;

    /// Save mode adds a filename field and turns the browser into a
    /// destination picker. Same list, same navigation - only the question
    /// at the bottom changes, which is also how every native dialog does it.
    enum class Mode { Open, Save };

    void open(double now);
    void openSave(std::string suggestedName, double now);
    void close(double now);
    [[nodiscard]] bool isOpen() const noexcept { return open_; }

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    void onPaintOverlay(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;
    void onLayout() override;

private:
    void navigate(const std::string& dir, double now);
    void confirm();
    [[nodiscard]] Rect cardRect() const;

    bool open_ = false;
    AnimatedF fade_{0.f};
    std::string dir_;
    std::vector<FileEntry> entries_;
    int selected_ = -1;
    int hover_ = -1;
    float scroll_ = 0.f;
    PointerId touchId_ = 0;
    float touchStartY_ = 0.f;
    float touchLastY_ = 0.f;
    bool touchHeld_ = false;
    bool touchDragged_ = false;
    double now_ = 0.0;

    Button* cancelBtn_ = nullptr;
    Button* openBtn_ = nullptr;
    Button* upBtn_ = nullptr;
    Button* nativeBtn_ = nullptr;
    Mode mode_ = Mode::Open;
    TextField* nameField_ = nullptr;
};

}  // namespace folio::ui
