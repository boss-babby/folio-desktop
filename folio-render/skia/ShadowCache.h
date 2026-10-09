#pragma once

#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"

#include <cstddef>

namespace folio::render::skia {

/// Cache of pre-blurred rounded-rectangle shadow tiles, drawn with
/// `SkCanvas::drawImageNine`.
///
/// WHY
/// ---
/// Measured on the reader screen at 1440x900: a frame took 30.1 ms, and
/// disabling elevation entirely (FOLIO_NO_SHADOWS=1) took it to 12.2 ms.
/// Nearly 18 ms - three 60 FPS frame budgets - went on re-running a
/// Gaussian blur over shapes that had not changed. A card's shadow is
/// recomputed from scratch every frame purely because the frame is being
/// redrawn.
///
/// THE IDEA
/// --------
/// A blurred rounded rectangle is four fixed corner profiles joined by four
/// edge profiles that are constant along their length. So the *size* of the
/// rectangle does not change what the shadow looks like - only the corner
/// radius and the blur do. Render the shadow once at the smallest size that
/// contains all four corners, then stretch the middle row and column to any
/// target size. That is exactly a nine-patch.
///
/// One tile therefore serves the 780x1010 page shadow and every other
/// shadow sharing its radius and sigma, and a hover animation that only
/// changes the shadow's opacity costs nothing extra.
///
/// SIZING (the part that is easy to get wrong)
/// -------------------------------------------
/// The stretched middle must be a saturated, fully-developed edge profile.
/// If the tile's shape is small relative to the blur, the blur from opposite
/// edges overlaps and the centre never reaches full opacity - a 5x5 square
/// blurred at sigma 20 peaks at about 1/255 and the shadow vanishes. This
/// was the first implementation's bug.
///
/// So the shape inside the tile is sized to `2 * (radius + 3*sigma) + 1`:
/// every edge is at least 3 sigma - 99.7% of the Gaussian - away from the
/// centre pixel and from the opposite edge.
///
/// By the same argument, a *destination* smaller than that genuinely has
/// interacting edges, and no stretched tile can reproduce it. For those the
/// tile is built at the destination's own size in the offending axis and
/// only the other axis stretches; wide, short chrome such as the command
/// bar takes this path. If even the corners do not fit, the caller is told
/// to fall back to a direct blur.
///
/// FORMAT
/// ------
/// Tiles are premultiplied BGRA holding black at varying alpha, tinted by
/// the caller through a kSrcIn colour filter. kAlpha_8 would be a quarter
/// of the memory and was tried first, but an alpha-only SkImage drawn via
/// drawImageNine renders nothing on the CPU backend.
class ShadowCache {
public:
    struct Tile {
        sk_sp<SkImage> image;
        int pad = 0;      ///< blur margin outside the shape, in pixels
        int cornerX = 0;  ///< x of the 1px stretchable column
        int cornerY = 0;  ///< y of the 1px stretchable row
    };

    struct Stats {
        std::size_t hits = 0, misses = 0, entries = 0, bytes = 0;
    };

    static ShadowCache& instance();

    /// Fetch (or build) a tile for a shadow of `radius` and `sigma` covering
    /// a `dstW` x `dstH` rectangle. `radius` must already include any CSS
    /// spread, and `dstW`/`dstH` must be the spread-expanded size.
    ///
    /// Returns nullptr when the destination is too small to be expressed as
    /// a nine-patch; the caller should blur directly in that case.
    const Tile* get(float radius, float sigma, float dstW, float dstH);

    Stats stats() const;
    void clear();

    ShadowCache(const ShadowCache&) = delete;
    ShadowCache& operator=(const ShadowCache&) = delete;

private:
    ShadowCache();
    ~ShadowCache();
    struct Impl;
    Impl* impl_;
};

}  // namespace folio::render::skia
