#include "screens/AppState.h"

#include <algorithm>

namespace folio::ui {

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Annotate: return "Annotate";
        case Mode::Edit:     return "Edit";
        case Mode::Read:     break;
    }
    return "Read";
}

const char* modeIcon(Mode m) {
    switch (m) {
        case Mode::Annotate: return "highlighter";
        case Mode::Edit:     return "pen";
        case Mode::Read:     break;
    }
    return "eye";
}

const char* toolName(Tool t) {
    switch (t) {
        case Tool::Hand:      return "Pan";
        case Tool::Highlight: return "Highlight";
        case Tool::Pen:       return "Draw";
        case Tool::Eraser:    return "Erase";
        case Tool::Text:      return "Text";
        case Tool::Note:      return "Note";
        case Tool::Stamp:     return "Stamp";
        case Tool::Signature: return "Sign";
        case Tool::Shapes:    return "Shapes";
        case Tool::Select:    break;
    }
    return "Select";
}

const char* toolIcon(Tool t) {
    switch (t) {
        case Tool::Hand:      return "hand";
        case Tool::Highlight: return "highlighter";
        case Tool::Pen:       return "pen";
        case Tool::Eraser:    return "eraser";
        case Tool::Text:      return "text";
        case Tool::Note:      return "note";
        case Tool::Stamp:     return "stamp";
        case Tool::Signature: return "signature";
        case Tool::Shapes:    return "shapes";
        case Tool::Select:    break;
    }
    return "cursor";
}

Breakpoint breakpointFor(float widthPx) noexcept {
    if (widthPx < 720.f) return Breakpoint::Compact;
    if (widthPx < 1120.f) return Breakpoint::Medium;
    return Breakpoint::Expanded;
}

const char* breakpointName(Breakpoint b) noexcept {
    switch (b) {
        case Breakpoint::Compact:  return "compact";
        case Breakpoint::Medium:   return "medium";
        case Breakpoint::Expanded: return "expanded";
    }
    return "expanded";
}

LayoutMetrics LayoutMetrics::forWidth(float w) noexcept {
    LayoutMetrics m;
    m.bp = breakpointFor(w);
    switch (m.bp) {
        case Breakpoint::Compact:
            m.gutter = 10.f;
            m.panelGap = 8.f;
            // A drawer, not a column: as wide as it needs to be, capped so
            // a sliver of the document stays visible behind it.
            m.leftPanelW = std::min(300.f, std::max(240.f, w - 72.f));
            m.rightPanelW = m.leftPanelW;
            m.panelsOverlay = true;
            m.showTabs = false;
            m.compactChrome = true;
            m.typeScale = 0.88f;
            break;
        case Breakpoint::Medium:
            m.gutter = 16.f;
            m.panelGap = 12.f;
            m.leftPanelW = 224.f;
            m.rightPanelW = 232.f;
            m.compactChrome = true;
            m.typeScale = 0.95f;
            break;
        case Breakpoint::Expanded:
            break;
    }
    return m;
}

std::vector<Tool> toolsFor(Mode m) {
    switch (m) {
        case Mode::Annotate:
            return {Tool::Select, Tool::Highlight, Tool::Pen, Tool::Note,
                    Tool::Eraser};
        case Mode::Edit:
            return {Tool::Select, Tool::Text, Tool::Shapes, Tool::Stamp,
                    Tool::Signature};
        case Mode::Read:
            break;
    }
    return {Tool::Select, Tool::Hand};
}

void AppState::seedDemoContent() {
    // Verbatim from the prototype's REC array - same names, same order,
    // same 24-page documents.
    recents = {
        {"Annual_Report_2026.pdf", "Today \xC2\xB7 24 pages \xC2\xB7 page 3", 0.12f, 0, 24},
        {"Contract_v3.pdf",        "Yesterday \xC2\xB7 24 pages",              0.00f, 1, 24},
        {"Invoice_0192.pdf",       "Mon \xC2\xB7 24 pages",                    0.00f, 2, 24},
        {"Thesis_draft.pdf",       "Oct 2 \xC2\xB7 24 pages \xC2\xB7 page 12",0.00f, 0, 24},
        {"Lease_agreement.pdf",    "Sep 28 \xC2\xB7 24 pages",                 0.00f, 1, 24},
        {"Manual_v2.pdf",          "Sep 21 \xC2\xB7 24 pages",                 0.00f, 2, 24},
        {"Budget_2027.pdf",        "Sep 18 \xC2\xB7 24 pages",                 0.00f, 0, 24},
        {"Meeting_notes.pdf",      "Sep 12 \xC2\xB7 24 pages",                 0.00f, 1, 24},
        {"Onboarding_guide.pdf",   "Sep 2 \xC2\xB7 24 pages",                  0.00f, 2, 24},
    };
}

}  // namespace folio::ui
