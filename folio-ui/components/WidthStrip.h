// Folio - a strip of stroke-weight choices, drawn as the weights themselves.
//
// This is a drop-in for the SegmentedControl that used to offer "Fine /
// Medium / Thick": it exposes the same selected() / setSelected() / onChange
// surface, so the twelve call sites around it did not have to change. What
// it does differently is show the actual line, which is the only honest way
// to present a stroke weight - three adjectives do not tell you whether
// "Thick" is 4pt or 12pt, and the answer differs per tool.

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include "components/Button.h"
#include "folio/ui/Widget.h"

namespace folio::ui {

class WidthStrip final : public Widget {
public:
    /// `samples` are display thicknesses in logical pixels, not the page
    /// units stored on an annotation - the caller owns that mapping.
    WidthStrip(std::vector<float> samples, int defaultIndex) {
        samples_ = std::move(samples);
        selected_ = std::clamp(defaultIndex, 0,
                               static_cast<int>(samples_.size()) - 1);
        for (std::size_t i = 0; i < samples_.size(); ++i) {
            auto* b = emplace<Button>("", "", ButtonStyle::Ghost);
            b->isWidthSample = true;
            b->widthSample = samples_[i];
            b->toggled = (static_cast<int>(i) == selected_);
            const int idx = static_cast<int>(i);
            b->onClick = [this, idx] {
                setSelected(idx, now());
                if (onChange) onChange(idx);
            };
            btns_.push_back(b);
        }
    }

    std::function<void(int)> onChange;

    [[nodiscard]] int selected() const noexcept { return selected_; }

    /// Mirrors SegmentedControl::setSelected, including the detail that
    /// bit the option system in round 12: this does NOT fire onChange.
    /// Programmatic refresh must not look like a user edit, or selecting
    /// an annotation pushes an undo entry for a change nobody made.
    void setSelected(int i, double /*now*/) {
        selected_ = std::clamp(i, 0, static_cast<int>(btns_.size()) - 1);
        for (std::size_t k = 0; k < btns_.size(); ++k)
            btns_[k]->toggled = (static_cast<int>(k) == selected_);
    }

    /// Tint the samples so they read against the current tool's ink.
    void setSampleColor(folio::render::Color c) {
        for (Button* b : btns_) b->sampleColor = c;
    }

    void onLayout() override {
        if (btns_.empty()) return;
        const float n = static_cast<float>(btns_.size());
        const float gap = 2.f;
        const float w = (frame.w - gap * (n - 1.f)) / n;
        for (std::size_t i = 0; i < btns_.size(); ++i) {
            btns_[i]->setFrame(static_cast<float>(i) * (w + gap), 0.f, w,
                               frame.h);
        }
    }

private:
    std::vector<float> samples_;
    std::vector<Button*> btns_;
    int selected_ = 0;
};

}  // namespace folio::ui
