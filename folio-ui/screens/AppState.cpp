#include "screens/AppState.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

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

LayoutMetrics LayoutMetrics::forViewport(float w, float h) noexcept {
    // A rotated phone has a medium *width* but still only a phone's height.
    // Do not turn its chrome into three desktop columns on rotation. A
    // landscape tablet retains its normal width breakpoint.
    if (h < 560.f && w < 1120.f)
        return forWidth(std::min(w, 600.f));
    return forWidth(w);
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

namespace {
constexpr std::size_t kMaxRecentDocs = 20;
std::filesystem::path pathFromUtf8(const std::string& path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}
RecentDoc recentFromPath(const std::string& path, int pages, int page) {
    RecentDoc r;
    r.path = path;
    std::error_code storageEc;
    r.linked = std::filesystem::is_regular_file(pathFromUtf8(path + ".folio-link"), storageEc);
    r.unavailable = false; // provider status learned by an attempted FD open
    const auto filename = pathFromUtf8(path).filename().u8string();
    r.title.assign(filename.begin(), filename.end());
    r.pages = std::max(1, pages);
    const int current = std::clamp(page, 1, r.pages);
    r.progress = static_cast<float>(current - 1) / static_cast<float>(r.pages);
    r.meta = std::to_string(r.pages) + " pages  ·  page " + std::to_string(current) +
             (r.linked ? " · Source only" : " · Offline copy");
    return r;
}
}

void AppState::loadRecents(std::string file) {
    recentStateFile = std::move(file);
    recents.clear();
    autoHideFocusControls = true; // old V1 files have no preference row
#if defined(__ANDROID__)
    selectionMagnifier = true;  // legacy state, mobile default ON
#else
    selectionMagnifier = false;
#endif
    std::ifstream in(pathFromUtf8(recentStateFile), std::ios::binary);
    std::string header;
    if (!std::getline(in, header) || header != "FOLIO_RECENTS_V1") return;
    std::string line;
    while (std::getline(in, line)) {
        if (line == "@focusAutoHide 0" || line == "@focusAutoHide 1") {
            autoHideFocusControls = line.back() == '1';
            continue;
        }
        if (line == "@selectionMagnifier 0" || line == "@selectionMagnifier 1") {
            selectionMagnifier = line.back() == '1';
            continue;
        }
        if (recents.size() >= kMaxRecentDocs || line.size() > 8192) continue;
        // The preference row is deliberately unquoted, so it can never be
        // mistaken for a legitimate path even in a legacy V1 file.
        std::istringstream row(line);
        std::string path;
        int pages = 0, page = 0;
        if (!(row >> std::quoted(path) >> pages >> page) || path.empty() ||
            pages < 1 || pages > 1000000 || page < 1) continue;
        try {
            std::error_code ec;
            if (std::filesystem::is_regular_file(pathFromUtf8(path), ec) ||
                std::filesystem::is_regular_file(pathFromUtf8(path + ".folio-link"), ec))
                recents.push_back(recentFromPath(path, pages, page));
        } catch (const std::filesystem::filesystem_error&) {
            // A corrupt or invalidly encoded path must not prevent startup.
        }
    }
}

void AppState::rememberDocument(const std::string& path, int pages, int page) {
    if (path.empty()) return;
    recents.erase(std::remove_if(recents.begin(), recents.end(),
                                [&](const RecentDoc& r) { return r.path == path; }),
                  recents.end());
    recents.insert(recents.begin(), recentFromPath(path, pages, page));
    if (recents.size() > kMaxRecentDocs) recents.resize(kMaxRecentDocs);
    saveRecents();
}

void AppState::forgetDocument(const std::string& path) {
    recents.erase(std::remove_if(recents.begin(), recents.end(),
                                [&](const RecentDoc& r) { return r.path == path; }),
                  recents.end());
    saveRecents();
}

void AppState::saveRecents() const {
    if (recentStateFile.empty()) return;
    const std::filesystem::path dst = pathFromUtf8(recentStateFile);
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) return;
    std::filesystem::path tmp = dst;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << "FOLIO_RECENTS_V1\n";
        out << "@focusAutoHide " << (autoHideFocusControls ? 1 : 0) << '\n';
        out << "@selectionMagnifier " << (selectionMagnifier ? 1 : 0) << '\n';
        for (const RecentDoc& r : recents) {
            const int page = std::clamp(static_cast<int>(
                std::lround(r.progress * static_cast<float>(r.pages))) + 1,
                1, r.pages);
            out << std::quoted(r.path) << ' ' << r.pages << ' ' << page << '\n';
        }
        out.flush();
        if (!out) return;
    }
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {
        // Windows rename refuses an existing destination. Keep the temp file
        // until an overwrite copy succeeds rather than deleting valid state.
        ec.clear();
        std::filesystem::copy_file(tmp, dst,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) std::filesystem::remove(tmp, ec);
    }
}

}  // namespace folio::ui
