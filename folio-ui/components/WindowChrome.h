// Platform-specific window controls (minimise / maximise / close).
//
// Folio draws its own title bar on every desktop target, so the controls are
// ours to draw too -- but they must *feel* native. The placement and shape of
// these three buttons is the single most recognisable piece of platform
// identity in a client-decorated window, and getting it wrong is the kind of
// thing that makes an app feel foreign on day one.
//
// Three layouts are supported:
//
//   Windows   right-aligned, 46x44 square hit targets, hairline glyphs,
//             close hovers to the familiar solid red.
//   Linux     right-aligned, circular ghost buttons with a little breathing
//             room, in the GNOME/Adwaita idiom.
//   macOS     LEFT-aligned traffic lights: 12px circles, 8px apart, glyphs
//             that only appear while the pointer is over the cluster.
//
// Screens must not assume a side. Ask `controlsOnLeft()` and `preferredWidth()`
// and lay the title bar out around the answer; that is what keeps the macOS
// build from needing its own copy of every screen.

#pragma once

#include <functional>

#include "folio/ui/Animation.h"
#include "folio/ui/Widget.h"

namespace folio::ui {

class Button;

enum class WindowPlatform { Windows, Linux, MacOS };

// The platform this binary was compiled for, overridable at runtime with
// FOLIO_WINDOW_CHROME=windows|linux|macos so one build can be checked against
// all three looks (and so screenshots for the other platforms can be taken
// without cross-compiling).
WindowPlatform hostWindowPlatform();

class WindowControls : public Widget {
public:
    explicit WindowControls(WindowPlatform plat = hostWindowPlatform());

    // Width the cluster needs; the caller reserves this much of the title bar.
    float preferredWidth() const;
    // True when the cluster belongs at the leading edge (macOS).
    bool controlsOnLeft() const { return plat_ == WindowPlatform::MacOS; }
    WindowPlatform platform() const { return plat_; }

    std::function<void()> onMinimise, onMaximise, onClose;
    bool maximised = false;

    void onLayout() override;
    void onPaint(folio::render::IPaintTarget& t, const PaintCtx& ctx) override;
    bool onPointer(const PointerEvent& e) override;

private:
    // Traffic lights are painted here rather than delegated to Button: they
    // share one hover state (hovering any light reveals all three glyphs),
    // which no per-button style can express.
    void paintTrafficLights(folio::render::IPaintTarget& t, const PaintCtx& ctx);
    int trafficHit(folio::render::Point p) const;

    WindowPlatform plat_;
    Button* min_ = nullptr;
    Button* max_ = nullptr;
    Button* close_ = nullptr;
    Animated<float> clusterHover_{0.f};
    int pressedLight_ = -1;
};

}  // namespace folio::ui
