// folio-ui/components/Controls.h
//
// Labels, dividers, segmented controls, text fields, toggles and sliders.

#pragma once

#include "folio/ui/Animation.h"
#include "folio/ui/Draw.h"
#include "folio/ui/Widget.h"

#include <functional>
#include <string>
#include <vector>

namespace folio::ui {

/// Which palette role a piece of text uses. Resolving this at paint time
/// rather than storing a Color is what lets labels follow the theme fade.
enum class TextRole : std::uint8_t { Primary, Muted, Accent, Danger, OnAccent };

[[nodiscard]] Color resolveRole(TextRole r, const tokens::Palette& p);

/// Static text. Not interactive, so it does not accept the pointer and
/// cannot steal hover from a card underneath it.
class Label : public Widget {
public:
    Label(std::string text, tokens::TypeScale scale, TextRole role = TextRole::Primary);

    std::string text;
    tokens::TypeScale scale;
    TextRole role = TextRole::Primary;
    folio::render::TextAlign align = folio::render::TextAlign::Left;
    /// Applies the token's `uppercase` transform. Section headers use it.
    bool uppercase = false;

    [[nodiscard]] float intrinsicWidth(IPaintTarget& t) const;
    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
};

/// A 1 px rule. `vertical` matches the prototype's `.v` toolbar separator.
class Divider : public Widget {
public:
    explicit Divider(bool vertical = true) : vertical_(vertical) {}
    /// Inset from each end, so a toolbar separator does not touch the chrome.
    float inset = 8.f;
    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;

private:
    bool vertical_;
};

// ---------------------------------------------------------------------------

/// A row of mutually exclusive options with a sliding selection pill.
///
/// The pill is a single animated rect rather than a per-segment background
/// fade. That is what produces the "the selection slid over" feel of the
/// prototype instead of "one lit up as another went dark".
class SegmentedControl : public Widget {
public:
    struct Item {
        std::string label;
        std::string icon;
        std::string tooltip;
    };

    explicit SegmentedControl(std::vector<Item> items, int selected = 0);

    std::function<void(int)> onChange;

    [[nodiscard]] int selected() const noexcept { return selected_; }
    void setSelected(int i, double now);
    [[nodiscard]] std::size_t count() const noexcept { return items_.size(); }
    [[nodiscard]] float intrinsicWidth(IPaintTarget& t) const;

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;

private:
    [[nodiscard]] Rect cellRect(std::size_t i) const;
    [[nodiscard]] int indexAt(Point local) const;

    std::vector<Item> items_;
    int selected_ = 0;
    int hoverIndex_ = -1;
    AnimatedF pillX_{0.f};
    AnimatedF pillW_{0.f};
    bool pillInitialised_ = false;
};

// ---------------------------------------------------------------------------

/// Single-line editable text.
///
/// SCOPE: this is a chrome text field - a search box and a page-number box.
/// It handles ASCII/UTF-8 insertion, backspace, arrows, home/end and a
/// blinking caret. It deliberately does NOT implement selection ranges,
/// bidi cursor movement or IME composition. Those belong to the platform
/// text input protocol (plan Appendix C lists IME as one of the eight things
/// that vary per platform), and faking them here would produce something
/// that looks finished and breaks on the first Japanese user.
class TextField : public Widget {
public:
    explicit TextField(std::string placeholder = {});

    std::string text;
    std::string placeholder;
    std::string leadingIcon;
    std::function<void(const std::string&)> onChanged;
    std::function<void()> onSubmit;
    /// Focus arrived (true) or left (false). Used by the page field to
    /// swap between "3 / 24" and an editable "3".
    std::function<void(bool)> onFocus;
    folio::render::TextAlign align = folio::render::TextAlign::Left;
    /// Draw no chrome until hovered or focused. A field that sits inside
    /// an existing surface - the page number in the zoom pill - must look
    /// like the label it replaced until the pointer says otherwise.
    bool flat = false;

    void onFocusChanged(bool gained) override;
    /// Put the caret after the last character - what "click to edit this
    /// value" should do, so typing appends rather than inserting mid-word.
    void setCaretToEnd() { caret_ = text.size(); }

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;

private:
    std::size_t caret_ = 0;
    double caretReset_ = 0.0;
    AnimatedF focusT_{0.f};
};

// ---------------------------------------------------------------------------

/// iOS-style switch with the prototype's 300 ms knob travel.
class Toggle : public Widget {
public:
    explicit Toggle(bool on = false);

    std::function<void(bool)> onChange;
    [[nodiscard]] bool isOn() const noexcept { return on_; }
    void setOn(bool v, double now);

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;

private:
    bool on_ = false;
    AnimatedF knob_{0.f};
};

// ---------------------------------------------------------------------------

/// Horizontal value slider (the grid-size control).
class Slider : public Widget {
public:
    Slider(float minV, float maxV, float value);

    std::function<void(float)> onChange;
    [[nodiscard]] float value() const noexcept { return value_; }
    void setValue(float v);

    void onPaint(IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;
    [[nodiscard]] bool animating(double now) const override;

private:
    [[nodiscard]] float normalised() const;
    void setFromX(float x);

    float min_, max_, value_;
    AnimatedF hoverT_{0.f};
};

}  // namespace folio::ui
