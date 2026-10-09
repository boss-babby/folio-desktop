// folio-ui/components/Button.h
//
// Every pressable thing in the prototype, as one widget with eight skins.
//
// The prototype's CSS has a `.b` base class and seven modifiers. Modelling
// that as eight C++ classes would duplicate the hover/press/toggle logic
// eight times; modelling it as one class with a style enum keeps the
// behaviour in one place, which matters because the *behaviour* is what has
// to be identical (250 ms hover fade, 150 ms press scale) while the paint
// differs only in which token it reaches for.

#pragma once

#include "folio/ui/Animation.h"
#include "folio/ui/Draw.h"
#include "folio/ui/Widget.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace folio::ui {

enum class ButtonStyle : std::uint8_t {
    /// Transparent until hovered. The default for toolbar controls.
    Ghost,
    /// Accent-filled call to action ("Open file").
    Primary,
    /// Square, icon-only, used throughout the command bar.
    Icon,
    /// Full-width left-aligned icon + label, the welcome sidebar rows.
    ListItem,
    /// Title-bar minimise/maximise, 46x44, no radius.
    Window,
    /// Title-bar close: same as Window but hovers red.
    Close,
    /// A cell inside a SegmentedControl; corners handled by the parent.
    Segment,
    /// Floating card-styled button (the fullscreen-exit pill).
    Card,
};

class Button : public Widget {
public:
    Button(std::string label, std::string icon, ButtonStyle style = ButtonStyle::Ghost);

    /// Convenience for the common icon-only case.
    static std::unique_ptr<Button> iconOnly(std::string icon, std::string tooltip = {});

    std::function<void()> onClick;

    std::string label;
    std::string icon;
    ButtonStyle style = ButtonStyle::Ghost;
    std::string tooltip;
    /// Latched state, e.g. a tool that is currently selected. Painted with
    /// the accent-soft background the prototype calls `.on`.
    bool toggled = false;
    /// Small amber dot in the corner, for "has unsaved changes".
    bool showsDirtyDot = false;
    /// Optional right-aligned trailing text (keyboard hint in list items).
    std::string trailing;
    /// Second line under the label, for menu rows that need explaining.
    /// Only drawn by ButtonStyle::ListItem.
    std::string subtitle;
    /// 12px label with minimal side padding and centred, for the narrow
    /// four-up action rows the prototype puts at the foot of a panel
    /// (`font-size:12.5px; flex:1; padding:0`). Without it a 60px cell
    /// elides "Rotate" to "Rota...".
    bool compact = false;
    /// Draw a small chevron on the trailing edge, marking the button as a
    /// dropdown (the prototype's `svg.cv`). `chevronOpen` rotates it 180
    /// degrees while the menu is showing.
    /// Paint a filled colour chip instead of an icon. Used by the ink
    /// swatches in the tool popovers and by the command bar's colour
    /// control, which has to show the current ink at a glance.
    bool isSwatch = false;
    folio::render::Color swatchColor{0, 0, 0, 255};

    /// Paint a sample stroke of `widthSample` device pixels instead of a
    /// label. "Fine / Medium / Thick" are three words that mean nothing
    /// until you have drawn with them; a line of the actual weight is the
    /// control explaining itself.
    bool isWidthSample = false;
    float widthSample = 2.f;
    folio::render::Color sampleColor{30, 32, 44, 255};

    bool trailingChevron = false;
    /// Draw the chevron immediately after the label, and centre the pair,
    /// instead of pinning the chevron to the right edge. A title button
    /// that is wider than its text otherwise strands its chevron across
    /// the bar, where it reads as a separate control. Falls back to the
    /// pinned behaviour when the label is too long to fit, so a long name
    /// still elides against the full width.
    bool chevronHugsLabel = false;
    bool chevronOpen = false;

    /// Intrinsic width for the label at the current style, so toolbars can
    /// lay themselves out without hard-coded numbers.
    [[nodiscard]] float measureWidth(IPaintTarget& t) const;

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;

private:
    [[nodiscard]] float radius() const;

    AnimatedF hoverT_{0.f};
    AnimatedF pressT_{0.f};
    AnimatedF toggleT_{0.f};
};

}  // namespace folio::ui
