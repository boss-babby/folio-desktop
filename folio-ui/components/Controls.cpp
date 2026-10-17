#include "components/Controls.h"

#include "components/Overlays.h"
#include "folio/ui/Icons.h"

#include <algorithm>
#include <cmath>

namespace folio::ui {
namespace {

/// Walk back one UTF-8 code point from `i`.
std::size_t prevCodepoint(const std::string& s, std::size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

std::size_t nextCodepoint(const std::string& s, std::size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

}  // namespace

Color resolveRole(TextRole r, const tokens::Palette& p) {
    switch (r) {
        case TextRole::Muted:    return p.muted;
        case TextRole::Accent:   return p.accentText;
        case TextRole::Danger:   return p.danger;
        case TextRole::OnAccent: return Color{255, 255, 255, 255};
        case TextRole::Primary:  break;
    }
    return p.text;
}

// ---------------------------------------------------------------------------
// Label
// ---------------------------------------------------------------------------

Label::Label(std::string t, tokens::TypeScale sc, TextRole r)
    : text(std::move(t)), scale(sc), role(r) {
    acceptsPointer = false;
    uppercase = sc.uppercase;
    debugName = "Label(" + text + ")";
}

float Label::intrinsicWidth(IPaintTarget& t) const {
    const auto ts = styleFor(scale, Color{});
    return t.measureText(uppercase ? upperAscii(text) : text, ts);
}

void Label::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    if (text.empty()) return;
    const auto ts = styleFor(scale, resolveRole(role, ctx.pal));
    t.drawTextInBox(uppercase ? upperAscii(text) : text, localBounds(), ts, align);
}

// ---------------------------------------------------------------------------
// Divider
// ---------------------------------------------------------------------------

void Divider::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    const Rect b = localBounds();
    if (vertical_) {
        dividerV(t, b.w * 0.5f, inset, b.h - inset, ctx.pal.divider, ctx.dpr);
    } else {
        hairline(t, Point{inset, b.h * 0.5f}, Point{b.w - inset, b.h * 0.5f},
                 ctx.pal.divider, ctx.dpr);
    }
}

// ---------------------------------------------------------------------------
// SegmentedControl
// ---------------------------------------------------------------------------

SegmentedControl::SegmentedControl(std::vector<Item> items, int sel)
    : items_(std::move(items)), selected_(sel) {
    acceptsPointer = true;
    debugName = "SegmentedControl";
}

Rect SegmentedControl::cellRect(std::size_t i) const {
    const float n = static_cast<float>(std::max<std::size_t>(items_.size(), 1));
    const float pad = 3.f;
    const float cw = (frame.w - pad * 2.f) / n;
    return Rect{pad + cw * static_cast<float>(i), pad, cw, frame.h - pad * 2.f};
}

int SegmentedControl::indexAt(Point local) const {
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const Rect r = cellRect(i);
        if (local.x >= r.x && local.x < r.right()) return static_cast<int>(i);
    }
    return -1;
}

void SegmentedControl::setSelected(int i, double now) {
    if (i < 0 || static_cast<std::size_t>(i) >= items_.size() || i == selected_) return;
    selected_ = i;
    const Rect r = cellRect(static_cast<std::size_t>(i));
    pillX_.animateTo(r.x, tokens::dur::paneSwitch, now);
    pillW_.animateTo(r.w, tokens::dur::paneSwitch, now);
    if (onChange) onChange(i);
}

float SegmentedControl::intrinsicWidth(IPaintTarget& t) const {
    float w = 6.f;
    for (const Item& it : items_) {
        float cw = tokens::space::pad * 2.f;
        if (!it.icon.empty()) cw += tokens::icon::sizeSmall + tokens::space::tight;
        if (!it.label.empty()) cw += t.measureText(it.label, styleFor(tokens::type_body, Color{}));
        w += std::max(cw, 44.f);
    }
    return w;
}

bool SegmentedControl::onPointer(const PointerEvent& e) {
    switch (e.phase) {
        case PointerPhase::Move:
        case PointerPhase::Enter: {
            const int idx = indexAt(e.pos);
            const bool changed = idx != hoverIndex_;
            if (changed) {
                hoverIndex_ = idx;
                if (!e.isTouch() && idx >= 0 &&
                    !items_[static_cast<std::size_t>(idx)].tooltip.empty()) {
                    OverlayHost::current().requestTooltip(
                        this, items_[static_cast<std::size_t>(idx)].tooltip, e.timestamp);
                }
            }
            return changed;
        }
        case PointerPhase::Leave:
            hoverIndex_ = -1;
            OverlayHost::current().cancelTooltip(this);
            return true;
        case PointerPhase::Up:
            if (hitLocal(e.pos)) setSelected(indexAt(e.pos), e.timestamp);
            return true;
        case PointerPhase::Down:
            return true;
        default:
            break;
    }
    return false;
}

bool SegmentedControl::animating(double now) const {
    return pillX_.active(now) || pillW_.active(now);
}

void SegmentedControl::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    const Rect b = localBounds();
    if (b.empty() || items_.empty()) return;

    // Lay the pill out on the first paint, when the frame is finally known.
    if (!pillInitialised_) {
        const Rect r = cellRect(static_cast<std::size_t>(selected_));
        const_cast<SegmentedControl*>(this)->pillX_.reset(r.x);
        const_cast<SegmentedControl*>(this)->pillW_.reset(r.w);
        const_cast<SegmentedControl*>(this)->pillInitialised_ = true;
    }

    t.fillRoundedRect(b, Corners::all(tokens::radius::segment), ctx.pal.surfaceSunken);

    const Rect pill{pillX_.value(ctx.now), 3.f, pillW_.value(ctx.now), b.h - 6.f};
    drawCard(t, pill, Corners::all(tokens::radius::sm), ctx.pal.card,
             tokens::elevation::tabActive(ctx.pal));

    for (std::size_t i = 0; i < items_.size(); ++i) {
        const Rect cell = cellRect(i);
        const bool sel = static_cast<int>(i) == selected_;
        const bool hov = static_cast<int>(i) == hoverIndex_ && !sel;
        Color fg = sel ? ctx.pal.text : ctx.pal.muted;
        if (hov) fg = lerpColor(fg, ctx.pal.text, 0.5f);

        const Item& it = items_[i];
        float contentW = 0.f;
        const auto ts = styleFor(sel ? tokens::type_bodyStrong : tokens::type_body, fg);
        if (!it.icon.empty()) contentW += tokens::icon::sizeSmall;
        if (!it.label.empty()) {
            if (contentW > 0.f) contentW += tokens::space::tight;
            contentW += t.measureText(it.label, ts);
        }
        float x = cell.x + (cell.w - contentW) * 0.5f;
        if (!it.icon.empty()) {
            drawIcon(t, it.icon,
                     Rect{x, cell.y + (cell.h - tokens::icon::sizeSmall) * 0.5f,
                          tokens::icon::sizeSmall, tokens::icon::sizeSmall},
                     fg, tokens::icon::strokeWidth);
            x += tokens::icon::sizeSmall + tokens::space::tight;
        }
        if (!it.label.empty()) {
            t.drawTextInBox(it.label, Rect{x, cell.y, cell.right() - x, cell.h}, ts,
                            folio::render::TextAlign::Left);
        }
    }
}

// ---------------------------------------------------------------------------
// TextField
// ---------------------------------------------------------------------------

TextField::TextField(std::string ph) : placeholder(std::move(ph)) {
    acceptsPointer = true;
    debugName = "TextField";
}

void TextField::onFocusChanged(bool gained) {
    if (onFocus) onFocus(gained);
}

bool TextField::onPointer(const PointerEvent& e) {
    if (e.phase == PointerPhase::Down) {
        caret_ = text.size();
        if (multiline && !visibleRows_.empty()) {
            const EditRow* row = &visibleRows_.front();
            for (const auto& candidate : visibleRows_)
                if (e.pos.y >= candidate.y) row = &candidate;
            if (!row->positions.empty()) {
                const float x = e.pos.x - editLeft_;
                std::size_t nearest = 0;
                for (std::size_t i = 1; i < row->advances.size(); ++i)
                    if (x >= (row->advances[i - 1] + row->advances[i]) * .5f)
                        nearest = i;
                caret_ = std::min(row->positions[nearest], text.size());
            }
        }
        caretReset_ = e.timestamp;
        return true;
    }
    return e.phase == PointerPhase::Up;
}

bool TextField::onKey(const KeyEvent& e) {
    if (!e.pressed) return false;
    const std::size_t before = text.size();

    switch (e.key) {
        case Key::Backspace:
            if (caret_ > 0) {
                const std::size_t p = prevCodepoint(text, caret_);
                text.erase(p, caret_ - p);
                caret_ = p;
            }
            break;
        case Key::Delete:
            if (caret_ < text.size()) {
                text.erase(caret_, nextCodepoint(text, caret_) - caret_);
            }
            break;
        case Key::Left:  caret_ = prevCodepoint(text, caret_); break;
        case Key::Right: caret_ = nextCodepoint(text, caret_); break;
        case Key::Home:  caret_ = 0; break;
        case Key::End:   caret_ = text.size(); break;
        case Key::Enter:
            if (multiline) {
                text.insert(caret_, "\n");
                ++caret_;
            } else if (onSubmit) onSubmit();
            if (!multiline) return true;
            break;
        case Key::Escape:
            return false;  // let the popover close
        default:
            if (!e.text.empty()) {
                text.insert(caret_, e.text);
                caret_ += e.text.size();
            } else {
                return false;
            }
            break;
    }
    caretReset_ = 0.0;
    if (text.size() != before && onChanged) onChanged(text);
    return true;
}

bool TextField::animating(double now) const {
    return focusT_.active(now) || focused;  // caret blink needs frames
}

void TextField::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    const Rect b = localBounds();
    if (b.empty()) return;
    const_cast<TextField*>(this)->focusT_.animateTo(focused ? 1.f : 0.f,
                                                    tokens::dur::hoverFast, ctx.now);
    const float f = focusT_.value(ctx.now);
    const Corners rr = Corners::all(tokens::radius::control);

    if (flat) {
        // Chrome only once the control is being used: a hairline well on
        // hover, the full focused treatment while editing.
        const float h = hovered ? 1.f : 0.f;
        const float show = std::max(h * 0.55f, f);
        if (show > 0.01f) {
            t.fillRoundedRect(b, rr, fade(ctx.pal.surfaceSunken, show));
            t.strokeRoundedRect(
                b, rr, fade(lerpColor(ctx.pal.border, ctx.pal.accent, f), show),
                lerp(1.f, 1.6f, f));
        }
    } else {
        t.fillRoundedRect(b, rr, ctx.pal.surfaceSunken);
        t.strokeRoundedRect(b, rr, lerpColor(ctx.pal.border, ctx.pal.accent, f),
                            lerp(1.f, 1.6f, f));
    }

    // A flat field draws no chrome when idle, so it needs no breathing room
    // inside it either: the text should occupy the same box the label it
    // replaced did, or a value that used to fit now elides.
    float x = (flat && leadingIcon.empty()) ? 0.f : tokens::space::gap;
    if (!leadingIcon.empty()) {
        drawIcon(t, leadingIcon,
                 Rect{x, (b.h - tokens::icon::sizeSmall) * 0.5f, tokens::icon::sizeSmall,
                      tokens::icon::sizeSmall},
                 ctx.pal.muted, tokens::icon::strokeWidth);
        x += tokens::icon::sizeSmall + tokens::space::tight;
    }

    const bool showPlaceholder = text.empty();
    const auto ts = styleFor(tokens::type_body,
                             showPlaceholder ? ctx.pal.muted : ctx.pal.text);
    const Rect textBox{x, 0.f,
                       b.w - x - ((flat && leadingIcon.empty())
                                      ? 0.f
                                      : tokens::space::gap),
                       b.h};
    if (multiline) {
        // Keep each row's UTF-8 start so the caret, wrap viewport and touch
        // positioning all refer to the same text, even for multiline notes.
        const std::string& value = showPlaceholder ? placeholder : text;
        std::vector<std::string> rows{std::string{}};
        std::vector<std::size_t> starts{0};
        for (std::size_t i = 0; i < value.size();) {
            const std::size_t begin = i;
            if (value[i] == '\n') {
                rows.emplace_back();
                starts.push_back(++i);
                continue;
            }
            do { ++i; } while (i < value.size() &&
                              (static_cast<unsigned char>(value[i]) & 0xc0u) == 0x80u);
            const auto piece = value.substr(begin, i - begin);
            if (!rows.back().empty() &&
                t.measureText(rows.back() + piece, ts) > textBox.w) {
                rows.emplace_back();
                starts.push_back(begin);
            }
            rows.back() += piece;
        }
        const std::size_t cursor = std::min(caret_, text.size());
        const std::size_t caretRow = showPlaceholder ? 0 :
            static_cast<std::size_t>(std::upper_bound(starts.begin(), starts.end(),
                                                       cursor) - starts.begin() - 1);
        const float lineH = 21.f;
        const std::size_t maxRows = static_cast<std::size_t>(
            std::max(1.f, (b.h - 16.f) / lineH));
        const std::size_t first = rows.size() > maxRows
            ? (focused ? std::min(caretRow, rows.size() - maxRows)
                       : rows.size() - maxRows) : 0;
        visibleRows_.clear();
        editLeft_ = x;
        t.save();
        t.clipRoundedRect(b.inset(3.f), Corners::all(4.f));
        for (std::size_t i = first; i < rows.size() && i < first + maxRows; ++i) {
            const float y = 8.f + static_cast<float>(i-first)*lineH;
            t.drawTextInBox(rows[i], Rect{x, y, textBox.w, lineH}, ts,
                            folio::render::TextAlign::Left);
            // The cached glyph advances make a tap place the caret at a
            // character boundary rather than always appending to the note.
            EditRow run;
            run.y = y;
            run.positions.push_back(starts[i]);
            run.advances.push_back(0.f);
            for (std::size_t at = starts[i], end = at + rows[i].size(); at < end;) {
                at = nextCodepoint(value, at);
                run.positions.push_back(at);
                run.advances.push_back(t.measureText(
                    std::string_view(value).substr(starts[i], at - starts[i]), ts));
            }
            visibleRows_.push_back(std::move(run));
        }
        if (focused && std::fmod(ctx.now - caretReset_, 1.06) < .53 &&
            caretRow >= first && caretRow < first + maxRows) {
            const float cx = x + t.measureText(
                std::string_view(text).substr(starts[caretRow],
                                              cursor - starts[caretRow]), ts);
            t.fillRect(Rect{cx, 8.f + static_cast<float>(caretRow-first)*lineH,
                            1.4f, 18.f}, ctx.pal.accent);
        }
        t.restore();
        return;
    }
    t.drawTextInBox(showPlaceholder ? placeholder : text, textBox, ts, align);

    // Caret: 530 ms blink, restarted on every edit so it is solid while
    // typing. Hidden entirely when unfocused.
    if (focused) {
        const double since = ctx.now - caretReset_;
        const bool on = std::fmod(since, 1.06) < 0.53;
        if (on) {
            // The caret has to start where the text actually starts, which
            // is not `x` unless the text is left-aligned.
            const float full = t.measureText(text, ts);
            float originX = x;
            if (align == folio::render::TextAlign::Centre)
                originX = x + (textBox.w - full) * 0.5f;
            else if (align == folio::render::TextAlign::Right)
                originX = x + (textBox.w - full);
            const float cx =
                originX + t.measureText(std::string_view(text).substr(0, caret_), ts);
            const auto fm = t.fontMetrics(ts);
            const float h = -fm.ascent + fm.descent;
            t.fillRect(Rect{cx, (b.h - h) * 0.5f, 1.4f, h}, ctx.pal.accent);
        }
    }
}

// ---------------------------------------------------------------------------
// Toggle
// ---------------------------------------------------------------------------

Toggle::Toggle(bool on) : on_(on), knob_(on ? 1.f : 0.f) {
    acceptsPointer = true;
    debugName = "Toggle";
}

void Toggle::setOn(bool v, double now) {
    if (v == on_) return;
    on_ = v;
    knob_.animateTo(v ? 1.f : 0.f, tokens::dur::toggleKnob, now);
    if (onChange) onChange(v);
}

bool Toggle::onPointer(const PointerEvent& e) {
    if (e.phase == PointerPhase::Up && hitLocal(e.pos)) {
        setOn(!on_, e.timestamp);
        return true;
    }
    return e.phase == PointerPhase::Down;
}

bool Toggle::animating(double now) const { return knob_.active(now); }

void Toggle::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    const Rect b = localBounds();
    if (b.empty()) return;
    const float k = knob_.value(ctx.now);
    const float r = b.h * 0.5f;

    t.fillRoundedRect(b, Corners::all(r), lerpColor(ctx.pal.border, ctx.pal.accent, k));
    const float kr = r - 2.5f;
    const float cx = lerp(b.x + r, b.right() - r, k);
    t.drawShadow(Rect{cx - kr, b.y + 2.5f, kr * 2.f, kr * 2.f}, Corners::all(kr),
                 folio::render::Shadow{0.f, 1.f, 3.f, 0.f, ctx.pal.shadow});
    t.fillCircle(Point{cx, b.h * 0.5f}, kr, Color{255, 255, 255, 255});
}

// ---------------------------------------------------------------------------
// Slider
// ---------------------------------------------------------------------------

Slider::Slider(float mn, float mx, float v) : min_(mn), max_(mx), value_(v) {
    acceptsPointer = true;
    debugName = "Slider";
}

float Slider::normalised() const {
    return (max_ > min_) ? clamp01((value_ - min_) / (max_ - min_)) : 0.f;
}

void Slider::setValue(float v) {
    const float c = std::clamp(v, min_, max_);
    if (c == value_) return;
    value_ = c;
    if (onChange) onChange(c);
}

void Slider::setFromX(float x) {
    const float track = frame.w - 14.f;
    if (track <= 0.f) return;
    setValue(min_ + (max_ - min_) * clamp01((x - 7.f) / track));
}

bool Slider::onPointer(const PointerEvent& e) {
    switch (e.phase) {
        case PointerPhase::Down:
        case PointerPhase::Move:
            // Move only acts while captured, which the router guarantees by
            // only delivering Move to the capture target during a drag.
            if (e.phase == PointerPhase::Down || pressed) {
                setFromX(e.pos.x);
                return true;
            }
            return false;
        case PointerPhase::Up:
            return true;
        default:
            break;
    }
    return false;
}

bool Slider::animating(double now) const { return hoverT_.active(now); }

void Slider::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    const Rect b = localBounds();
    if (b.empty()) return;
    const_cast<Slider*>(this)->hoverT_.animateTo(hovered || pressed ? 1.f : 0.f,
                                                 tokens::dur::hoverFast, ctx.now);
    const float hv = hoverT_.value(ctx.now);

    const float cy = b.h * 0.5f;
    const float th = 4.f;
    t.fillRoundedRect(Rect{0.f, cy - th * 0.5f, b.w, th}, Corners::all(th * 0.5f),
                      ctx.pal.surfaceSunken);
    const float n = normalised();
    const float fillW = 7.f + (b.w - 14.f) * n;
    t.fillRoundedRect(Rect{0.f, cy - th * 0.5f, fillW, th}, Corners::all(th * 0.5f),
                      ctx.pal.accent);

    const float kr = lerp(6.f, 7.5f, hv);
    const Point c{7.f + (b.w - 14.f) * n, cy};
    t.drawShadow(Rect{c.x - kr, c.y - kr, kr * 2.f, kr * 2.f}, Corners::all(kr),
                 folio::render::Shadow{0.f, 1.f, 4.f, 0.f, ctx.pal.shadow});
    t.fillCircle(c, kr, Color{255, 255, 255, 255});
    t.fillCircle(c, kr - 2.f, ctx.pal.accent);
}

}  // namespace folio::ui
