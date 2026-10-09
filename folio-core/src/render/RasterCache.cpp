// folio/core/render/RasterCache.cpp  --  work package C2

#include "folio/core/render/RasterCache.h"

#include <algorithm>
#include <cmath>

namespace folio::core {
namespace {

// --- policy constants ------------------------------------------------------
//
// These are the knobs that decide whether scrolling feels solid or feels like
// a web page from 2009. They are tuned against the three gestures that matter:
// flick-scroll (fast, shallow), page-back (one page, must always hit) and
// pinch-zoom (same page, many scale buckets in two seconds).

/// Pages either side of the focus that stay cheap to keep. Covers "read a
/// page, scroll back to check the previous one", which must never re-render.
constexpr double kNearPages = 2.0;

/// How fast value falls off with page distance beyond kNearPages.
constexpr double kDistanceFalloff = 0.55;

/// Freshness half-life in ticks (= composited frames). 600 frames is ten
/// seconds at 60 fps: long enough to survive a round trip to the search panel
/// and back, short enough that a tile abandoned a minute ago is cheap.
constexpr double kFreshnessHalfLife = 600.0;

/// Floor on freshness. Without it an old-but-close tile eventually scores
/// below a fresh-but-distant one and the cache starts evicting the page the
/// user has been reading for five minutes.
constexpr double kFreshnessFloor = 0.05;

/// Penalty for a tile rendered in a colour scheme that is no longer active.
/// Near-worthless: it cannot be shown, only re-rendered. Not zero, because
/// toggling dark mode twice is a real thing people do.
constexpr double kSchemeMismatch = 0.02;

/// Per-bucket penalty for the wrong zoom. Wrong-scale tiles still have value -
/// findFallback() draws them while the right ones render - so this decays
/// rather than cliffs.
constexpr double kScalePenaltyPerBucket = 0.30;

/// Class multipliers. A proxy is the blank-page insurance policy; a thumbnail
/// is cheap and conspicuous when missing.
constexpr double kProxyBonus = 64.0;
constexpr double kThumbBonus = 3.0;

[[nodiscard]] double pageDistanceValue(PageId key, PageId focus) noexcept {
    const auto a = static_cast<double>(key);
    const auto b = static_cast<double>(focus);
    const double d = std::fabs(a - b);
    if (d <= kNearPages) {
        return 1.0;
    }
    return 1.0 / (1.0 + kDistanceFalloff * (d - kNearPages));
}

}  // namespace

// ---------------------------------------------------------------------------

double RasterCache::retentionValue(const TileKey& key,
                                   TileClass cls,
                                   std::uint64_t ageTicks,
                                   const CacheFocus& focus) noexcept {
    double v = pageDistanceValue(key.pageId, focus.page);

    if (key.scheme != focus.scheme) {
        v *= kSchemeMismatch;
    }

    // Scale affinity. Proxies and thumbnails are deliberately exempt: they are
    // stored at a fixed small scale and are expected to differ from the
    // viewport's bucket, so penalising them here would evict the very things
    // that exist to cover a cache miss.
    if (cls == TileClass::Normal) {
        const int buckets = std::abs(static_cast<int>(key.scaleQ) -
                                     static_cast<int>(focus.scaleQ));
        v /= (1.0 + kScalePenaltyPerBucket * static_cast<double>(buckets));
    }

    const double freshness =
        std::max(kFreshnessFloor,
                 std::pow(0.5, static_cast<double>(ageTicks) / kFreshnessHalfLife));
    v *= freshness;

    switch (cls) {
        case TileClass::Proxy: v *= kProxyBonus; break;
        case TileClass::Thumb: v *= kThumbBonus; break;
        case TileClass::Normal: break;
    }
    return v;
}

// ---------------------------------------------------------------------------

RasterCache::RasterCache(std::size_t byteBudget) noexcept : budget_(byteBudget) {}

TileHandle RasterCache::find(const TileKey& key) {
    const std::lock_guard lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end()) {
        ++counters_.misses;
        return nullptr;
    }
    it->second.lastUsed = clock_.load(std::memory_order_relaxed);
    ++counters_.hits;
    return it->second.tile;
}

bool RasterCache::contains(const TileKey& key) const {
    const std::lock_guard lock(mutex_);
    return map_.find(key) != map_.end();
}

TileHandle RasterCache::findFallback(const TileKey& key) const {
    const std::lock_guard lock(mutex_);

    TileHandle best;
    int bestCost = 0;

    for (const auto& [k, e] : map_) {
        if (k.pageId != key.pageId || k.rotation != key.rotation ||
            k.scheme != key.scheme || k.tileX != key.tileX || k.tileY != key.tileY) {
            continue;
        }
        if (k.scaleQ == key.scaleQ) {
            return e.tile;   // exact; caller should have used find()
        }
        // Minifying a bigger tile beats magnifying a smaller one, so charge
        // double for buckets below the target.
        const int delta = static_cast<int>(k.scaleQ) - static_cast<int>(key.scaleQ);
        const int cost = (delta > 0) ? delta : (-delta) * 2;
        if (!best || cost < bestCost) {
            best = e.tile;
            bestCost = cost;
        }
    }
    return best;
}

TileHandle RasterCache::insert(std::unique_ptr<RasterTile> tile, TileClass cls) {
    if (!tile) {
        return nullptr;
    }
    const std::size_t sz = tile->byteSize();
    const TileKey key = tile->key;

    const std::lock_guard lock(mutex_);

    if (sz > budget_) {
        // One tile larger than the entire budget. Refusing is correct: caching
        // it would evict everything else and then itself. Happens for real at
        // 1600% zoom on a phone, which is why this returns rather than asserts.
        ++counters_.rejected;
        return nullptr;
    }

    if (const auto existing = map_.find(key); existing != map_.end()) {
        // Re-render of a tile we already have, usually a progressive pass
        // finishing. Replace in place and keep the pin count.
        bytes_ -= existing->second.bytes;
        existing->second.tile = std::shared_ptr<const RasterTile>(std::move(tile));
        existing->second.bytes = sz;
        existing->second.lastUsed = clock_.load(std::memory_order_relaxed);
        existing->second.cls = cls;
        bytes_ += sz;
        ++counters_.insertions;
        evictLocked(0);
        return existing->second.tile;
    }

    evictLocked(sz);

    Entry e;
    e.tile = std::shared_ptr<const RasterTile>(std::move(tile));
    e.bytes = sz;
    e.lastUsed = clock_.load(std::memory_order_relaxed);
    e.cls = cls;

    auto handle = e.tile;
    bytes_ += sz;
    map_.emplace(key, std::move(e));
    ++counters_.insertions;
    return handle;
}

void RasterCache::evictLocked(std::size_t bytesNeeded) {
    if (bytes_ + bytesNeeded <= budget_) {
        return;
    }

    // Score everything evictable once, sort ascending, walk until we fit.
    //
    // O(n log n) per eviction burst. With a 256 MB budget and 1 MB tiles that
    // is a few hundred entries - microseconds, off the compositor thread, and
    // only when the cache is actually full. A heap would be asymptotically
    // nicer and would need incremental re-scoring every time the focus moves,
    // which is every frame. Measure before changing this.
    struct Victim {
        double      value;
        std::size_t bytes;
        TileKey     key;
    };
    std::vector<Victim> victims;
    victims.reserve(map_.size());

    const std::uint64_t now = clock_.load(std::memory_order_relaxed);
    for (const auto& [k, e] : map_) {
        if (e.pins > 0) {
            continue;
        }
        const std::uint64_t age = (now > e.lastUsed) ? (now - e.lastUsed) : 0;
        victims.push_back({retentionValue(k, e.cls, age, focus_), e.bytes, k});
    }

    std::sort(victims.begin(), victims.end(), [](const Victim& a, const Victim& b) {
        if (a.value != b.value) {
            return a.value < b.value;      // least valuable first
        }
        return a.bytes > b.bytes;          // tie: reclaim more per eviction
    });

    for (const auto& v : victims) {
        if (bytes_ + bytesNeeded <= budget_) {
            break;
        }
        const auto it = map_.find(v.key);
        if (it == map_.end()) {
            continue;
        }
        bytes_ -= it->second.bytes;
        ++counters_.evictions;
        counters_.evictedBytes += it->second.bytes;
        map_.erase(it);
    }

    // If we are still over budget here, every remaining entry is pinned. That
    // is a caller bug - pinning more than the budget - and it is better to run
    // over than to drop a tile the compositor is about to draw. The overage is
    // visible in stats().bytes > stats().budget, which the perf gate asserts
    // on, so it cannot hide.
}

void RasterCache::setFocus(const CacheFocus& focus) {
    const std::lock_guard lock(mutex_);
    focus_ = focus;
}

CacheFocus RasterCache::focus() const {
    const std::lock_guard lock(mutex_);
    return focus_;
}

void RasterCache::pin(const TileKey& key) {
    const std::lock_guard lock(mutex_);
    if (const auto it = map_.find(key); it != map_.end()) {
        ++it->second.pins;
    }
}

void RasterCache::unpin(const TileKey& key) {
    const std::lock_guard lock(mutex_);
    if (const auto it = map_.find(key); it != map_.end() && it->second.pins > 0) {
        --it->second.pins;
    }
}

void RasterCache::setBudget(std::size_t bytes) {
    const std::lock_guard lock(mutex_);
    budget_ = bytes;
    evictLocked(0);
}

std::size_t RasterCache::budget() const {
    const std::lock_guard lock(mutex_);
    return budget_;
}

std::size_t RasterCache::invalidatePage(PageId page, Generation gen) {
    const std::lock_guard lock(mutex_);
    std::size_t dropped = 0;
    for (auto it = map_.begin(); it != map_.end();) {
        if (it->first.pageId == page && it->second.tile->generation < gen) {
            bytes_ -= it->second.bytes;
            it = map_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

std::size_t RasterCache::invalidateOlderThan(Generation gen) {
    const std::lock_guard lock(mutex_);
    std::size_t dropped = 0;
    for (auto it = map_.begin(); it != map_.end();) {
        if (it->second.tile->generation < gen) {
            bytes_ -= it->second.bytes;
            it = map_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

void RasterCache::clear() {
    const std::lock_guard lock(mutex_);
    map_.clear();
    bytes_ = 0;
}

RasterCache::Stats RasterCache::stats() const {
    const std::lock_guard lock(mutex_);
    Stats s = counters_;
    s.bytes = bytes_;
    s.entries = map_.size();
    s.budget = budget_;
    return s;
}

void RasterCache::resetCounters() {
    const std::lock_guard lock(mutex_);
    const Stats zero{};
    counters_ = zero;
}

}  // namespace folio::core
