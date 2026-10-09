// folio-ui/components/OpenDialog.cpp

#include "components/OpenDialog.h"
#include "folio/ui/Draw.h"
#include "folio/ui/Icons.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include "folio/ui/Tokens.h"

#include <algorithm>
#include <cmath>

namespace folio::ui {

using folio::render::TextAlign;

// ---------------------------------------------------------------------------
// FilePicker
// ---------------------------------------------------------------------------

namespace {

/// Lists real .pdf files from the filesystem. Used when no host dialog has
/// been installed - the streamed demo, and any platform whose native picker
/// is not wired up yet.
///
/// This is a real directory listing, not a mock: entries come from
/// std::filesystem and the paths they carry are paths PDFium can open.
class LocalPicker final : public FilePicker {
public:
    [[nodiscard]] bool isNative() const override { return false; }

    void requestOpen(std::function<void(std::string)> done) override {
        if (done) done({});  // no host dialog here; the caller shows OpenDialog
    }

    [[nodiscard]] std::string defaultDirectory() const override {
        namespace fs = std::filesystem;
        // Prefer the bundled documents folder, then the user's own.
        for (const char* c : {"documents", "./documents"}) {
            std::error_code ec;
            if (fs::is_directory(c, ec)) return fs::absolute(c, ec).string();
        }
        if (const char* home = std::getenv("HOME")) {
            std::error_code ec;
            const fs::path docs = fs::path(home) / "Documents";
            if (fs::is_directory(docs, ec)) return docs.string();
            return home;
        }
        return ".";
    }

    [[nodiscard]] std::vector<FileEntry> browse(const std::string& dir) override {
        namespace fs = std::filesystem;
        std::vector<FileEntry> out;
        std::error_code ec;

        const fs::path base(dir);
        if (base.has_parent_path() && base.parent_path() != base) {
            // ".." is filesystem notation, not a word. Name the folder the
            // row actually goes to.
            const fs::path up = base.parent_path();
            std::string upName = up.filename().string();
            if (upName.empty()) upName = up.string();   // at the root
            out.push_back({upName, up.string(), "Parent folder", true});
        }

        std::vector<fs::directory_entry> dirs, files;
        for (const auto& e : fs::directory_iterator(base, ec)) {
            const std::string name = e.path().filename().string();
            if (!name.empty() && name[0] == '.') continue;
            std::error_code e2;
            if (e.is_directory(e2)) {
                dirs.push_back(e);
            } else {
                std::string ext = e.path().extension().string();
                for (char& c : ext) c = static_cast<char>(std::tolower(c));
                if (ext == ".pdf") files.push_back(e);
            }
        }
        const auto byName = [](const fs::directory_entry& a,
                               const fs::directory_entry& b) {
            return a.path().filename().string() < b.path().filename().string();
        };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(files.begin(), files.end(), byName);

        for (const auto& d : dirs)
            out.push_back({d.path().filename().string(), d.path().string(), "Folder",
                           true});
        for (const auto& f : files) {
            std::error_code e2;
            const auto sz = fs::file_size(f.path(), e2);
            char meta[64];
            if (e2) {
                std::snprintf(meta, sizeof meta, "PDF");
            } else if (sz >= 1024u * 1024u) {
                std::snprintf(meta, sizeof meta, "PDF  \u00b7  %.1f MB",
                              static_cast<double>(sz) / (1024.0 * 1024.0));
            } else if (sz >= 1024u) {
                std::snprintf(meta, sizeof meta, "PDF  \u00b7  %.0f KB",
                              static_cast<double>(sz) / 1024.0);
            } else {
                // Anything under a kilobyte rounds to "0 KB", which looks
                // like a broken file rather than a small one.
                std::snprintf(meta, sizeof meta, "PDF  \u00b7  %llu bytes",
                              static_cast<unsigned long long>(sz));
            }
            out.push_back({f.path().filename().string(), f.path().string(), meta,
                           false});
        }
        return out;
    }
};

LocalPicker g_samplePicker;
FilePicker* g_picker = nullptr;

constexpr float kCardW = 560.f;
constexpr float kCardH = 440.f;
constexpr float kRowH = 46.f;
constexpr float kListTop = 104.f;
constexpr float kFootH = 64.f;
// Save mode needs one more row for the filename field.
constexpr float kNameH = 46.f;

}  // namespace

FilePicker& FilePicker::current() {
    return g_picker ? *g_picker : g_samplePicker;
}

void FilePicker::setCurrent(FilePicker* p) { g_picker = p; }

FilePicker& FilePicker::local() { return g_samplePicker; }

// ---------------------------------------------------------------------------
// OpenDialog
// ---------------------------------------------------------------------------

OpenDialog::OpenDialog() {
    acceptsPointer = true;
    visible = false;
    debugName = "OpenDialog";

    cancelBtn_ = emplace<Button>("Cancel", "", ButtonStyle::Ghost);
    cancelBtn_->onClick = [this] { close(now_); };

    openBtn_ = emplace<Button>("Open", "", ButtonStyle::Primary);
    openBtn_->onClick = [this] { confirm(); };

    upBtn_ = emplace<Button>("", "chevron-left", ButtonStyle::Icon);
    upBtn_->tooltip = "Back";
    upBtn_->visible = false;

    // Offered only when the host actually has a dialog to show; on the web
    // and in the streamed demo there is nothing behind this button, so it
    // would be a dead control.
    nativeBtn_ = emplace<Button>("Browse\u2026", "folder-open", ButtonStyle::Ghost);
    nativeBtn_->tooltip = "Use the system file dialog";
    nativeBtn_->onClick = [this] {
        FilePicker::current().requestOpen([this](std::string path) {
            if (path.empty()) return;
            close(now_);
            if (onOpen) onOpen(std::move(path));
        });
    };
}

Rect OpenDialog::cardRect() const {
    return Rect{(frame.w - kCardW) * 0.5f, (frame.h - kCardH) * 0.5f - 10.f, kCardW,
                kCardH};
}

void OpenDialog::open(double now) {
    now_ = now;
    open_ = true;
    visible = true;
    selected_ = -1;
    scroll_ = 0.f;
    mode_ = Mode::Open;
    openBtn_->label = "Open";
    if (nameField_) nameField_->visible = false;
    navigate(FilePicker::current().defaultDirectory(), now);
    nativeBtn_->visible = FilePicker::current().isNative();
    fade_.reset(0.f);
    fade_.animateTo(1.f, tokens::dur::paneSwitch, now);
}

void OpenDialog::close(double now) {
    if (!open_) return;
    open_ = false;
    fade_.animateTo(0.f, tokens::dur::paneSwitch, now);
}

void OpenDialog::navigate(const std::string& dir, double now) {
    dir_ = dir;
    entries_ = FilePicker::current().browse(dir);
    selected_ = -1;
    scroll_ = 0.f;
    (void)now;
}

void OpenDialog::confirm() {
    if (mode_ == Mode::Save) {
        // A directory selection navigates; anything else commits the field.
        if (selected_ >= 0 && selected_ < static_cast<int>(entries_.size())) {
            const FileEntry e = entries_[static_cast<std::size_t>(selected_)];
            if (e.isDirectory) { navigate(e.path, now_); return; }
        }
        std::string name = nameField_ ? nameField_->text : std::string{};
        while (!name.empty() && (name.front() == ' ')) name.erase(name.begin());
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (name.empty()) return;          // nothing to save to
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".pdf") != 0)
            name += ".pdf";
        std::string dir = dir_;
        if (!dir.empty() && dir.back() == '/') dir.pop_back();
        close(now_);
        if (onSave) onSave(dir + "/" + name);
        return;
    }

    if (selected_ < 0 || selected_ >= static_cast<int>(entries_.size())) return;
    const FileEntry e = entries_[static_cast<std::size_t>(selected_)];
    if (e.isDirectory) {
        navigate(e.path, now_);
        return;
    }
    close(now_);
    if (onOpen) onOpen(e.path);
}

void OpenDialog::openSave(std::string suggestedName, double now) {
    if (!nameField_) {
        nameField_ = emplace<TextField>("File name");
        nameField_->onSubmit = [this] { confirm(); };
    }
    // open() deliberately resets to Open mode, so everything specific to
    // Save has to be applied after it, not before.
    open(now);
    mode_ = Mode::Save;
    openBtn_->label = "Save";
    nameField_->visible = true;
    nameField_->text = std::move(suggestedName);
    layoutTree();
}

void OpenDialog::onLayout() {
    const Rect c = cardRect();
    const float by = c.y + c.h - kFootH + 16.f;
    if (nameField_ && nameField_->visible) {
        nameField_->setFrame(c.x + 16.f, by - kNameH, c.w - 32.f, 34.f);
    }
    openBtn_->setFrame(c.x + c.w - 16.f - 92.f, by, 92.f, 32.f);
    cancelBtn_->setFrame(c.x + c.w - 16.f - 92.f - 8.f - 82.f, by, 82.f, 32.f);
    nativeBtn_->setFrame(c.x + 16.f, by, 118.f, 32.f);
    upBtn_->setFrame(c.x + 14.f, c.y + 14.f, 28.f, 28.f);
}

bool OpenDialog::animating(double now) const { return fade_.active(now); }

bool OpenDialog::onKey(const KeyEvent& e) {
    if (!open_) return false;
    if (e.key == Key::Escape) { close(now_); return true; }
    if (e.key == Key::Enter) { confirm(); return true; }
    // Save mode has a text field, and this handler is modal - without an
    // explicit hand-off it swallows every keystroke and the field can never
    // be edited. Arrows still belong to the list.
    if (mode_ == Mode::Save && nameField_ && nameField_->visible &&
        nameField_->focused && e.key != Key::Up && e.key != Key::Down) {
        nameField_->onKey(e);
        return true;
    }
    if (e.key == Key::Down) {
        selected_ = std::min(selected_ + 1, static_cast<int>(entries_.size()) - 1);
        return true;
    }
    if (e.key == Key::Up) {
        selected_ = std::max(selected_ - 1, 0);
        return true;
    }
    return true;  // modal: swallow everything
}

bool OpenDialog::onPointer(const PointerEvent& e) {
    if (!open_) return false;
    const Rect c = cardRect();
    const Rect list{c.x + 12.f, c.y + kListTop, c.w - 24.f,
                    c.h - kListTop - kFootH - 8.f -
                        (nameField_ && nameField_->visible ? kNameH : 0.f)};

    if (e.phase == PointerPhase::Wheel) {
        const float maxS =
            std::max(0.f, static_cast<float>(entries_.size()) * kRowH - list.h);
        scroll_ = std::clamp(scroll_ - e.wheelY, 0.f, maxS);
        return true;
    }

    const bool inList = list.contains(e.pos.x, e.pos.y);
    hover_ = -1;
    if (inList) {
        const int idx =
            static_cast<int>(std::floor((e.pos.y - list.y + scroll_) / kRowH));
        if (idx >= 0 && idx < static_cast<int>(entries_.size())) hover_ = idx;
    }

    if (e.phase == PointerPhase::Up) {
        if (hover_ >= 0) {
            // A second click on an already-selected row opens it, which is
            // what double-click would do if the event stream carried one.
            if (selected_ == hover_) { confirm(); return true; }
            selected_ = hover_;
            return true;
        }
        // Clicking the scrim dismisses, which is the convention everywhere.
        if (!c.contains(e.pos.x, e.pos.y)) { close(now_); return true; }
    }
    return true;  // modal: nothing behind this gets pointer events
}

void OpenDialog::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    auto* self = const_cast<OpenDialog*>(this);
    self->now_ = ctx.now;
    const float a = fade_.value(ctx.now);
    if (a <= 0.004f) {
        if (!open_) self->visible = false;
        return;
    }

    // Scrim.
    t.fillRect(localBounds(), Color{0, 0, 0, static_cast<std::uint8_t>(96.f * a)});

    // Card, rising slightly as it fades in.
    const Rect c = cardRect();
    const Rect cr{c.x, c.y + (1.f - a) * 10.f, c.w, c.h};
    const Corners rr = Corners::all(tokens::radius::card);
    drawElevation(t, cr, rr, tokens::elevation::popover(ctx.pal), a,
                  /*interiorOpaque=*/true);
    t.fillRoundedRect(cr, rr, fade(ctx.pal.card, a));
    t.strokeRoundedRect(cr, rr, fade(ctx.pal.border, a * 0.8f), 1.f);
}

void OpenDialog::onPaintOverlay(IPaintTarget& t, const PaintCtx& ctx) {
    const float a = fade_.value(ctx.now);
    if (a <= 0.004f) return;

    const Rect c = cardRect();
    const Rect cr{c.x, c.y + (1.f - a) * 10.f, c.w, c.h};

    t.drawTextInBox(mode_ == Mode::Save ? "Extract to" : "Open a PDF",
                    Rect{cr.x + 20.f, cr.y + 16.f, cr.w - 40.f, 26.f},
                    styleFor(tokens::type_cardTitle, fade(ctx.pal.text, a)),
                    TextAlign::Left);
    // A full absolute path is the least interesting thing on this card and
    // the widest. Show where you are and what is here.
    {
        std::string where = std::filesystem::path(dir_).filename().string();
        if (where.empty()) where = dir_;
        int pdfs = 0;
        for (const FileEntry& e : entries_) if (!e.isDirectory) ++pdfs;
        std::string sub = where;
        sub += "  \u00b7  ";
        sub += (pdfs == 0)   ? "no PDFs here"
             : (pdfs == 1)   ? "1 PDF"
                             : (std::to_string(pdfs) + " PDFs");
        t.drawTextInBox(sub, Rect{cr.x + 20.f, cr.y + 44.f, cr.w - 40.f, 18.f},
                        styleFor(tokens::type_small, fade(ctx.pal.muted, a)),
                        TextAlign::Left);
    }
    t.drawLine(Point{cr.x, cr.y + kListTop - 10.f},
               Point{cr.x + cr.w, cr.y + kListTop - 10.f}, fade(ctx.pal.border, a),
               1.f);

    const Rect list{cr.x + 12.f, cr.y + kListTop, cr.w - 24.f,
                    cr.h - kListTop - kFootH - 8.f -
                        (nameField_ && nameField_->visible ? kNameH : 0.f)};
    t.save();
    t.clipRect(list, true);
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const float ry = list.y + static_cast<float>(i) * kRowH - scroll_;
        if (ry > list.bottom() || ry + kRowH < list.y) continue;
        const Rect row{list.x, ry, list.w, kRowH - 2.f};
        const bool sel = static_cast<int>(i) == selected_;
        const bool hov = static_cast<int>(i) == hover_;
        if (sel) {
            t.fillRoundedRect(row, Corners::all(tokens::radius::control),
                              fade(ctx.pal.accentSoft, a));
        } else if (hov) {
            t.fillRoundedRect(row, Corners::all(tokens::radius::control),
                              fade(ctx.pal.hover, a));
        }
        const FileEntry& e = entries_[i];
        drawIcon(t, e.isDirectory ? "folder" : "file-text",
                 Rect{row.x + 10.f, row.y + (row.h - 18.f) * 0.5f, 18.f, 18.f},
                 fade(sel ? ctx.pal.accent : ctx.pal.muted, a),
                 tokens::icon::strokeWidth);
        t.drawTextInBox(e.name, Rect{row.x + 38.f, row.y + 4.f, row.w - 48.f, 20.f},
                        styleFor(tokens::type_body, fade(ctx.pal.text, a)),
                        TextAlign::Left);
        t.drawTextInBox(e.meta, Rect{row.x + 38.f, row.y + 22.f, row.w - 48.f, 18.f},
                        styleFor(tokens::type_small, fade(ctx.pal.muted, a)),
                        TextAlign::Left);
    }
    t.restore();
}

}  // namespace folio::ui
