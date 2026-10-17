// folio-ui/components/Annotations.cpp

#include "components/Annotations.h"
#include "folio/ui/Draw.h"
#include "folio/ui/Icons.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>

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
    if (a.kind == AnnotKind::Ink || a.kind == AnnotKind::Signature ||
        a.kind == AnnotKind::Line || a.kind == AnnotKind::Arrow) {
        for (std::size_t i = 1; i < a.points.size(); ++i)
            if (distToSegment(p, a.points[i - 1], a.points[i]) <= tol) return true;
        return false;
    }
    const Rect& b = a.box;
    return p.x >= b.x - tol && p.x <= b.x + b.w + tol && p.y >= b.y - tol &&
           p.y <= b.y + b.h + tol;
}

}  // namespace

bool AnnotationStore::saveSidecar(const std::string& file,
                                  const std::string& document) const {
    if (file.empty() || document.empty()) return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path path = fs::u8path(file);
    if (items_.empty()) { fs::remove(path, ec); return true; }
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "FOLIO_ANNOTATIONS_V3 " << std::quoted(document) << ' '
            << items_.size() << '\n';
        out << std::setprecision(9);
        for (const auto& a : items_) {
            out << a.id << ' ' << a.group << ' ' << a.textLinked << ' '
                << static_cast<int>(a.kind) << ' ' << a.page << ' '
                << static_cast<int>(a.color.r) << ' '
                << static_cast<int>(a.color.g) << ' '
                << static_cast<int>(a.color.b) << ' '
                << static_cast<int>(a.color.a) << ' '
                << a.width << ' ' << a.opacity << ' '
                << a.box.x << ' ' << a.box.y << ' ' << a.box.w << ' ' << a.box.h << ' '
                << a.fontSize << ' ' << static_cast<int>(a.font) << ' '
                << std::quoted(a.text) << ' ' << std::quoted(a.comment) << ' '
                << a.points.size();
            for (const Point& p : a.points) out << ' ' << p.x << ' ' << p.y;
            out << ' ' << a.sourceIndex << ' ' << a.sourceSubtype << ' ' << a.deleted
                << ' ' << a.sourceFingerprint;
            out << '\n';
        }
        out.flush();
        if (!out) { fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear(); fs::rename(tmp, path, ec);
    }
    return !ec;
}

bool AnnotationStore::loadSidecar(const std::string& file,
                                  const std::string& document) {
    namespace fs = std::filesystem;
    if (file.empty() || document.empty()) return false;
    std::error_code ec;
    const fs::path path = fs::u8path(file);
    if (fs::file_size(path, ec) > 32u * 1024u * 1024u || ec) return false;
    std::ifstream in(path, std::ios::binary);
    std::string magic, stored;
    std::size_t count = 0;
    if (!(in >> magic >> std::quoted(stored) >> count) ||
        (magic != "FOLIO_ANNOTATIONS_V1" && magic != "FOLIO_ANNOTATIONS_V2" &&
         magic != "FOLIO_ANNOTATIONS_V3") ||
        stored != document || count > 50000)
        return false;
    std::vector<Annotation> loaded;
    loaded.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Annotation a;
        int kind = 0, red = 0, green = 0, blue = 0, alpha = 0, font = 0;
        std::size_t points = 0;
        if (!(in >> a.id >> a.group >> a.textLinked >> kind >> a.page
              >> red >> green >> blue >> alpha >> a.width >> a.opacity
              >> a.box.x >> a.box.y >> a.box.w >> a.box.h
              >> a.fontSize >> font >> std::quoted(a.text)
              >> std::quoted(a.comment) >> points) ||
            kind < 0 || kind > static_cast<int>(AnnotKind::Arrow) ||
            points > 10000 || a.comment.size() > 131072 || a.page < 0 ||
            red < 0 || red > 255 || green < 0 || green > 255 ||
            blue < 0 || blue > 255 || alpha < 0 || alpha > 255 ||
            font < 0 || font > static_cast<int>(folio::render::FontFamily::Mono))
            return false;
        a.kind = static_cast<AnnotKind>(kind);
        a.font = static_cast<folio::render::FontFamily>(font);
        a.color = Color{static_cast<std::uint8_t>(red),
                        static_cast<std::uint8_t>(green),
                        static_cast<std::uint8_t>(blue),
                        static_cast<std::uint8_t>(alpha)};
        for (std::size_t j = 0; j < points; ++j) {
            Point p{};
            if (!(in >> p.x >> p.y)) return false;
            a.points.push_back(p);
        }
        if (magic == "FOLIO_ANNOTATIONS_V2" || magic == "FOLIO_ANNOTATIONS_V3") {
            if (!(in >> a.sourceIndex >> a.sourceSubtype >> a.deleted) ||
                a.sourceIndex < -1 || a.sourceIndex > 5000 ||
                (a.sourceIndex >= 0 && a.sourceSubtype <= 0)) return false;
            if (magic == "FOLIO_ANNOTATIONS_V3" &&
                (!(in >> a.sourceFingerprint) ||
                 (a.sourceIndex >= 0 && !a.sourceFingerprint))) return false;
            if (magic == "FOLIO_ANNOTATIONS_V2" && a.sourceIndex >= 0) return false;
        }
        loaded.push_back(std::move(a));
    }
    clear();
    for (const auto& a : loaded) addExisting(a);
    return true;
}

void AnnotationStore::ensurePageIndex() const {
    if (indexedEpoch_ == indexEpoch_) return;
    pageIndex_.clear();
    for (std::size_t i = 0; i < items_.size(); ++i)
        pageIndex_[items_[i].page].push_back(i);
    indexedEpoch_ = indexEpoch_;
}

std::vector<const Annotation*> AnnotationStore::forPage(int page) const {
    ensurePageIndex();
    std::vector<const Annotation*> out;
    const auto it = pageIndex_.find(page);
    if (it == pageIndex_.end()) return out;
    out.reserve(it->second.size());
    for (auto i : it->second)
        if (!items_[i].deleted && kindVisible(items_[i].kind)) out.push_back(&items_[i]);
    return out;
}

std::size_t AnnotationStore::countOn(int page) const {
    ensurePageIndex();
    const auto it = pageIndex_.find(page);
    if (it == pageIndex_.end()) return 0;
    return static_cast<std::size_t>(std::count_if(it->second.begin(), it->second.end(),
        [this](std::size_t i) { return !items_[i].deleted; }));
}

std::uint64_t AnnotationStore::hitTest(int page, Point p, float tolerance) const {
    ensurePageIndex();
    const auto it = pageIndex_.find(page);
    if (it == pageIndex_.end()) return 0;
    for (auto ri = it->second.rbegin(); ri != it->second.rend(); ++ri) {
        const Annotation& a = items_[*ri];
        if (!a.deleted && kindVisible(a.kind) && hits(a, p, tolerance)) return a.id;
    }
    return 0;
}

bool AnnotationStore::eraseAt(int page, Point p, float tolerance) {
    const auto id = hitTest(page, p, tolerance);
    if (!id) return false;
    removeById(id);
    return true;
}

void AnnotationStore::removeById(std::uint64_t id) {
    const auto before = items_.size();
    for (auto& item : items_) {
        if (item.id == id && item.sourceIndex >= 0) {
            item.deleted = true;
            ++revision_; ++indexEpoch_;
            return;
        }
    }
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [id](const Annotation& a) { return a.id == id; }),
                 items_.end());
    if (items_.size() != before) { ++revision_; ++indexEpoch_; }
}

void AnnotationStore::clearPage(int page) {
    for (auto& a : items_)
        if (a.page == page && a.sourceIndex >= 0) a.deleted = true;
    const auto before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [page](const Annotation& a) {
                                    return a.page == page && a.sourceIndex < 0;
                                }),
                 items_.end());
    if (items_.size() != before) { ++revision_; ++indexEpoch_; }
}

bool AnnotationStore::undo() {
    if (items_.empty()) return false;
    items_.pop_back();
    ++revision_;
    ++indexEpoch_;
    return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void drawAnnotation(IPaintTarget& t, const Annotation& a, const Rect& pageRect,
                    float alpha, bool darkPaper) {
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
            // Unlike alpha-over, Multiply preserves dark PDF ink while
            // tinting the paper. Inverted dark pages need Screen to preserve
            // their pale ink; restore the blend BEFORE other marks/text.
            t.setBlendMode(darkPaper ? folio::render::BlendMode::Screen
                                     : folio::render::BlendMode::Multiply);
            t.fillRoundedRect(rc(a.box), Corners::all(2.f),
                              fade(a.color, af * kHighlightAlpha));
            t.setBlendMode(folio::render::BlendMode::SrcOver);
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
        case AnnotKind::Line:
        case AnnotKind::Arrow: {
            if (a.points.size() < 2) break;
            const Point start = px(a.points.front()), end = px(a.points.back());
            t.drawLine(start, end, col, sw);
            if (a.kind == AnnotKind::Arrow) {
                const float dx = end.x - start.x, dy = end.y - start.y;
                const float length = std::hypot(dx, dy);
                if (length > 2.f) {
                    const float tip = std::min(18.f, std::max(8.f, sw * 4.f));
                    const float ux = dx / length, uy = dy / length;
                    t.drawLine(end, {end.x - ux * tip - uy * tip * .5f,
                                     end.y - uy * tip + ux * tip * .5f}, col, sw);
                    t.drawLine(end, {end.x - ux * tip + uy * tip * .5f,
                                     end.y - uy * tip - ux * tip * .5f}, col, sw);
                }
            }
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
            const Rect pin{r.x, r.y, std::max(12.f, r.w),
                           std::max(12.f, r.h)};
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
