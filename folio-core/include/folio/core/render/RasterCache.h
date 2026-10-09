// folio/core/render/RasterCache.h  --  work package C2
//
// The bounded tile store that sits between the rasterizer and the compositor.
//
// This is on the high-risk list for one reason: an unbounded raster cache is
// the single most reliable way to get a PDF reader killed by the OS on a
// phone. A 300-page document at 200% zoom is tens of gigabytes of pixels. The
// cache must therefore be hard-bounded in BYTES - not entries, not pages - and
// it must choose what to throw away well enough that scrolling back one page
// never re-renders.
//
// ---------------------------------------------------------------------------
// EVICTION POLICY, AND THE ONE THAT WAS REJECTED
// ---------------------------------------------------------------------------
// Each live entry gets a retention value in (0, 1]:
//
//     value = proximity(pageDistance) * affinity(scale, scheme) * freshness(age)
//
// and eviction walks entries in ascending value until the cache is back under
// budget. Pinned entries are skipped entirely.
//
// The obvious textbook alternative is GreedyDual-Size: order by value/bytes,
// so that evicting one large object is preferred to evicting many small ones.
// It is wrong here, and it is worth writing down why, because it looks right.
//
// GreedyDual-Size assumes value and size are roughly independent. In a tile
// cache they are strongly, adversarially correlated: the tiles the user is
// actually looking at are full-resolution 512x512 RGBA (1 MB each), while the
// low-value junk - proxies, thumbnails, stale zoom buckets - is 16-256 KB.
// Dividing by size therefore ranks the visible page BELOW the off-screen
// thumbnails, and the cache cheerfully evicts exactly what is on screen. A
// 0.9-value 1 MB tile scores 9e-7; a 0.05-value 16 KB thumbnail scores 3e-6.
// The thumbnail wins. The user sees a blank page.
//
// So: strict value ordering, size used only to decide how many entries must
// go. Size does break ties, largest-first, which is free and mildly helpful.
//
// ---------------------------------------------------------------------------
// LIFETIME
// ---------------------------------------------------------------------------
// find() hands back a shared_ptr. Eviction drops the cache's reference; if the
// compositor is mid-frame and still holds one, the pixels stay alive until it
// lets go. No locking across the frame, no use-after-free, no copy. This is
// the whole reason entries are refcounted rather than owned outright.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "folio/core/engine/DocumentSnapshot.h"
#include "folio/core/render/RasterTile.h"
#include "folio/core/render/TileKey.h"

namespace folio::core {

/// What the user is looking at. Drives every eviction decision, so it must be
/// updated on every viewport change - a stale focus silently turns the policy
/// back into plain LRU.
struct CacheFocus {
    PageId        page = 0;
    std::uint16_t scaleQ = 8;                      // 1.0x
    ColorScheme   scheme = ColorScheme::Light;
};

enum class TileClass : std::uint8_t {
    Normal = 0,
    /// Always-resident low-resolution whole-page image. Never evicted while
    /// its page is within the proxy window. This is the "no blank pages while
    /// flinging" guarantee from plan §8.2 and it is worth a few MB.
    Proxy  = 1,
    /// Grid / organise thumbnails. Cheap, and re-rendering them during a scrub
    /// is very visible, so they decay slowly.
    Thumb  = 2,
};

class RasterCache {
public:
    struct Stats {
        std::size_t bytes = 0;
        std::size_t entries = 0;
        std::size_t budget = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t insertions = 0;
        std::uint64_t evictions = 0;
        std::uint64_t evictedBytes = 0;
        std::uint64_t rejected = 0;     // single item bigger than the budget

        [[nodiscard]] double hitRate() const noexcept {
            const auto total = hits + misses;
            return total ? static_cast<double>(hits) / static_cast<double>(total) : 0.0;
        }
    };

    explicit RasterCache(std::size_t byteBudget) noexcept;

    RasterCache(const RasterCache&) = delete;
    RasterCache& operator=(const RasterCache&) = delete;

    // --- lookup ------------------------------------------------------------

    /// Fetch and mark used. Counts a hit or a miss.
    [[nodiscard]] TileHandle find(const TileKey& key);

    /// Presence test that does NOT touch recency and does NOT count as a hit.
    /// The scheduler uses this to avoid enqueuing work that is already done;
    /// if it used find(), every dedupe check would make the entry look hot.
    [[nodiscard]] bool contains(const TileKey& key) const;

    /// Best available substitute for `key` at a different zoom bucket, for
    /// drawing something sharp-ish while the real tile renders. Prefers the
    /// nearest bucket, with a mild bias to downscaling (minifying a larger
    /// tile looks better than magnifying a smaller one).
    [[nodiscard]] TileHandle findFallback(const TileKey& key) const;

    // --- insertion ---------------------------------------------------------

    /// Takes ownership. Returns the published handle, or nullptr if the tile
    /// was rejected because it alone exceeds the budget.
    TileHandle insert(std::unique_ptr<RasterTile> tile, TileClass cls = TileClass::Normal);

    // --- policy inputs -----------------------------------------------------

    void setFocus(const CacheFocus& focus);
    [[nodiscard]] CacheFocus focus() const;

    /// Advances the logical clock. Call once per composited frame. Age is
    /// measured in ticks rather than wall-clock so tests are deterministic and
    /// so a backgrounded app does not age its whole cache out while idle.
    void tick() noexcept { ++clock_; }
    [[nodiscard]] std::uint64_t now() const noexcept { return clock_.load(std::memory_order_relaxed); }

    /// Protect from eviction. Reference-counted: two pins need two unpins.
    void pin(const TileKey& key);
    void unpin(const TileKey& key);

    /// Re-budget at run time - the mobile foreground/background transition
    /// halves this rather than dropping everything (plan §8.4).
    void setBudget(std::size_t bytes);
    [[nodiscard]] std::size_t budget() const;

    // --- invalidation ------------------------------------------------------

    /// Drop every tile for `page` rendered from a snapshot older than `gen`.
    /// This is the edit path: annotate a page, its tiles die, nothing else
    /// does. Returns the number dropped.
    std::size_t invalidatePage(PageId page, Generation gen);

    /// Drop everything older than `gen`, any page. Used after a save or an
    /// undo that rewrites the whole file.
    std::size_t invalidateOlderThan(Generation gen);

    void clear();

    [[nodiscard]] Stats stats() const;
    void resetCounters();

    /// Exposed for the eviction tests. Higher means keep. Pure function of the
    /// arguments, so the policy can be exercised without building a cache.
    [[nodiscard]] static double retentionValue(const TileKey& key,
                                               TileClass cls,
                                               std::uint64_t ageTicks,
                                               const CacheFocus& focus) noexcept;

private:
    struct Entry {
        std::shared_ptr<const RasterTile> tile;
        std::uint64_t lastUsed = 0;
        std::size_t   bytes = 0;
        std::uint32_t pins = 0;
        TileClass     cls = TileClass::Normal;
    };

    /// Caller holds the lock.
    void evictLocked(std::size_t bytesNeeded);

    mutable std::mutex                        mutex_;
    std::unordered_map<TileKey, Entry>        map_;
    std::size_t                               bytes_ = 0;
    std::size_t                               budget_ = 0;
    CacheFocus                                focus_{};
    std::atomic<std::uint64_t>                clock_{1};
    mutable Stats                             counters_{};
};

}  // namespace folio::core
