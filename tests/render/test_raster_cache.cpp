// Work package C2 acceptance tests.
//
//   "Done when: the cache holds a hard byte budget under a synthetic fling
//    across 400 pages, never exceeds it, and scrolling back one page is a hit."
//
// The interesting tests are the last three. Anyone can test that an LRU cache
// evicts; what matters here is that the policy does not evict the page on
// screen, and that a fling does not flush the cache it is flying through.

#include "folio/core/render/RasterCache.h"
#include "../Harness.h"

#include <random>
#include <vector>

using namespace folio::core;
using namespace folio::test;

namespace {

constexpr std::size_t kTileBytes = 512 * 512 * 4;   // 1 MiB, a real tile

std::unique_ptr<RasterTile> makeTile(PageId page, std::uint16_t tx, std::uint16_t ty,
                                     std::uint16_t scaleQ = 8,
                                     ColorScheme scheme = ColorScheme::Light,
                                     Generation gen = 1,
                                     std::size_t bytes = kTileBytes) {
    auto t = std::make_unique<RasterTile>();
    t->key.pageId = page;
    t->key.tileX = tx;
    t->key.tileY = ty;
    t->key.scaleQ = scaleQ;
    t->key.scheme = scheme;
    t->width = 512;
    t->height = 512;
    t->stride = 512 * 4;
    t->generation = gen;
    t->pixels.resize(bytes);
    return t;
}

TileKey keyOf(PageId page, std::uint16_t tx = 0, std::uint16_t ty = 0,
              std::uint16_t scaleQ = 8, ColorScheme scheme = ColorScheme::Light) {
    TileKey k;
    k.pageId = page; k.tileX = tx; k.tileY = ty; k.scaleQ = scaleQ; k.scheme = scheme;
    return k;
}

// --- basics ----------------------------------------------------------------

void test_insert_find_roundtrip() {
    section("C2  insert / find / miss accounting");
    RasterCache cache(64u * 1024 * 1024);

    check(cache.find(keyOf(1)) == nullptr, "miss on empty cache");
    check(cache.stats().misses == 1, "miss counted");

    auto h = cache.insert(makeTile(1, 0, 0));
    check(h != nullptr, "insert returns a handle");
    check(cache.stats().entries == 1, "one entry");
    check(cache.stats().bytes >= kTileBytes, "bytes accounted");

    auto got = cache.find(keyOf(1));
    check(got != nullptr, "hit after insert");
    check(got.get() == h.get(), "same object, not a copy");
    check(cache.stats().hits == 1, "hit counted");

    check(cache.contains(keyOf(1)), "contains() true");
    check(cache.stats().hits == 1, "contains() does not count as a hit");
    check(cache.stats().misses == 1, "contains() does not count as a miss");
}

void test_budget_is_never_exceeded() {
    section("C2  hard byte budget holds under sustained insertion");
    const std::size_t budget = 16u * 1024 * 1024;     // 16 tiles' worth
    RasterCache cache(budget);

    std::size_t peak = 0;
    for (PageId p = 0; p < 400; ++p) {
        cache.setFocus({p, 8, ColorScheme::Light});
        for (std::uint16_t t = 0; t < 4; ++t) {
            cache.insert(makeTile(p, t, 0));
            peak = std::max(peak, cache.stats().bytes);
        }
        cache.tick();
    }
    check(peak <= budget, "peak bytes never exceeded the budget");
    note("peak " + std::to_string(peak / 1024 / 1024) + " MiB of " +
         std::to_string(budget / 1024 / 1024) + " MiB budget, " +
         std::to_string(cache.stats().evictions) + " evictions over 1600 inserts");
}

void test_oversized_tile_is_rejected() {
    section("C2  a tile larger than the whole budget is refused, not cached");
    RasterCache cache(1u * 1024 * 1024);
    cache.insert(makeTile(1, 0, 0, 8, ColorScheme::Light, 1, 512 * 1024));
    check(cache.stats().entries == 1, "normal tile cached");

    auto h = cache.insert(makeTile(2, 0, 0, 8, ColorScheme::Light, 1, 4u * 1024 * 1024));
    check(h == nullptr, "oversized insert returns nullptr");
    check(cache.stats().rejected == 1, "rejection counted");
    check(cache.stats().entries == 1, "it did not evict the cache on its way out");
    check(cache.find(keyOf(1)) != nullptr, "the existing tile survived");
}

// --- the policy ------------------------------------------------------------

void test_visible_page_outlives_distant_pages() {
    section("C2  the page on screen is not evicted for distant pages");
    //
    // This is the test that fails under GreedyDual-Size (value/bytes), which
    // is why the policy does not use it. See the header comment.
    //
    const std::size_t budget = 12u * 1024 * 1024;
    RasterCache cache(budget);
    cache.setFocus({100, 8, ColorScheme::Light});

    // Four full-resolution tiles of the visible page.
    for (std::uint16_t t = 0; t < 4; ++t) {
        cache.insert(makeTile(100, t, 0));
    }
    // A flood of small, far-away thumbnails - the adversarial case.
    for (PageId p = 0; p < 300; ++p) {
        if (p == 100) continue;
        cache.insert(makeTile(p, 0, 0, 2, ColorScheme::Light, 1, 64 * 1024),
                     TileClass::Thumb);
    }

    int survivors = 0;
    for (std::uint16_t t = 0; t < 4; ++t) {
        if (cache.contains(keyOf(100, t))) ++survivors;
    }
    check(survivors == 4, "all four visible tiles survived the thumbnail flood");
    check(cache.stats().bytes <= budget, "still inside the budget");
    note(std::to_string(survivors) + "/4 visible tiles, " +
         std::to_string(cache.stats().entries) + " total entries");
}

void test_scroll_back_one_page_is_a_hit() {
    section("C2  scrolling back one page never re-renders");
    RasterCache cache(32u * 1024 * 1024);

    for (PageId p = 0; p < 40; ++p) {
        cache.setFocus({p, 8, ColorScheme::Light});
        for (std::uint16_t t = 0; t < 4; ++t) {
            cache.insert(makeTile(p, t, 0));
        }
        cache.tick();

        if (p > 0) {
            int hits = 0;
            for (std::uint16_t t = 0; t < 4; ++t) {
                if (cache.contains(keyOf(p - 1, t))) ++hits;
            }
            check(hits == 4, "page " + std::to_string(p - 1) +
                             " still resident while viewing " + std::to_string(p));
        }
    }
}

void test_proxy_survives_everything() {
    section("C2  proxies are the no-blank-page guarantee and must not evict");
    const std::size_t budget = 8u * 1024 * 1024;
    RasterCache cache(budget);
    cache.setFocus({0, 8, ColorScheme::Light});

    // One proxy for a page we then scroll a very long way from.
    cache.insert(makeTile(5, 0, 0, 1, ColorScheme::Light, 1, 256 * 1024),
                 TileClass::Proxy);

    for (PageId p = 100; p < 200; ++p) {
        cache.setFocus({p, 8, ColorScheme::Light});
        cache.insert(makeTile(p, 0, 0));
        cache.tick();
    }
    check(cache.contains(keyOf(5, 0, 0, 1)),
          "the proxy for page 5 survived 100 pages of full-res inserts");
}

void test_stale_colour_scheme_is_evicted_first() {
    section("C2  tiles in the old colour scheme go first after a theme switch");
    RasterCache cache(6u * 1024 * 1024);
    cache.setFocus({10, 8, ColorScheme::Light});

    for (std::uint16_t t = 0; t < 3; ++t) {
        cache.insert(makeTile(10, t, 0, 8, ColorScheme::Light));
    }
    cache.setFocus({10, 8, ColorScheme::Dark});        // user hits dark mode
    for (std::uint16_t t = 0; t < 3; ++t) {
        cache.insert(makeTile(10, t, 0, 8, ColorScheme::Dark));
    }

    int lightLeft = 0, darkLeft = 0;
    for (std::uint16_t t = 0; t < 3; ++t) {
        if (cache.contains(keyOf(10, t, 0, 8, ColorScheme::Light))) ++lightLeft;
        if (cache.contains(keyOf(10, t, 0, 8, ColorScheme::Dark)))  ++darkLeft;
    }
    check(darkLeft == 3, "all three dark tiles resident");
    check(lightLeft < 3, "stale light tiles were preferred for eviction");
    note("light " + std::to_string(lightLeft) + "/3, dark " +
         std::to_string(darkLeft) + "/3");
}

void test_pinning_is_absolute_and_refcounted() {
    section("C2  pin / unpin");
    RasterCache cache(4u * 1024 * 1024);
    cache.setFocus({0, 8, ColorScheme::Light});

    cache.insert(makeTile(999, 0, 0));
    cache.pin(keyOf(999));
    cache.pin(keyOf(999));            // two pins

    for (PageId p = 0; p < 50; ++p) {
        cache.setFocus({p, 8, ColorScheme::Light});
        cache.insert(makeTile(p, 0, 0));
        cache.tick();
    }
    check(cache.contains(keyOf(999)), "pinned tile survived 50 competing inserts");

    cache.unpin(keyOf(999));
    check(cache.contains(keyOf(999)), "one unpin is not enough");
    cache.unpin(keyOf(999));
    for (PageId p = 50; p < 80; ++p) {
        cache.setFocus({p, 8, ColorScheme::Light});
        cache.insert(makeTile(p, 0, 0));
        cache.tick();
    }
    check(!cache.contains(keyOf(999)), "evictable again after the second unpin");
}

// --- lifetime --------------------------------------------------------------

void test_handle_outlives_eviction() {
    section("C2  a handle held across eviction stays valid");
    //
    // The compositor can be mid-frame when the render thread evicts. If that
    // frees pixels the GPU upload is reading, it is a crash that reproduces
    // once a week on one user's machine. Refcounting makes it impossible.
    //
    RasterCache cache(2u * 1024 * 1024);
    cache.setFocus({0, 8, ColorScheme::Light});
    cache.insert(makeTile(1, 0, 0));

    auto held = cache.find(keyOf(1));
    check(held != nullptr, "got a handle");
    const auto* raw = held.get();

    for (PageId p = 100; p < 140; ++p) {
        cache.setFocus({p, 8, ColorScheme::Light});
        cache.insert(makeTile(p, 0, 0));
        cache.tick();
    }
    check(!cache.contains(keyOf(1)), "the tile was evicted from the map");
    check(held.get() == raw, "but the held handle still points at live pixels");
    check(held->pixels.size() == kTileBytes, "and the pixels are intact");
    check(held->key.pageId == 1, "and the key is intact");
}

// --- invalidation ----------------------------------------------------------

void test_edit_invalidates_one_page_only() {
    section("C2  an edit drops that page's tiles and nothing else");
    RasterCache cache(64u * 1024 * 1024);
    for (PageId p = 0; p < 10; ++p) {
        for (std::uint16_t t = 0; t < 3; ++t) {
            cache.insert(makeTile(p, t, 0, 8, ColorScheme::Light, /*gen=*/1));
        }
    }
    check(cache.stats().entries == 30, "30 tiles cached");

    const std::size_t dropped = cache.invalidatePage(4, /*gen=*/2);
    check(dropped == 3, "three tiles dropped for page 4");
    check(cache.stats().entries == 27, "everything else untouched");
    check(!cache.contains(keyOf(4, 0)), "page 4 tile 0 gone");
    check(cache.contains(keyOf(3, 0)), "page 3 untouched");
    check(cache.contains(keyOf(5, 0)), "page 5 untouched");

    cache.insert(makeTile(4, 0, 0, 8, ColorScheme::Light, /*gen=*/2));
    check(cache.invalidatePage(4, 2) == 0, "the gen-2 re-render is not dropped again");
}

void test_fallback_prefers_nearest_scale_and_downscaling() {
    section("C2  findFallback picks a sane substitute while the real tile renders");
    RasterCache cache(64u * 1024 * 1024);
    cache.insert(makeTile(7, 0, 0, /*scaleQ=*/4));     // 0.5x
    cache.insert(makeTile(7, 0, 0, /*scaleQ=*/16));    // 2.0x

    // Want 1.0x (scaleQ 8). Down is 4 buckets below, up is 8 above; the rule
    // charges double for magnifying, so 16 (cost 8) beats 4 (cost 8)... tie,
    // and a third candidate settles it.
    cache.insert(makeTile(7, 0, 0, /*scaleQ=*/12));    // 1.5x, cost 4
    auto fb = cache.findFallback(keyOf(7, 0, 0, 8));
    check(fb != nullptr, "found a fallback");
    check(fb->key.scaleQ == 12, "picked the 1.5x tile (nearest, downscaling)");

    auto exact = cache.findFallback(keyOf(7, 0, 0, 4));
    check(exact != nullptr && exact->key.scaleQ == 4, "exact match returned when present");

    check(cache.findFallback(keyOf(8, 0, 0, 8)) == nullptr,
          "no fallback from a different page");
}

// --- the thing that actually matters ---------------------------------------

void test_synthetic_fling_hit_rate() {
    section("C2  synthetic fling across 400 pages: budget held, hit rate sane");
    //
    // Models the real access pattern: a fast scroll forward with occasional
    // back-tracking, 6 tiles per page, mobile budget.
    //
    const std::size_t budget = 96u * 1024 * 1024;     // the mobile number
    RasterCache cache(budget);
    std::mt19937 rng(20261008);
    std::uniform_int_distribution<int> backtrack(0, 9);

    std::size_t peak = 0;
    PageId page = 0;
    for (int step = 0; step < 2000; ++step) {
        // 10% of the time the user scrolls back a page or two.
        if (backtrack(rng) == 0 && page >= 2) {
            page -= 2;
        } else {
            page = (page + 1) % 400;
        }
        cache.setFocus({page, 8, ColorScheme::Light});

        for (std::uint16_t t = 0; t < 6; ++t) {
            const auto k = keyOf(page, t);
            if (!cache.find(k)) {
                cache.insert(makeTile(page, t, 0));
            }
        }
        cache.tick();
        peak = std::max(peak, cache.stats().bytes);
    }

    const auto s = cache.stats();
    check(peak <= budget, "budget held across the whole fling");
    check(s.hitRate() > 0.10, "hit rate above 10% despite 400 pages in 96 MB");
    note("peak " + std::to_string(peak / 1024 / 1024) + " MiB, hit rate " +
         std::to_string(static_cast<int>(s.hitRate() * 100)) + "%, " +
         std::to_string(s.evictions) + " evictions, " +
         std::to_string(s.entries) + " resident");
}

}  // namespace

FOLIO_TEST_MAIN("C2 RasterCache") {
    test_insert_find_roundtrip();
    test_budget_is_never_exceeded();
    test_oversized_tile_is_rejected();
    test_visible_page_outlives_distant_pages();
    test_scroll_back_one_page_is_a_hit();
    test_proxy_survives_everything();
    test_stale_colour_scheme_is_evicted_first();
    test_pinning_is_absolute_and_refcounted();
    test_handle_outlives_eviction();
    test_edit_invalidates_one_page_only();
    test_fallback_prefers_nearest_scale_and_downscaling();
    test_synthetic_fling_hit_rate();
}
