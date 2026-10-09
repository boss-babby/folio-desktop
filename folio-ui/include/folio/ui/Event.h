// folio-ui/include/folio/ui/Event.h
//
// Input events, in the UI's own vocabulary.
//
// These types are deliberately NOT SDL, Qt or browser events. The shell layer
// translates whatever the platform gives it into these, which is what lets the
// same widget tree run under SDL3 on a desktop and under the streaming shell
// in a headless sandbox without a single #ifdef in a component.

#pragma once

#include "folio/render/IPaintTarget.h"

#include <cstdint>
#include <string>

namespace folio::ui {

using folio::render::Point;
using folio::render::Rect;

enum class PointerPhase : std::uint8_t {
    Move,
    Down,
    Up,
    /// Synthesised by the router when the pointer enters/leaves a widget, so
    /// components never have to compare rectangles themselves.
    Enter,
    Leave,
    Wheel,
    /// Pointer left the window entirely; clears all hover state.
    Cancel,
};

enum class MouseButton : std::uint8_t { None = 0, Left = 1, Right = 2, Middle = 4 };

/// What is driving the pointer.
///
/// This matters far more than it looks. A finger and a mouse want opposite
/// behaviour from the same drag on a page: the mouse selects text, the
/// finger pans - because a finger has no hover state to show you what it
/// would select, and panning is what every other reader does. A pen wants
/// a third behaviour again: it draws, whatever mode the toolbar is in,
/// because that is why you picked up a pen.
enum class PointerType : std::uint8_t { Mouse, Touch, Pen };

/// Identifies one finger across a multi-touch sequence. A mouse is always
/// id 0; touches get whatever id the platform assigns, stable from down to
/// up. Needed to tell "two fingers moved" from "one finger teleported".
using PointerId = std::uint32_t;

struct PointerEvent {
    PointerPhase phase = PointerPhase::Move;
    /// Position in the receiving widget's local coordinates. The router
    /// rewrites this as it descends, so a component never needs to know where
    /// it sits on screen.
    Point pos{};
    /// Position in window coordinates, for things that must position a
    /// popover or tooltip.
    Point windowPos{};
    MouseButton button = MouseButton::None;
    std::uint8_t buttonsDown = 0;
    float wheelX = 0.f;
    float wheelY = 0.f;
    std::uint32_t modifiers = 0;
    double timestamp = 0.0;
    int clickCount = 1;

    PointerType pointerType = PointerType::Mouse;
    PointerId pointerId = 0;
    /// 0..1. Pens report real pressure; mouse and touch report 1 when down
    /// and 0 when up, so a width curve can read this unconditionally.
    float pressure = 1.f;
    /// Pen tilt in degrees from vertical, -90..90. Zero for everything else.
    float tiltX = 0.f;
    float tiltY = 0.f;

    [[nodiscard]] bool isTouch() const noexcept {
        return pointerType == PointerType::Touch;
    }
    [[nodiscard]] bool isPen() const noexcept {
        return pointerType == PointerType::Pen;
    }
};

enum Modifier : std::uint32_t {
    ModShift = 1u << 0,
    ModCtrl  = 1u << 1,
    ModAlt   = 1u << 2,
    ModMeta  = 1u << 3,
};

/// Physical keys the UI reacts to. Deliberately a small enum rather than
/// platform scancodes: the shells map into it, and adding a key is a
/// one-line change in each shell instead of a scattered constant hunt.
enum class Key : std::uint16_t {
    Unknown = 0,
    Escape, Enter, Tab, Backspace, Delete, Space,
    Left, Right, Up, Down, Home, End, PageUp, PageDown,
    Plus, Minus, Digit0,
    A, C, F, N, O, P, S, T, W, Y, Z,
};

struct KeyEvent {
    Key key = Key::Unknown;
    bool pressed = true;
    bool repeat = false;
    std::uint32_t modifiers = 0;
    /// Committed text for this key, if any (already IME-composed by the
    /// shell). Empty for pure navigation keys.
    std::string text;
};

}  // namespace folio::ui
