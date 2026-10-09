#include "ShadowCache.h"

#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSurface.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace folio::render::skia {
namespace {

/// Quantise so near-identical requests share a tile. Without this an
/// animated elevation would mint a tile per frame - precisely the cost the
/// cache exists to remove. A quarter pixel of sigma is imperceptible in a
/// soft gradient.
constexpr float kQuantum = 0.25f;

int q(float v) { return static_cast<int>(std::lround(v / kQuantum)); }

/// Distance beyond which a Gaussian contributes less than 1/255.
int reachOf(float sigma) { return static_cast<int>(std::ceil(3.f * sigma)); }

struct Key {
    int radius, sigma, shapeW, shapeH;
    auto operator<=>(const Key&) const = default;
};

/// Beyond this a tile is bigger than the shape it serves and the cache is
/// pointless; the caller blurs directly instead.
constexpr int kMaxSide = 1024;

/// Guards against a resizing window interning a tile per pixel of width.
constexpr std::size_t kMaxEntries = 96;

}  // namespace

struct ShadowCache::Impl {
    mutable std::mutex mu;
    std::map<Key, Tile> tiles;
    Stats stats;
};

ShadowCache::ShadowCache() : impl_(new Impl) {}
ShadowCache::~ShadowCache() { delete impl_; }

ShadowCache& ShadowCache::instance() {
    static ShadowCache c;
    return c;
}

const ShadowCache::Tile* ShadowCache::get(float radius, float sigma, float dstW,
                                          float dstH) {
    if (sigma <= 0.f || dstW <= 0.f || dstH <= 0.f) return nullptr;

    const int reach = reachOf(sigma);
    const int r = static_cast<int>(std::ceil(std::max(0.f, radius)));

    // Smallest shape whose centre pixel is a fully-developed edge profile:
    // 3 sigma of clearance from the corner on each side.
    const int ideal = 2 * (r + reach) + 1;

    // If the destination is smaller than that in an axis, the real shadow's
    // opposite edges interact and no stretch can fake it - so build the
    // tile at the destination's own size in that axis and stretch only the
    // other one.
    const int shapeW = (dstW >= static_cast<float>(ideal))
                           ? ideal
                           : static_cast<int>(std::lround(dstW));
    const int shapeH = (dstH >= static_cast<float>(ideal))
                           ? ideal
                           : static_cast<int>(std::lround(dstH));

    // The two corner blocks plus the stretchable pixel must fit.
    if (shapeW < 2 * r + 1 || shapeH < 2 * r + 1) return nullptr;

    const Key key{q(radius), q(sigma), shapeW, shapeH};
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (auto it = impl_->tiles.find(key); it != impl_->tiles.end()) {
        ++impl_->stats.hits;
        return it->second.image ? &it->second : nullptr;
    }

    const int pad = reach + 1;
    const int sideW = shapeW + pad * 2;
    const int sideH = shapeH + pad * 2;
    if (sideW > kMaxSide || sideH > kMaxSide) return nullptr;
    ++impl_->stats.misses;

    const SkImageInfo info =
        SkImageInfo::Make(sideW, sideH, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkSurface> surf = SkSurfaces::Raster(info);
    if (!surf) return nullptr;

    SkCanvas* c = surf->getCanvas();
    c->clear(SK_ColorTRANSPARENT);

    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(SK_ColorBLACK);  // tinted later by the caller's colour filter
    p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, sigma));
    c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(static_cast<float>(pad),
                                                      static_cast<float>(pad),
                                                      static_cast<float>(shapeW),
                                                      static_cast<float>(shapeH)),
                                     static_cast<float>(r), static_cast<float>(r)),
                 p);

    Tile tile;
    tile.image = surf->makeImageSnapshot();
    tile.pad = pad;
    // The stretchable pixel sits at the shape's midpoint, which is the one
    // place guaranteed to be a saturated edge profile in both axes.
    tile.cornerX = pad + shapeW / 2;
    tile.cornerY = pad + shapeH / 2;

    if (impl_->tiles.size() >= kMaxEntries) impl_->tiles.clear();
    impl_->stats.bytes += static_cast<std::size_t>(sideW) *
                          static_cast<std::size_t>(sideH) * 4u;

    auto [it, _] = impl_->tiles.emplace(key, std::move(tile));
    return it->second.image ? &it->second : nullptr;
}

ShadowCache::Stats ShadowCache::stats() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    Stats s = impl_->stats;
    s.entries = impl_->tiles.size();
    return s;
}

void ShadowCache::clear() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->tiles.clear();
    impl_->stats = {};
}

}  // namespace folio::render::skia
