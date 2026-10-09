// folio-ui/shell/FrameEncoder.h
//
// Turns a rendered Skia surface into bytes a browser can display.
//
// This exists only for the streaming shell. A native shell hands the
// surface straight to the window system; nothing is encoded. The cost
// measured here (encode milliseconds) is therefore sandbox/remote overhead
// and must be reported separately from render cost, or the numbers will
// libel the renderer.

#pragma once

#include "include/core/SkSurface.h"

#include <cstdint>
#include <vector>

namespace folio::shell {

enum class Codec : std::uint8_t { Jpeg, Png, Webp };

class FrameEncoder {
public:
    /// Encode the surface's pixels. Returns an empty vector on failure.
    ///
    /// `quality` is 1..100 for JPEG/WebP and ignored for PNG.
    [[nodiscard]] static std::vector<std::uint8_t> encode(SkSurface& surface,
                                                          Codec codec, int quality);

    /// Cheap per-frame change detection, so an idle UI sends nothing.
    ///
    /// FNV-1a over every Nth row rather than every byte: a full 1440x900
    /// BGRA hash is ~5 MB of reads per frame, which is a measurable slice of
    /// a 16 ms budget. Sampling every 4th row keeps it under a millisecond
    /// and still catches any change large enough for a human to see. A
    /// change confined entirely to the skipped rows would be missed, so the
    /// caller must also honour the app's own needsFrame() signal rather than
    /// relying on the hash alone.
    [[nodiscard]] static std::uint64_t quickHash(SkSurface& surface);
};

}  // namespace folio::shell
