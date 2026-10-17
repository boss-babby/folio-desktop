#include "components/Button.h"

#include "components/Overlays.h"
#include "folio/ui/Icons.h"

#include <algorithm>

namespace folio::ui {

Button::Button(std::string lbl, std::string ic, ButtonStyle st)
    : label(std::move(lbl)), icon(std::move(ic)), style(st) {
    acceptsPointer = true;
    debugName = "Button(" + label + (label.empty() ? icon : "") + ")";
}

std::unique_ptr<Button> Button::iconOnly(std::string ic, std::string tip) {
    auto b = std::make_unique<Button>("", std::move(ic), ButtonStyle::Icon);
    b->tooltip = std::move(tip);
    return b;
}

float Button::radius() const {
    switch (style) {
        case ButtonStyle::Window:   return 0.f;
        case ButtonStyle::Close:    return 0.f;
        case ButtonStyle::Segment:  return tokens::radius::sm;
        case ButtonStyle::ListItem: return tokens::radius::control;
        case ButtonStyle::Card:     return tokens::radius::pill;
        case ButtonStyle::Icon:     return tokens::radius::sm;
        case ButtonStyle::Primary:  return tokens::radius::control;
        case ButtonStyle::Ghost:    break;
    }
    return tokens::radius::control;
}

float Button::measureWidth(IPaintTarget& t) const {
    const auto ts = styleFor(tokens::type_body, Color{});
    float w = tokens::space::pad * 2.f;
    if (!icon.empty()) w += tokens::icon::size + (label.empty() ? 0.f : tokens::space::tight);
    if (!label.empty()) w += t.measureText(label, ts);
    if (!trailing.empty()) {
        w += tokens::space::gap + t.measureText(trailing, styleFor(tokens::type_small, Color{}));
    }
    return std::max(w, static_cast<float>(tokens::size::control));
}

bool Button::onPointer(const PointerEvent& e) {
    switch (e.phase) {
        case PointerPhase::Enter:
            if (!e.isTouch() && !tooltip.empty()) {
                OverlayHost::current().requestTooltip(this, tooltip, e.timestamp);
            }
            return true;
        case PointerPhase::Leave:
            OverlayHost::current().cancelTooltip(this);
            return true;
        case PointerPhase::Down:
            OverlayHost::current().cancelTooltip(this);
            return true;
        case PointerPhase::Up:
            // Only fire if released inside: dragging off a button and
            // letting go must cancel, which is what every platform does and
            // what users rely on to back out of a mis-press.
            if (hitLocal(e.pos) && enabled && onClick) onClick();
            return true;
        default:
            break;
    }
    return false;
}

bool Button::animating(double now) const {
    return hoverT_.active(now) || pressT_.active(now) || toggleT_.active(now);
}

void Button::onPaint(IPaintTarget& t, const PaintCtx& ctx) {
    const auto& pal = ctx.pal;
    const Rect b = localBounds();
    if (b.empty()) return;

    // Drive the transitions from the state the router maintains. Doing it in
    // paint rather than in the event handler means a state change that
    // happens without an event (a programmatic toggle) animates too.
    const_cast<Button*>(this)->hoverT_.animateTo(hovered && enabled ? 1.f : 0.f,
                                                 tokens::dur::hover, ctx.now);
    const_cast<Button*>(this)->pressT_.animateTo(pressed && enabled ? 1.f : 0.f,
                                                 tokens::dur::pressScale, ctx.now);
    const_cast<Button*>(this)->toggleT_.animateTo(toggled ? 1.f : 0.f,
                                                  tokens::dur::toolSwap, ctx.now);

    const float hv = hoverT_.value(ctx.now);
    const float pr = pressT_.value(ctx.now);
    const float on = toggleT_.value(ctx.now);
    const float dim = enabled ? 1.f : 0.45f;

    // Press scale, about the centre. 0.96 matches the prototype.
    const float s = lerp(1.f, 0.96f, pr);
    t.save();
    if (s != 1.f) {
        t.translate(b.w * 0.5f, b.h * 0.5f);
        t.scale(s, s);
        t.translate(-b.w * 0.5f, -b.h * 0.5f);
    }

    const Corners rr = Corners::all(radius());

    // --- background --------------------------------------------------------
    Color fg = pal.text;
    switch (style) {
        case ButtonStyle::Primary: {
            Color bg = pal.accent;
            if (hv > 0.f) bg = over(bg, Color{255, 255, 255, static_cast<std::uint8_t>(26 * hv)});
            drawCard(t, b, rr, bg, tokens::elevation::cardFlat(pal), dim);
            fg = Color{255, 255, 255, 255};
            break;
        }
        case ButtonStyle::Close: {
            // Hovers to a solid red with white glyph, like every Windows
            // title bar since 8.
            if (hv > 0.f) t.fillRoundedRect(b, rr, fade(tokens::fixed::closeHover, hv));
            fg = lerpColor(pal.muted, Color{255, 255, 255, 255}, hv);
            break;
        }
        case ButtonStyle::Window: {
            if (hv > 0.f) t.fillRoundedRect(b, rr, fade(pal.hover, hv));
            fg = pal.muted;
            break;
        }
        case ButtonStyle::Card: {
            drawCard(t, b, rr, pal.card, tokens::elevation::card(pal), dim);
            fg = pal.text;
            break;
        }
        case ButtonStyle::Segment:
        case ButtonStyle::Icon:
        case ButtonStyle::ListItem:
        case ButtonStyle::Ghost: {
            // Toggled wins over hover; hovering an already-active tool
            // should not wash it out.
            if (on > 0.f) t.fillRoundedRect(b, rr, fade(pal.accentSoft, on));
            if (hv > 0.f) t.fillRoundedRect(b, rr, fade(pal.hover, hv * (1.f - on * 0.55f)));
            fg = lerpColor(style == ButtonStyle::Icon ? pal.muted : pal.text,
                           pal.accentText, on);
            break;
        }
    }
    if (!enabled) fg = fade(fg, 0.45f);

    // --- content -----------------------------------------------------------
    const float pad = compact ? 2.f
                     : (style == ButtonStyle::ListItem) ? tokens::space::pad
                                                        : tokens::space::gap;
    const auto ts = styleFor(compact ? tokens::type_small : tokens::type_body, fg);
    const float iconSz = (style == ButtonStyle::Window || style == ButtonStyle::Close)
                             ? tokens::icon::sizeSmall
                             : tokens::icon::size;

    if (isWidthSample) {
        // Centre a horizontal stroke of the real weight. Rounded caps so a
        // 1px sample still reads as a drawn mark rather than a hairline
        // divider, which is what the panel's separators look like.
        const float inset = compact ? 6.f : 9.f;
        const float w = std::max(1.f, widthSample);
        const float y = b.y + b.h * 0.5f;
        const Rect bar{b.x + inset, y - w * 0.5f, b.w - inset * 2.f, w};
        t.fillRoundedRect(bar, Corners::all(std::min(w, bar.w) * 0.5f),
                          fade(sampleColor, dim));
    } else if (isSwatch) {
        // A chip, not a ring: the colour is the content. Inset enough that
        // the toggled background still reads as a selection halo around it.
        const float inset = compact ? 5.f : 7.f;
        Rect chip{b.x + inset, b.y + inset, b.w - inset * 2.f, b.h - inset * 2.f};
        const float r = std::min(chip.w, chip.h) * 0.5f;
        t.fillRoundedRect(chip, Corners::all(r), fade(swatchColor, dim));
        // A hairline keeps a white or near-background swatch visible.
        t.strokeRoundedRect(chip, Corners::all(r),
                            Color{0, 0, 0, static_cast<std::uint8_t>(30 * dim)}, 1.f);
    } else if (label.empty() && !icon.empty()) {
        drawIcon(t, icon, b, fg, tokens::icon::strokeWidth);
    } else {
        float x = pad;
        const bool centred = compact || style == ButtonStyle::Primary ||
                             style == ButtonStyle::Segment;
        const float textW = t.measureText(label, ts);
        const float chevSz = 14.f;
        float contentW = 0.f;
        if (!icon.empty()) contentW += iconSz + tokens::space::tight;
        contentW += textW;
        // Only hug when the whole group actually fits; a long title must
        // still be free to elide against the full width.
        const bool hug = trailingChevron && chevronHugsLabel &&
                         contentW + chevSz + tokens::space::tight +
                                 pad * 2.f <= b.w;
        if (hug) contentW += chevSz + tokens::space::tight;
        if (centred || hug) x = std::max(pad, (b.w - contentW) * 0.5f);

        if (!icon.empty()) {
            drawIcon(t, icon, Rect{x, (b.h - iconSz) * 0.5f, iconSz, iconSz}, fg,
                     tokens::icon::strokeWidth);
            x += iconSz + tokens::space::tight;
        }
        float labelEnd = x;
        if (!label.empty()) {
            const float avail = hug ? textW
                                    : b.w - x - pad -
                                          (trailing.empty()
                                               ? 0.f
                                               : tokens::space::gap * 3.f);
            labelEnd = x + std::max(0.f, avail);
            if (subtitle.empty()) {
                t.drawTextInBox(label, Rect{x, 0.f, std::max(0.f, avail), b.h}, ts,
                                folio::render::TextAlign::Left);
            } else {
                // Two-line row: label above, explanation below. Each half gets
                // its own box because drawTextInBox centres vertically.
                t.drawTextInBox(label, Rect{x, 3.f, std::max(0.f, avail), b.h * 0.5f},
                                styleFor(tokens::type_body, fg),
                                folio::render::TextAlign::Left);
                t.drawTextInBox(subtitle,
                                Rect{x, b.h * 0.5f - 3.f, std::max(0.f, avail),
                                     b.h * 0.5f},
                                styleFor(tokens::type_small, fade(fg, 0.6f)),
                                folio::render::TextAlign::Left);
            }
        }
        if (trailingChevron) {
            // 14px chevron, rotated by drawing the opposite glyph - the
            // prototype animates a transform, but a flip reads the same at
            // this size and keeps the icon path cache intact.
            const float cs = chevSz;
            const float cx = hug ? labelEnd + tokens::space::tight
                                 : b.w - cs - 7.f;
            drawIcon(t, chevronOpen ? "chevron-up" : "chevron-down",
                     Rect{cx, (b.h - cs) * 0.5f, cs, cs}, fg,
                     tokens::icon::strokeWidth);
        }
        if (!trailing.empty()) {
            t.drawTextInBox(trailing, Rect{0.f, 0.f, b.w - pad, b.h},
                            styleFor(tokens::type_small, fade(fg, 0.55f)),
                            folio::render::TextAlign::Right);
        }
    }

    if (showsDirtyDot) {
        t.fillCircle(Point{b.w - 7.f, 7.f}, 3.f, tokens::fixed::dirtyDot);
    }

    if (focused && style != ButtonStyle::Window && style != ButtonStyle::Close) {
        focusRing(t, b, rr, pal.accent, 0.9f);
    }

    t.restore();
}

}  // namespace folio::ui
