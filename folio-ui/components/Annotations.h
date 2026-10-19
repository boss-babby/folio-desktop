// folio-ui/components/Annotations.h
//
// The annotation model.
//
// WHY COORDINATES ARE NORMALISED TO THE PAGE
// ------------------------------------------
// Every geometric field here is in page space: 0..1 across the page's width
// and height, with the origin at the page's top-left. Nothing is stored in
// screen pixels.
//
// That is the only representation that survives the things a reader does.
// Zoom, rotation, a window resize, switching from continuous to two-up, and
// loading the same file on a phone all change where a page lands on screen
// and how big it is; none of them should move a highlight relative to the
// word it sits on. Storing screen coordinates would require rewriting every
// annotation on each of those events, and would quietly lose precision each
// time. It is also the representation PDF itself wants: converting 0..1 to
// PDF user space is one multiply by the page box.
//
// The cost is a multiply per point per frame when drawing, which is nothing
// next to the text it is drawn over.

#pragma once

#include "folio/render/IPaintTarget.h"

#include <cstdint>
#include <array>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

namespace folio::ui {

using folio::render::Color;
using folio::render::Point;
using folio::render::Rect;

/// How much of a highlight's nominal opacity actually reaches the paper.
///
/// A highlight is meant to tint text, not cover it. The on-screen overlay and
/// the appearance stream written into the PDF must agree on this or a
/// document changes appearance the moment it is saved - which it did, until
/// both sides were pointed at this constant.
inline constexpr float kHighlightAlpha = 0.38f;

enum class AnnotKind : std::uint8_t {
    Highlight,      ///< translucent quad over text
    Underline,      ///< rule along the text baseline
    Strikethrough,  ///< rule through the middle of the text
    Ink,            ///< freehand polyline
    Note,       ///< anchored sticky note
    Box,        ///< rectangle outline
    Ellipse,
    Text,       ///< free text
    Stamp,
    Signature,
    Line,       ///< straight free-form line
    Arrow,      ///< line with arrowhead at its end
};

struct Annotation {
    std::uint64_t id = 0;
    /// Non-zero when this annotation is one line of a multi-line markup.
    /// A selection spanning three lines is three annotations (PDF text
    /// markup is per-quad), but it is ONE thing to the person who made it:
    /// clicking any line selects, recolours and deletes all of them.
    std::uint64_t group = 0;
    /// True only for markup created from selected PDF glyph quads. A box
    /// drawn with the highlighter tool is free-form and remains movable.
    bool textLinked = false;
    // Imported editable PDF annotation converted to a Folio overlay. The
    // source object remains in the original PDF; the viewer suppresses it
    // and export replaces ONLY the matching subtype/index. A tombstone keeps
    // deletions durable and undoable without deleting unrelated PDF objects.
    int sourceIndex = -1;
    int sourceSubtype = 0;
    std::uint64_t sourceFingerprint = 0;
    bool deleted = false;
    AnnotKind kind = AnnotKind::Ink;
    int page = 0;

    Color color{79, 70, 229, 255};
    float width = 2.5f;    ///< stroke width in *page* units x1000, see strokePx()
    float opacity = 1.f;

    /// Normalised bounding box, used by every kind except Ink.
    Rect box{0.f, 0.f, 0.f, 0.f};
    /// Normalised polyline, used by Ink.
    std::vector<Point> points;
    std::string text;
    std::string comment; ///< note body / attached comment (PDF /Contents)

    // --- free text only ------------------------------------------------------
    /// Glyph height as a fraction of page height, so the size is a property
    /// of the document rather than of the current zoom. 0 means "derive it
    /// from the box", which is what pre-existing annotations do.
    float fontSize = 0.f;
    folio::render::FontFamily font = folio::render::FontFamily::Sans;

    /// Stroke width in screen pixels at a given on-screen page width.
    /// Width is authored against a 560 pt reference page so a 2.5 pt pen
    /// stays 2.5 pt whatever the zoom.
    [[nodiscard]] float strokePx(float pageWpx) const {
        return width * (pageWpx / 560.f);
    }

    [[nodiscard]] bool empty() const {
        if (kind == AnnotKind::Line || kind == AnnotKind::Arrow) {
            if (points.size() < 2) return true;
            const float dx = points.back().x - points.front().x;
            const float dy = points.back().y - points.front().y;
            return dx * dx + dy * dy < .000004f;
        }
        if (kind == AnnotKind::Ink || kind == AnnotKind::Signature)
            return points.size() < 2;
        return box.w <= .0005f && box.h <= .0005f;
    }
};

/// Authoritative annotations plus a lazily rebuilt per-page lookup. Index
/// entries are offsets, never pointers, so vector reallocations cannot dangle.
class AnnotationStore {
public:
    std::uint64_t add(Annotation a) {
        a.id = ++nextId_;
        items_.push_back(std::move(a));
        ++revision_;
        ++indexEpoch_;
        return nextId_;
    }

    /// Re-insert an annotation that already has an id - used by undo, which
    /// must restore the *same* id so any later command referring to it still
    /// resolves. Keeps the id counter above anything reinstated.
    void addExisting(const Annotation& a) {
        for (auto& existing : items_) {
            if (existing.id == a.id) {
                existing = a;
                ++revision_;
                ++indexEpoch_;
                return;
            }
        }
        items_.push_back(a);
        if (a.id > nextId_) nextId_ = a.id;
        ++revision_;
        ++indexEpoch_;
    }

    /// Overwrite an annotation in place. Returns false if the id is gone.
    bool replace(std::uint64_t id, const Annotation& with) {
        for (auto& it : items_) {
            if (it.id != id) continue;
            if (it.page != with.page) ++indexEpoch_;
            it = with;
            it.id = id;
            ++revision_;
            return true;
        }
        return false;
    }

    /// Mark the store dirty without changing anything structurally - used
    /// while dragging, where the annotation is mutated in place.
    void touch() noexcept { ++revision_; }

    [[nodiscard]] const Annotation* get(std::uint64_t id) const {
        for (const auto& it : items_)
            if (it.id == id) return &it;
        return nullptr;
    }
    [[nodiscard]] Annotation* get(std::uint64_t id) {
        for (auto& it : items_)
            if (it.id == id) return &it;
        return nullptr;
    }

    /// Every annotation sharing this one's group, including itself.
    /// Returns just {id} for an ungrouped annotation, so callers can treat
    /// the grouped and solitary cases identically.
    [[nodiscard]] std::vector<std::uint64_t> groupMembers(std::uint64_t id) const {
        std::vector<std::uint64_t> out;
        const Annotation* a = get(id);
        if (!a) return out;
        if (a->group == 0) { out.push_back(id); return out; }
        for (const auto& it : items_)
            if (it.group == a->group) out.push_back(it.id);
        if (out.empty()) out.push_back(id);
        return out;
    }

    /// Topmost annotation under a normalised point, or 0. Later annotations
    /// are drawn on top, so the search runs backwards.
    [[nodiscard]] std::uint64_t hitTest(int page, Point p, float tolerance) const;

    /// Remove whatever sits under a normalised point on `page`, topmost
    /// first. Returns true if something went.
    bool eraseAt(int page, Point p, float tolerance);

    void removeById(std::uint64_t id);
    void clearPage(int page);
    void clear() {
        items_.clear();
        ++revision_;
        ++indexEpoch_;
    }

    [[nodiscard]] const std::vector<Annotation>& all() const noexcept { return items_; }
    /// Move an already indexed tab's annotations out and back without
    /// copying every stroke or calling addExisting (which scans by id).
    /// Both calls are on the UI thread; nextId_ stays monotonic across tabs.
    [[nodiscard]] std::vector<Annotation> takeAll() noexcept {
        auto snapshot = std::move(items_);
        items_.clear();
        ++revision_;
        ++indexEpoch_;
        return snapshot;
    }
    void restoreAll(std::vector<Annotation>&& snapshot) noexcept {
        items_ = std::move(snapshot);
        ++revision_;
        ++indexEpoch_;
    }
    bool saveSidecar(const std::string& file, const std::string& document) const;
    bool loadSidecar(const std::string& file, const std::string& document);
    [[nodiscard]] std::vector<const Annotation*> forPage(int page) const;
    [[nodiscard]] std::size_t count() const noexcept { return items_.size(); }
    [[nodiscard]] std::size_t countOn(int page) const;
    void setKindVisible(AnnotKind kind, bool on) noexcept {
        visible_[static_cast<std::size_t>(kind)] = on;
        ++revision_;
    }
    [[nodiscard]] bool kindVisible(AnnotKind kind) const noexcept {
        return visible_[static_cast<std::size_t>(kind)];
    }
    void resetVisibility() noexcept { visible_.fill(true); ++revision_; }

    /// Bumped on every mutation, so views can tell when to redraw without
    /// diffing the contents.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    /// Annotation currently being drawn, if any. Kept separate from the
    /// committed list so an in-progress stroke never lands in undo and
    /// never has to be removed if the gesture is cancelled.
    Annotation* beginDraft(Annotation a) {
        draft_ = std::move(a);
        hasDraft_ = true;
        return &draft_;
    }
    [[nodiscard]] Annotation* draft() noexcept { return hasDraft_ ? &draft_ : nullptr; }
    [[nodiscard]] const Annotation* draft() const noexcept {
        return hasDraft_ ? &draft_ : nullptr;
    }
    void cancelDraft() noexcept { hasDraft_ = false; }
    /// Commit the draft if it is worth keeping. Returns the new id, or 0.
    std::uint64_t commitDraft() {
        if (!hasDraft_) return 0;
        hasDraft_ = false;
        if (draft_.empty()) return 0;
        return add(draft_);
    }

    // --- undo ---------------------------------------------------------------
    // NOTE: undo lives in UndoStack (components/Edits.h), not here. A store
    // that can only pop its last entry cannot express a move or a delete,
    // which is what this used to be.
    bool undo();
    [[nodiscard]] bool canUndo() const noexcept { return !items_.empty(); }

private:
    void ensurePageIndex() const;
    mutable std::unordered_map<int, std::vector<std::size_t>> pageIndex_;
    mutable std::uint64_t indexedEpoch_ = ~std::uint64_t{0};
    std::uint64_t indexEpoch_ = 0;
    std::array<bool, static_cast<std::size_t>(AnnotKind::Arrow) + 1> visible_ = [] {
        std::array<bool, static_cast<std::size_t>(AnnotKind::Arrow) + 1> v{};
        v.fill(true);
        return v;
    }();
    std::vector<Annotation> items_;
    Annotation draft_;
    bool hasDraft_ = false;
    std::uint64_t nextId_ = 0;
    std::uint64_t revision_ = 0;
};

/// Draw one annotation into a page rect already placed on screen.
/// `pageRect` is where the page is, in the target's current coordinates.
void drawAnnotation(folio::render::IPaintTarget& t, const Annotation& a,
                    const Rect& pageRect, float alpha, bool darkPaper = false);

}  // namespace folio::ui
