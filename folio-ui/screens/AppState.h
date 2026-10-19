// folio-ui/screens/AppState.h
//
// Application state, separate from any widget.
//
// Keeping this out of the widget tree is what makes the UI testable and what
// will let the real document model (folio-core's DocumentSnapshot) drop in
// behind it. Widgets read it and raise intents; they never own truth.

#pragma once

#include "folio/ui/Theme.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace folio::ui {

/// Layout size class, derived from the window width.
///
/// Folio is one C++ UI on six targets, so "the phone build" cannot be a
/// different programme - it is the same widget tree laid out against a
/// narrower frame. Everything that differs between a phone, a tablet and a
/// desktop window is a consequence of this enum, read during layout.
///
/// The thresholds are window widths in logical pixels, not device classes:
/// a desktop window dragged narrow gets the tablet layout, which is the
/// behaviour people expect and also the only way this stays testable
/// without a phone.
enum class Breakpoint : std::uint8_t {
    Compact,   ///< < 720  - phones, and very narrow desktop windows
    Medium,    ///< < 1120 - tablets, split-screen, small laptops
    Expanded,  ///< the design the prototype was drawn at
};

[[nodiscard]] Breakpoint breakpointFor(float widthPx) noexcept;
[[nodiscard]] const char* breakpointName(Breakpoint b) noexcept;

/// Geometry that varies with the size class.
///
/// Gathered in one struct so a screen reads its metrics once at the top of
/// onLayout instead of testing the breakpoint at thirty call sites - that
/// is how layouts drift into disagreeing with themselves.
struct LayoutMetrics {
    Breakpoint bp = Breakpoint::Expanded;
    float gutter = 24.f;        ///< window edge to content
    float panelGap = 16.f;      ///< between panels and canvas
    float leftPanelW = 268.f;
    float rightPanelW = 272.f;
    /// Panels float over the canvas instead of displacing it. Below Medium
    /// there is not enough width for three columns, so they become drawers.
    bool panelsOverlay = false;
    bool showTabs = true;       ///< the tab strip needs real width
    bool compactChrome = false; ///< icon-only buttons, no labels
    float typeScale = 1.f;      ///< headings shrink on small frames

    [[nodiscard]] static LayoutMetrics forWidth(float w) noexcept;
    [[nodiscard]] static LayoutMetrics forViewport(float w, float h) noexcept;
};

/// The editing mode the prototype's mode button cycles through.
enum class Mode : std::uint8_t { Read, Annotate, Edit };

/// Active tool within Annotate/Edit.
// Annotations belong to the open document, so the store lives in AppState
// alongside it rather than inside a view.
enum class Tool : std::uint8_t {
    Select, Hand, Highlight, Pen, Eraser, Text, Note, Stamp, Signature, Shapes,
};

[[nodiscard]] const char* modeName(Mode m);
[[nodiscard]] const char* modeIcon(Mode m);
[[nodiscard]] const char* toolName(Tool t);
[[nodiscard]] const char* toolIcon(Tool t);
/// Tools offered in each mode, in toolbar order.
[[nodiscard]] std::vector<Tool> toolsFor(Mode m);

struct RecentDoc {
    std::string path;  ///< actual opened PDF, never a synthetic demo entry
    std::string title;
    std::string meta;
    float progress = 0.f;
    int accent = 0;
    int pages = 12;
    bool linked = false;  ///< private stable locator; original opened through fresh SAF FD
    bool unavailable = false;
};

struct OpenDoc {
    std::string title;
    bool dirty = false;
    int pages = 12;
};

/// Which top-level screen is showing.
enum class Screen : std::uint8_t { Welcome, Reader };

struct AppState {
    Theme theme;
    Screen screen = Screen::Welcome;

    Mode mode = Mode::Read;
    Tool tool = Tool::Select;
    int inkColour = 0;        // index into tokens::inkPalette
    int highlightColour = 0;  // index into tokens::highlighter::kAll

    std::vector<RecentDoc> recents;
    std::vector<OpenDoc> openDocs;

    /// Which of the left panel's four tabs is showing: Pages, Outline,
    /// Notes, Layers - the prototype's .seg.tabs4.
    int leftTab = 0;
    bool leftPanelCollapsed = false;
    bool rightPanelCollapsed = true;  ///< inspector is opt-in, not shown on open
    bool focusMode = false;
    /// Reading preference shared by all PDFs: reveal focus controls on tap,
    /// then remove them after three seconds without activity. Can be turned
    /// off in the inspector; retained with the recent-document state file.
    bool autoHideFocusControls = true;
    // A phone finger covers the glyph it is choosing. Desktop/mouse stays
    // unchanged unless the user explicitly enables this touch-only option.
#if defined(__ANDROID__)
    bool selectionMagnifier = true;
#else
    bool selectionMagnifier = false;
#endif

    /// A small, bounded list of real files. The shell supplies a per-user
    /// state file (Android's internal files directory on mobile).
    void loadRecents(std::string file);
    void rememberDocument(const std::string& path, int pages, int page = 1,
                          bool deferSave = false);
    void forgetDocument(const std::string& path);
    void saveRecents() const;
    void flushPendingRecents();
    ~AppState() { flushPendingRecents(); }
    std::string recentStateFile;
private:
    bool recentsPending_ = false;
};

}  // namespace folio::ui
