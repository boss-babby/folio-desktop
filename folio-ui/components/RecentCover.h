// A bounded first-page raster for a real recent PDF. Shared with the card;
// immutable pixels can be reused by the paint backend across frames.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <algorithm>

#include "folio/render/IPaintTarget.h"

namespace folio::ui {

struct RecentCover {
    int width = 0, height = 0;
    std::uint64_t imageKey = 0;
    std::vector<std::byte> pixels; // tightly packed BGRA8888, top-down
};

/// "Cover" fit with a top-aligned viewport. A portrait PDF's first page
/// fills the width and is clipped below the card; wide pages crop evenly at
/// the sides. The PDF is NEVER distorted into a card's aspect ratio.
[[nodiscard]] inline folio::render::Rect recentCoverDestination(
    folio::render::Rect crop, int width, int height) {
    if (width <= 0 || height <= 0 || crop.empty()) return {};
    const float scale = std::max(crop.w / static_cast<float>(width),
                                 crop.h / static_cast<float>(height));
    const float w = static_cast<float>(width) * scale;
    const float h = static_cast<float>(height) * scale;
    return {crop.x + (crop.w - w) * .5f, crop.y, w, h};
}

} // namespace folio::ui
