// tests/render/test_skia_page_cache.cpp
//
// The offscreen page-raster LRU in SkiaPaintTarget.
//
// This is the cache that turns "re-run a thousand glyph draws per page per
// frame" into one textured blit. It is also the component most able to leak
// memory quietly, so the properties worth pinning down are the boring ones:
// a hit returns the same pixels, the byte budget is actually respected,
// eviction prefers the least recently used entry, and the idle trim gives
// memory back.

#include "../Harness.h"

#include "folio/render/IPaintTarget.h"
#include "../../folio-render/skia/SkiaPaintTarget.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"

#include <algorithm>
#include <cstdlib>

using namespace folio::render;
using folio::render::skia::SkiaPaintTarget;

namespace {

sk_sp<SkSurface> makeSurface(int w, int h) {
    return SkSurfaces::Raster(
        SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType));
}

constexpr int kTile = 64;                              // px per cached entry
constexpr std::size_t kTileBytes = kTile * kTile * 4;  // 16 KiB

}  // namespace

FOLIO_TEST_MAIN("skia page raster cache") {
    using folio::test::check;
    using folio::test::section;

    auto surf = makeSurface(400, 400);
    SkiaPaintTarget target(surf->getCanvas());

    section("a repeated draw runs its paint callback once");
    {
        SkiaPaintTarget::setRasterCacheBudget(64u * 1024u * 1024u);
        target.invalidateRasterCache();

        int painted = 0;
        const auto draw = [&painted](IPaintTarget& g) {
            ++painted;
            g.fillRect(Rect{0.f, 0.f, kTile, kTile}, Color{255, 0, 0, 255});
        };
        const Rect dst{0.f, 0.f, kTile, kTile};

        const bool first = target.drawCached(1, dst, kTile, kTile, draw);
        const bool second = target.drawCached(1, dst, kTile, kTile, draw);
        const bool third = target.drawCached(1, dst, kTile, kTile, draw);

        check(!first, "first call is a miss");
        check(second && third, "repeat calls hit the cache");
        check((painted) == (1), "the paint callback runs exactly once for three draws");

        const auto s = target.rasterCacheStats();
        check((static_cast<int>(s.entries)) == (1), "one entry cached");
        check((static_cast<int>(s.hits)) == (2), "two hits recorded");
        check((static_cast<int>(s.misses)) == (1), "one miss recorded");
        check((static_cast<int>(s.bytes)) == (static_cast<int>(kTileBytes)), "accounted bytes match w*h*4");
    }

    section("a cache hit is pixel-identical to the miss that filled it");
    // A cache that returns *different* pixels on a hit is worse than no cache,
    // so compare a cached blit against the same content drawn directly.
    {
        target.invalidateRasterCache();
        const auto draw = [](IPaintTarget& g) {
            g.fillRect(Rect{0.f, 0.f, kTile, kTile}, Color{20, 40, 200, 255});
            g.fillRect(Rect{8.f, 8.f, 16.f, 16.f}, Color{255, 255, 0, 255});
        };

        auto a = makeSurface(kTile, kTile);
        auto b = makeSurface(kTile, kTile);
        const Rect dst{0.f, 0.f, kTile, kTile};

        {  // miss path, then hit path, into two separate surfaces
            SkiaPaintTarget ta(a->getCanvas());
            ta.drawCached(7, dst, kTile, kTile, draw);
        }
        {
            SkiaPaintTarget tb(b->getCanvas());
            const bool hit = tb.drawCached(7, dst, kTile, kTile, draw);
            check(hit, "second surface hits the shared cache");
        }

        SkPixmap pa, pb;
        check(a->peekPixels(&pa) && b->peekPixels(&pb), "pixels readable");
        int worst = 0;
        for (int y = 0; y < kTile; ++y)
            for (int x = 0; x < kTile; ++x) {
                const auto ca = pa.getColor(x, y);
                const auto cb = pb.getColor(x, y);
                worst = std::max(
                    {worst, std::abs(int(SkColorGetR(ca)) - int(SkColorGetR(cb))),
                     std::abs(int(SkColorGetG(ca)) - int(SkColorGetG(cb))),
                     std::abs(int(SkColorGetB(ca)) - int(SkColorGetB(cb)))});
            }
        check((worst) == (0), "a cache hit is byte-identical to the miss that filled it");
    }

    section("byte budget and least-recently-used eviction");
    {
        target.invalidateRasterCache();
        // Room for exactly 4 tiles.
        SkiaPaintTarget::setRasterCacheBudget(kTileBytes * 4);

        const auto draw = [](IPaintTarget& g) {
            g.fillRect(Rect{0.f, 0.f, kTile, kTile}, Color{1, 2, 3, 255});
        };
        const Rect dst{0.f, 0.f, kTile, kTile};

        for (std::uint64_t k = 0; k < 4; ++k)
            target.drawCached(100 + k, dst, kTile, kTile, draw);
        check((static_cast<int>(target.rasterCacheStats().entries)) == (4), "four fit");

        // Touch 100,101,102 so 103 becomes least-recently-used, then insert a
        // fifth. 103 is the one that must go.
        for (std::uint64_t k = 0; k < 3; ++k)
            target.drawCached(100 + k, dst, kTile, kTile, draw);
        target.drawCached(999, dst, kTile, kTile, draw);

        const auto s = target.rasterCacheStats();
        check((static_cast<int>(s.entries)) == (4), "still four after eviction");
        check(s.bytes <= kTileBytes * 4, "byte budget is respected");
        check(s.evictions >= 1, "something was evicted");

        const std::size_t before = target.rasterCacheStats().hits;
        target.drawCached(103, dst, kTile, kTile, draw);
        check((static_cast<int>(target.rasterCacheStats().hits)) == (static_cast<int>(before)), "the LRU entry (103) is the one dropped");

        // Re-inserting 103 evicted the next-oldest (100) in its turn, so the
        // entry to probe for survival is the most recently touched one.
        const std::size_t h2 = target.rasterCacheStats().hits;
        target.drawCached(999, dst, kTile, kTile, draw);
        check((static_cast<int>(target.rasterCacheStats().hits)) == (static_cast<int>(h2 + 1)),
              "the most recently used entry survived");
    }

    section("an oversize entry bypasses the cache");
    // One entry bigger than a quarter of the budget would evict everything
    // else on every frame, so it must bypass the cache entirely.
    {
        target.invalidateRasterCache();
        SkiaPaintTarget::setRasterCacheBudget(kTileBytes * 4);
        int painted = 0;
        const auto draw = [&painted](IPaintTarget& g) {
            ++painted;
            g.fillRect(Rect{0.f, 0.f, 512.f, 512.f}, Color{9, 9, 9, 255});
        };
        target.drawCached(5000, Rect{0.f, 0.f, 200.f, 200.f}, 512, 512, draw);
        target.drawCached(5000, Rect{0.f, 0.f, 200.f, 200.f}, 512, 512, draw);
        check((painted) == (2), "an oversize draw is not cached, it is drawn through");
        check((static_cast<int>(target.rasterCacheStats().entries)) == (0), "and it leaves no entry behind");
    }

    section("idle trim releases cold entries and keeps hot ones");
    {
        target.invalidateRasterCache();
        SkiaPaintTarget::setRasterCacheBudget(64u * 1024u * 1024u);
        const auto draw = [](IPaintTarget& g) {
            g.fillRect(Rect{0.f, 0.f, kTile, kTile}, Color{4, 5, 6, 255});
        };
        const Rect dst{0.f, 0.f, kTile, kTile};

        for (std::uint64_t k = 0; k < 10; ++k)
            target.drawCached(2000 + k, dst, kTile, kTile, draw);
        check((static_cast<int>(target.rasterCacheStats().entries)) == (10), "ten cached");

        // Keep two of them hot, as a visible page would be.
        for (int i = 0; i < 20; ++i) {
            target.drawCached(2000, dst, kTile, kTile, draw);
            target.drawCached(2001, dst, kTile, kTile, draw);
        }

        SkiaPaintTarget::trimRasterCache(/*maxAgeFrames=*/20);
        const auto s = target.rasterCacheStats();
        check(s.entries < 10, "idle trim released cold entries");
        check(s.bytes == s.entries * kTileBytes, "byte count tracks the trim");

        const std::size_t h = s.hits;
        target.drawCached(2000, dst, kTile, kTile, draw);
        target.drawCached(2001, dst, kTile, kTile, draw);
        check((static_cast<int>(target.rasterCacheStats().hits)) == (static_cast<int>(h + 2)), "hot entries survived the trim");

        SkiaPaintTarget::setRasterCacheBudget(256u * 1024u * 1024u);
    }

}
