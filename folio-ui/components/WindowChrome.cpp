#include "components/WindowChrome.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "folio/ui/Draw.h"
#include "folio/ui/Theme.h"
#include "folio/ui/Tokens.h"
#include "components/Button.h"

namespace folio::ui {

using folio::render::Color;
using folio::render::Corners;
using folio::render::Point;
using folio::render::Rect;

namespace {

// Traffic-light metrics, straight off a Sonoma title bar.
constexpr float kLightD = 12.f;    // diameter
constexpr float kLightGap = 8.f;   // edge-to-edge
constexpr float kLightLead = 20.f; // window edge to first light centre-ish

constexpr Color kLightClose{0xFF, 0x5F, 0x57, 0xFF};
constexpr Color kLightMin{0xFE, 0xBC, 0x2E, 0xFF};
constexpr Color kLightMax{0x28, 0xC8, 0x40, 0xFF};
// Unfocused windows grey all three out; we approximate "focused" as always
// true until the shell reports focus.
constexpr Color kLightGlyph{0x00, 0x00, 0x00, 0x99};

}  // namespace

WindowPlatform hostWindowPlatform() {
    if (const char* e = std::getenv("FOLIO_WINDOW_CHROME")) {
        if (std::strcmp(e, "windows") == 0) return WindowPlatform::Windows;
        if (std::strcmp(e, "linux") == 0) return WindowPlatform::Linux;
        if (std::strcmp(e, "macos") == 0) return WindowPlatform::MacOS;
    }
#if defined(__APPLE__)
    return WindowPlatform::MacOS;
#elif defined(_WIN32)
    return WindowPlatform::Windows;
#else
    return WindowPlatform::Linux;
#endif
}

WindowControls::WindowControls(WindowPlatform plat) : plat_(plat) {
    debugName = "window.controls";
    if (plat_ == WindowPlatform::MacOS) {
        // Self-painted, so this widget must take pointer events itself -
        // without this the cluster never learns it is hovered and the
        // glyphs never appear.
        acceptsPointer = true;
        return;
    }

    min_ = emplace<Button>("", "minimise", ButtonStyle::Window);
    min_->debugName = "win.min";
    min_->onClick = [this] { if (onMinimise) onMinimise(); };

    max_ = emplace<Button>("", "maximise", ButtonStyle::Window);
    max_->debugName = "win.max";
    max_->onClick = [this] { if (onMaximise) onMaximise(); };

    close_ = emplace<Button>("", "close", ButtonStyle::Close);
    close_->debugName = "win.close";
    close_->onClick = [this] { if (onClose) onClose(); };
}

float WindowControls::preferredWidth() const {
    switch (plat_) {
        case WindowPlatform::MacOS:
            return kLightLead + 3.f * kLightD + 2.f * kLightGap + 8.f;
        case WindowPlatform::Linux:
            return 3.f * 32.f + 2.f * 6.f + 12.f;
        case WindowPlatform::Windows:
        default:
            return 3.f * tokens::size::windowControl::width;
    }
}

void WindowControls::onLayout() {
    if (plat_ == WindowPlatform::MacOS) return;

    const float h = frame.h;
    if (plat_ == WindowPlatform::Windows) {
        // Flush to the top-right corner, no gaps, full title-bar height --
        // the glyphs sit in a continuous strip you can throw the mouse at.
        const float w = tokens::size::windowControl::width;
        float x = frame.w - 3.f * w;
        for (Button* b : {min_, max_, close_}) {
            b->setFrame(x, 0.f, w, h);
            x += w;
        }
    } else {
        // GNOME: round buttons, inset from the edge, small gutters between.
        const float d = 32.f;
        const float y = (h - d) * 0.5f;
        float x = frame.w - (3.f * d + 2.f * 6.f) - 6.f;
        for (Button* b : {min_, max_, close_}) {
            b->setFrame(x, y, d, d);
            x += d + 6.f;
        }
    }
    if (max_) max_->icon = maximised ? "restore" : "maximise";
}

int WindowControls::trafficHit(Point p) const {
    if (plat_ != WindowPlatform::MacOS) return -1;
    const float cy = frame.h * 0.5f;
    for (int i = 0; i < 3; ++i) {
        const float cx = kLightLead + static_cast<float>(i) * (kLightD + kLightGap) +
                         kLightD * 0.5f;
        const float dx = p.x - cx, dy = p.y - cy;
        // A slightly generous radius: the real ones are easier to hit than
        // 6px suggests because the whole cluster is one target.
        if (dx * dx + dy * dy <= 9.f * 9.f) return i;
    }
    return -1;
}

void WindowControls::paintTrafficLights(folio::render::IPaintTarget& t,
                                        const PaintCtx& ctx) {
    const float hv = clusterHover_.value(now());
    const float cy = frame.h * 0.5f;
    const float dark = tokens::darkness(ctx.pal);

    static constexpr Color kCols[3] = {kLightClose, kLightMin, kLightMax};
    for (int i = 0; i < 3; ++i) {
        const float cx = kLightLead + static_cast<float>(i) * (kLightD + kLightGap) +
                         kLightD * 0.5f;
        const Rect r{cx - kLightD * 0.5f, cy - kLightD * 0.5f, kLightD, kLightD};
        const Corners rr = Corners::all(kLightD * 0.5f);

        Color c = kCols[i];
        if (pressedLight_ == i) {
            // Pressed lights darken rather than lighten.
            c = Color{static_cast<std::uint8_t>(c.r * 0.78f),
                      static_cast<std::uint8_t>(c.g * 0.78f),
                      static_cast<std::uint8_t>(c.b * 0.78f), c.a};
        }
        t.fillRoundedRect(r, rr, c);
        // Hairline inner edge: on a light title bar the yellow needs it to
        // read as a disc rather than a smudge.
        t.strokeRoundedRect(r, rr,
                            Color{0, 0, 0, static_cast<std::uint8_t>(30 * (1.f - dark))},
                            1.f);

        if (hv <= 0.01f) continue;
        const Color g = fade(kLightGlyph, hv);
        const float s = 3.2f;
        switch (i) {
            case 0:  // close: an x
                t.drawLine({cx - s, cy - s}, {cx + s, cy + s}, g, 1.4f);
                t.drawLine({cx + s, cy - s}, {cx - s, cy + s}, g, 1.4f);
                break;
            case 1:  // minimise: a bar
                t.drawLine({cx - s - 0.5f, cy}, {cx + s + 0.5f, cy}, g, 1.4f);
                break;
            default: {  // maximise: two facing triangles (or arrows when zoomed)
                t.drawLine({cx - s, cy + s}, {cx + s, cy - s}, g, 1.4f);
                t.drawLine({cx - s, cy + s}, {cx - s + 1.f, cy + s - 3.4f}, g, 1.4f);
                t.drawLine({cx + s, cy - s}, {cx + s - 1.f, cy - s + 3.4f}, g, 1.4f);
                break;
            }
        }
    }
}

void WindowControls::onPaint(folio::render::IPaintTarget& t, const PaintCtx& ctx) {
    if (plat_ == WindowPlatform::MacOS) paintTrafficLights(t, ctx);
}

bool WindowControls::onPointer(const PointerEvent& e) {
    if (plat_ != WindowPlatform::MacOS) return false;  // children handle it

    // Enter/Leave are what the router synthesises; a bare Move is only
    // delivered while a button is held, so hovering has to key off these.
    if (e.phase == PointerPhase::Enter || e.phase == PointerPhase::Move)
        clusterHover_.animateTo(1.f, 120, now());
    else if (e.phase == PointerPhase::Leave)
        clusterHover_.animateTo(0.f, 120, now());

    const int hit = trafficHit(e.pos);
    if (e.phase == PointerPhase::Down && hit >= 0) {
        pressedLight_ = hit;
        return true;
    }
    if (e.phase == PointerPhase::Up && pressedLight_ >= 0) {
        const int was = pressedLight_;
        pressedLight_ = -1;
        if (was == hit) {
            if (was == 0 && onClose) onClose();
            if (was == 1 && onMinimise) onMinimise();
            if (was == 2 && onMaximise) onMaximise();
        }
        return true;
    }
    return hit >= 0;
}

}  // namespace folio::ui
