// folio-ui/components/Annotations.cpp

#include "components/Annotations.h"
#include "folio/ui/Draw.h"
#include "folio/ui/Icons.h"

#include <algorithm>
#include <cmath>

namespace folio::ui {

using folio::render::IPaintTarget;
using folio::render::Corners;
using folio::render::LineCap;
using folio::render::LineJoin;
using folio::render::PathVerb;

namespace {

/// Shortest distance from `p` to the segment ab, all normalised.
float distToSegment(Point p, Point a, Point b) {
    const float vx = b.x - a.x, vy = b.y - a.y;
    const float wx = p.x - a.x, wy = p.y - a.y;
    const float len2 = vx * vx + vy * vy;
    float tt = len2 > 1e-9f ? (wx * vx + wy * vy) / len2 : 0.f;
    tt = std::clamp(tt, 0.f, 1.f);
    const float dx = p.x - (a.x + tt * vx), dy = p.y - (a.y + tt * vy);
    return std::sqrt(dx * dx + dy * dy);
}

bool hits(const Annotation& a, Point p, float tol) {
    if (a.kind == AnnotKind::Ink) {
        for (std::size_t i = 1; i < a.points.size(); ++i)
            if (distToSegment(p, a.points[i - 1], a.points[i]) <= tol) return true;
        return false;
    }
    const Rect& b = a.box;
    return p.x >= b.x - tol && p.x <= b.x + b.w + tol && p.y >= b.y - tol &&
           p.y <= b.y + b.h + tol;
}

}  // namespace

std::vector<const Annotation*> AnnotationStore::forPage(int page) const {
    std::vector<const Annotation*> out;
    for (const Annotation& a : items_)
        if (a.page == page) out.push_back(&a);
    return out;
}

std::size_t AnnotationStore::countOn(int page) const {
    return static_cast<std::size_t>(
        std::count_if(items_.begin(), items_.end(),
                      [page](const Annotation& a) { return a.page == page; }));
}

std::uint64_t AnnotationStore::hitTest(int page, Point p, float tolerance) const {
    // Topmost first, same ordering rule as eraseAt: the last annotation
    // added is painted last, so it is the one under the cursor.
    for (auto it = items_.rbegin(); it != items_.rend(); ++it)
        if (it->page == page && hits(*it, p, tolerance)) return it->id;
    return 0;
}

bool AnnotationStore::eraseAt(int page, Point p, float tolerance) {
    // Topmost first: the most recently added annotation is drawn last, so it
    // is the one the user is pointing at.
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
        if (it->page != page || !hits(*it, p, tolerance)) continue;
        items_.erase(std::next(it).base());
        ++revision_;
        return true;
    }
    return false;
}

void AnnotationStore::removeById(std::uint64_t id) {
    const auto before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [id](const Annotation& a) { return a.id == id; }),
                 items_.end());
    if (items_.size() != before) ++revision_;
}

void AnnotationStore::clearPage(int page) {
    const auto before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [page](const Annotation& a) { return a.page == page; }),
                 items_.end());
    if (items_.size() != before) ++revision_;
}

bool AnnotationStore::undo() {
    if (items_.empty()) return false;
    items_.pop_back();
    ++revision_;
    return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void drawAnnotation(IPaintTarget& t, const Annotation& a, const Rect& pageRect,
                    float alpha) {
    const float W = pageRect.w, H = pageRect.h;
    const auto px = [&](Point n) {
        return Point{pageRect.x + n.x * W, pageRect.y + n.y * H};
    };
    const auto rc = [&](const Rect& n) {
        return Rect{pageRect.x + n.x * W, pageRect.y + n.y * H, n.w * W, n.h * H};
    };
    const float af = alpha * a.opacity;
    const Color col = fade(a.color, af);
    const float sw = std::max(1.f, a.strokePx(W));

    switch (a.kind) {
        case AnnotKind::Highlight: {
            // Multiply would be truer to a marker pen, but the backend-neutral
            // interface has no multiply blend; a low alpha over light paper is
            // visually equivalent and costs nothing.
            t.fillRoundedRect(rc(a.box), Corners::all(2.f),
                              fade(a.color, af * kHighlightAlpha));
            break;
        }
        case AnnotKind::Underline: {
            // PDF puts the underline just below the glyph box rather than on
            // the baseline, which is what keeps it clear of descenders.
            const Rect r = rc(a.box);
            const float w = std::max(1.f, r.h * 0.055f);
            const float y = r.bottom() - std::max(1.f, r.h * 0.08f);
            t.drawLine({r.x, y}, {r.right(), y}, fade(a.color, af), w);
            break;
        }
        case AnnotKind::Strikethrough: {
            // Through the x-height centre, not the box centre: the box
            // includes ascenders, and splitting it looks high.
            const Rect r = rc(a.box);
            const float w = std::max(1.f, r.h * 0.055f);
            const float y = r.y + r.h * 0.58f;
            t.drawLine({r.x, y}, {r.right(), y}, fade(a.color, af), w);
            break;
        }
        case AnnotKind::Ink: {
            if (a.points.size() < 2) break;
            std::vector<PathVerb> path;
            path.reserve(a.points.size());
            const Point p0 = px(a.points[0]);
            path.push_back(PathVerb{PathVerb::Op::MoveTo, p0.x, p0.y});
            // Quadratic through midpoints: a polyline of raw pointer samples
            // looks faceted at high zoom, and this costs one extra point.
            for (std::size_t i = 1; i + 1 < a.points.size(); ++i) {
                const Point c = px(a.points[i]);
                const Point n = px(a.points[i + 1]);
                path.push_back(PathVerb{PathVerb::Op::QuadTo, c.x, c.y,
                                        (c.x + n.x) * 0.5f, (c.y + n.y) * 0.5f});
            }
            const Point pl = px(a.points.back());
            path.push_back(PathVerb{PathVerb::Op::LineTo, pl.x, pl.y});
            t.strokePath(path, col, sw, LineCap::Round, LineJoin::Round);
            break;
        }
        case AnnotKind::Box:
            t.strokeRoundedRect(rc(a.box), Corners::all(2.f), col, sw);
            break;
        case AnnotKind::Ellipse: {
            // No ellipse primitive; a rounded rect with radius = half the
            // short side is one.
            const Rect r = rc(a.box);
            t.strokeRoundedRect(r, Corners::all(std::min(r.w, r.h) * 0.5f), col, sw);
            break;
        }
        case AnnotKind::Note: {
            const Rect r = rc(a.box);
            const Rect pin{r.x, r.y, 22.f, 22.f};
            t.fillRoundedRect(pin, Corners::all(6.f), fade(a.color, af));
            drawIcon(t, "note", pin.inset(5.f), fade(Color{255, 255, 255, 255}, af),
                     1.8f);
            break;
        }
        case AnnotKind::Text: {
            // Free text belongs to the page, not to the interface, so it has
            // to scale with the zoom the way every other annotation does.
            // Using the UI's body size would keep it the same number of
            // screen pixels at 50% and at 400% - it would float above the
            // document rather than sit on it, and it would export at the
            // wrong size.
            const Rect r = rc(a.box);
            folio::render::TextStyle ts = styleFor(tokens::type_body, col);
            ts.size = std::max(6.f, (a.fontSize > 0.f ? a.fontSize : a.box.h * 0.74f) * H);
            ts.family = a.font;
            t.drawTextInBox(a.text, r, ts, folio::render::TextAlign::Left);
            break;
        }
        case AnnotKind::Stamp: {
            const Rect r = rc(a.box);
            t.strokeRoundedRect(r, Corners::all(6.f), col, 2.f);
            t.drawTextInBox(a.text.empty() ? "APPROVED" : a.text, r,
                            styleFor(tokens::type_bodyStrong, col),
                            folio::render::TextAlign::Centre);
            break;
        }
        case AnnotKind::Signature: {
            if (a.points.size() < 2) break;
            std::vector<PathVerb> path;
            const Point p0 = px(a.points[0]);
            path.push_back(PathVerb{PathVerb::Op::MoveTo, p0.x, p0.y});
            for (std::size_t i = 1; i < a.points.size(); ++i) {
                const Point c = px(a.points[i]);
                path.push_back(PathVerb{PathVerb::Op::LineTo, c.x, c.y});
            }
            t.strokePath(path, col, sw, LineCap::Round, LineJoin::Round);
            break;
        }
    }
}

}  // namespace folio::ui
