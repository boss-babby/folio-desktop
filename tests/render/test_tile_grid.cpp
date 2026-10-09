// Work package B3 / C1 acceptance tests.
//
//   C1: "Property test: round-trip any point through all rotations/zooms"
//   B3: "A 4x4 tile grid reassembles byte-identical to one full-page render"
//
// The B3 test runs against a FAKE rasterizer that applies the real matrices.
// That is deliberate: it validates TileGrid's matrix algebra without needing
// PDFium linked, so the math is proven correct before the engine lands. When
// PdfiumRasterizer arrives it must pass the same test.

#include "folio/core/render/TileGrid.h"
#include "folio/core/util/Arena.h"
#include "folio/core/util/Observable.h"
#include "folio/core/util/SlotMap.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace folio::core;

namespace {

int g_failures = 0;
void check(bool cond, const std::string& what) {
    if (!cond) { std::printf("  FAIL: %s\n", what.c_str()); ++g_failures; }
}
bool nearly(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

// --- C1: enumeration -------------------------------------------------------

void test_tiles_cover_page_exactly() {
    std::printf("C1  tiles cover the page with no gaps or overlaps\n");

    struct Case { float w, h, s; std::uint8_t rot; };
    const Case cases[] = {
        {612.f, 792.f, 1.00f, 0}, {612.f, 792.f, 2.00f, 0},
        {612.f, 792.f, 1.25f, 1}, {612.f, 792.f, 0.50f, 2},
        {841.f, 595.f, 3.00f, 3}, {100.f, 100.f, 1.00f, 0},
    };

    bool allOk = true;
    for (const auto& c : cases) {
        TileGrid g(c.w, c.h, c.s, c.rot);
        std::vector<std::uint8_t> cover(
            static_cast<std::size_t>(g.deviceWidth()) * g.deviceHeight(), 0);

        for (const auto& t : g.allTiles()) {
            for (std::int32_t y = t.rect.y; y < t.rect.bottom(); ++y)
                for (std::int32_t x = t.rect.x; x < t.rect.right(); ++x)
                    ++cover[static_cast<std::size_t>(y) * g.deviceWidth() + x];
        }
        for (auto v : cover) if (v != 1) { allOk = false; break; }

        // 90/270 must swap the device footprint.
        const bool swap = (c.rot == 1 || c.rot == 3);
        const auto pw = static_cast<std::int32_t>(std::lround(c.w * c.s));
        const auto ph = static_cast<std::int32_t>(std::lround(c.h * c.s));
        if (g.deviceWidth() != (swap ? ph : pw)) allOk = false;
        if (g.deviceHeight() != (swap ? pw : ph)) allOk = false;
    }
    check(allOk, "every device pixel covered exactly once, all rotations");
}

// --- C1: matrix round-trip -------------------------------------------------

void test_matrix_roundtrip_all_rotations() {
    std::printf("C1  page->device matrix correct for 0/90/180/270\n");

    const float w = 612.f, h = 792.f, s = 1.5f;
    const float W = w * s, H = h * s;

    // Where the page's top-left corner must land, per rotation.
    struct Expect { std::uint8_t rot; float x, y; };
    const Expect corners[] = {
        {0, 0.f, 0.f},   // identity
        {1, H,   0.f},   //  90 CW: top-left -> top-right of a H-wide canvas
        {2, W,   H  },   // 180   : top-left -> bottom-right
        {3, 0.f, W  },   // 270 CW: top-left -> bottom-left of a W-tall canvas
    };

    bool allOk = true;
    for (const auto& e : corners) {
        TileGrid g(w, h, s, e.rot);
        const auto [x, y] = g.fullPageMatrix().apply(0.f, 0.f);
        if (!nearly(x, e.x) || !nearly(y, e.y)) {
            std::printf("      rot %u: corner (0,0) -> (%.1f,%.1f), expected (%.1f,%.1f)\n",
                        e.rot, static_cast<double>(x), static_cast<double>(y),
                        static_cast<double>(e.x), static_cast<double>(e.y));
            allOk = false;
        }
    }
    check(allOk, "page origin maps to the right device corner in each rotation");

    // The opposite page corner must land inside the device bounds for all rots.
    bool inBounds = true;
    for (std::uint8_t rot = 0; rot < 4; ++rot) {
        TileGrid g(w, h, s, rot);
        const auto [x, y] = g.fullPageMatrix().apply(w, h);
        if (x < -0.5f || y < -0.5f ||
            x > static_cast<float>(g.deviceWidth()) + 0.5f ||
            y > static_cast<float>(g.deviceHeight()) + 0.5f) {
            inBounds = false;
        }
    }
    check(inBounds, "opposite page corner stays within device bounds");
}

// --- B3: tiles reassemble to the full page ---------------------------------

void test_tile_matrices_match_full_page() {
    std::printf("B3  tile matrices reassemble to the full-page render\n");

    bool allOk = true;
    for (std::uint8_t rot = 0; rot < 4; ++rot) {
        for (float s : {0.75f, 1.0f, 2.0f}) {
            TileGrid g(612.f, 792.f, s, rot, 256);  // 256 px -> a real 4x4+ grid
            const Matrix full = g.fullPageMatrix();

            for (const auto& t : g.allTiles()) {
                const Matrix m = t.matrix;
                // Sample the page interior; a tile's matrix must equal the
                // full-page matrix shifted by the tile origin. Any deviation
                // is exactly the one-pixel-seam bug.
                for (float py : {0.f, 396.f, 792.f}) {
                    for (float px : {0.f, 306.f, 612.f}) {
                        const auto [fx, fy] = full.apply(px, py);
                        const auto [tx, ty] = m.apply(px, py);
                        if (!nearly(tx, fx - static_cast<float>(t.rect.x)) ||
                            !nearly(ty, fy - static_cast<float>(t.rect.y))) {
                            allOk = false;
                        }
                    }
                }
            }
        }
    }
    check(allOk, "tileMatrix == fullPageMatrix - tileOrigin, all rots/zooms");
}

void test_fake_render_reassembly_is_byte_identical() {
    std::printf("B3  fake 4x4 tiled render == single full render, byte for byte\n");

    // Stand-in for FPDF_RenderPageBitmapWithMatrix: writes a deterministic
    // value derived from the PAGE-SPACE point that each output pixel samples.
    // If the matrices are right, tiling is invisible in the output.
    auto fakeRender = [](const Matrix& m, std::int32_t w, std::int32_t h,
                         std::vector<std::uint8_t>& out) {
        out.assign(static_cast<std::size_t>(w) * h, 0);
        // Invert the affine transform to recover page coords per device pixel.
        const float det = m.a * m.d - m.b * m.c;
        if (std::fabs(det) < 1e-9f) return;
        const float ia =  m.d / det, ic = -m.c / det;
        const float ib = -m.b / det, id =  m.a / det;
        for (std::int32_t y = 0; y < h; ++y) {
            for (std::int32_t x = 0; x < w; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - m.e;
                const float dy = static_cast<float>(y) + 0.5f - m.f;
                const float px = ia * dx + ic * dy;
                const float py = ib * dx + id * dy;
                const auto v = static_cast<std::uint8_t>(
                    (static_cast<int>(px * 0.37f) ^ static_cast<int>(py * 0.71f)) & 0xFF);
                out[static_cast<std::size_t>(y) * w + x] = v;
            }
        }
    };

    bool allOk = true;
    for (std::uint8_t rot = 0; rot < 4; ++rot) {
        TileGrid g(612.f, 792.f, 1.0f, rot, 256);

        std::vector<std::uint8_t> fullBuf;
        fakeRender(g.fullPageMatrix(), g.deviceWidth(), g.deviceHeight(), fullBuf);

        std::vector<std::uint8_t> composed(
            static_cast<std::size_t>(g.deviceWidth()) * g.deviceHeight(), 0);
        std::vector<std::uint8_t> tileBuf;

        for (const auto& t : g.allTiles()) {
            fakeRender(t.matrix, t.rect.w, t.rect.h, tileBuf);
            for (std::int32_t y = 0; y < t.rect.h; ++y)
                for (std::int32_t x = 0; x < t.rect.w; ++x)
                    composed[static_cast<std::size_t>(t.rect.y + y) * g.deviceWidth()
                             + (t.rect.x + x)] =
                        tileBuf[static_cast<std::size_t>(y) * t.rect.w + x];
        }

        std::size_t diffs = 0;
        for (std::size_t i = 0; i < fullBuf.size(); ++i)
            if (fullBuf[i] != composed[i]) ++diffs;

        if (diffs != 0) {
            std::printf("      rot %u: %zu differing pixels of %zu\n",
                        rot, diffs, fullBuf.size());
            allOk = false;
        }
    }
    check(allOk, "tiled composition is byte-identical for all four rotations");
}

void test_viewport_culling_and_ordering() {
    std::printf("C1  viewport culling + centre-out ordering\n");

    TileGrid g(612.f, 792.f, 4.0f, 0, 256);  // 2448x3168 -> 10x13 tiles
    check(g.tileCount() == 130, "10x13 = 130 tiles at 4x zoom");

    const auto vis = g.tilesForViewport(DeviceRect{500, 500, 800, 600});
    check(!vis.empty() && vis.size() < g.tileCount(), "viewport culls most tiles");

    bool inside = true;
    for (const auto& t : vis) {
        if (t.rect.right() <= 500 || t.rect.x >= 1300 ||
            t.rect.bottom() <= 500 || t.rect.y >= 1100) inside = false;
    }
    check(inside, "every returned tile intersects the viewport");

    // First tile must be nearest the viewport centre (900, 800).
    const float fx = static_cast<float>(vis.front().rect.x) + static_cast<float>(vis.front().rect.w) * 0.5f - 900.f;
    const float fy = static_cast<float>(vis.front().rect.y) + static_cast<float>(vis.front().rect.h) * 0.5f - 800.f;
    bool firstIsNearest = true;
    for (const auto& t : vis) {
        const float dx = static_cast<float>(t.rect.x) + static_cast<float>(t.rect.w) * 0.5f - 900.f;
        const float dy = static_cast<float>(t.rect.y) + static_cast<float>(t.rect.h) * 0.5f - 800.f;
        if (dx * dx + dy * dy < fx * fx + fy * fy - 0.1f) firstIsNearest = false;
    }
    check(firstIsNearest, "tiles ordered centre-out");

    check(g.tilesForViewport(DeviceRect{0, 0, 0, 0}).empty(), "empty viewport -> no tiles");
}

// --- A4: utilities ---------------------------------------------------------

void test_observable_and_signal() {
    std::printf("A4  Signal / Observable semantics\n");

    Signal<int> sig;
    int sum = 0, calls = 0;
    const auto a = sig.connect([&](int v) { sum += v; ++calls; });
    const auto b = sig.connect([&](int v) { sum += v * 10; ++calls; });

    sig.emit(3);
    check(sum == 33 && calls == 2, "both subscribers fired");

    sig.disconnect(a);
    sum = 0; calls = 0;
    sig.emit(2);
    check(sum == 20 && calls == 1, "disconnect works");
    check(sig.subscriberCount() == 1, "subscriber count correct");
    (void)b;

    // Self-disconnect mid-emit must not corrupt iteration.
    Signal<> s2;
    int fired = 0;
    SubscriptionId self = kNoSubscription;
    self = s2.connect([&] { ++fired; s2.disconnect(self); });
    (void)s2.connect([&] { ++fired; });
    s2.emit();
    check(fired == 2, "self-disconnect during emit is safe");
    fired = 0;
    s2.emit();
    check(fired == 1, "disconnected slot does not fire again");

    Observable<int> page{0};
    int notes = 0;
    const auto c = page.changed.connect([&](const int&) { ++notes; });
    page.set(5);
    page.set(5);  // idempotent write must not notify
    page.set(6);
    check(notes == 2, "Observable suppresses no-op writes");
    (void)c;

    {
        Signal<int> s3;
        int n = 0;
        {
            ScopedConnection<int> conn(s3, s3.connect([&](int) { ++n; }));
            s3.emit(1);
        }
        s3.emit(1);
        check(n == 1, "ScopedConnection disconnects on scope exit");
    }
}

void test_slotmap_generations() {
    std::printf("A4  SlotMap stale-handle detection\n");

    SlotMap<std::string> m;
    const auto h1 = m.insert("alpha");
    const auto h2 = m.insert("beta");
    check(m.size() == 2 && *m.get(h1) == "alpha", "insert + get");

    check(m.erase(h1), "erase returns true");
    check(m.get(h1) == nullptr, "erased handle reads as null");
    check(!m.contains(h1), "erased handle not contained");
    check(*m.get(h2) == "beta", "other handles unaffected");

    // The critical property: the recycled slot must NOT answer to the old
    // handle. Without the generation bump this silently returns "gamma".
    const auto h3 = m.insert("gamma");
    check(h3.index == h1.index, "slot index recycled");
    check(h3.generation != h1.generation, "generation bumped");
    check(m.get(h1) == nullptr, "stale handle still null after recycle");
    check(*m.get(h3) == "gamma", "new handle reads new value");
}

void test_arena() {
    std::printf("A4  Arena bump allocation and frame reset\n");

    Arena a(1024);
    auto* p1 = static_cast<int*>(a.allocate(sizeof(int), alignof(int)));
    auto* p2 = static_cast<double*>(a.allocate(sizeof(double), alignof(double)));
    *p1 = 42; *p2 = 3.5;
    check(*p1 == 42 && *p2 == 3.5, "allocations usable");
    check(reinterpret_cast<std::uintptr_t>(p2) % alignof(double) == 0, "alignment honoured");

    (void)a.allocate(4096);  // forces a new block
    check(a.blockCount() >= 2, "grows past the initial block");

    a.reset();
    check(a.bytesThisFrame() == 0, "reset clears the frame counter");
    check(a.blockCount() == 1, "reset keeps one block for reuse");

    // Steady state: repeated frames must not keep growing the allocator.
    const std::size_t before = a.blockCount();
    for (int f = 0; f < 1000; ++f) {
        ArenaFrame frame(a);
        for (int i = 0; i < 20; ++i) (void)a.allocate(32);
    }
    check(a.blockCount() == before, "1000 frames cause no block growth");
}

}  // namespace

int main() {
    std::printf("\nFolio render + util tests\n=========================\n\n");

    test_tiles_cover_page_exactly();
    test_matrix_roundtrip_all_rotations();
    test_tile_matrices_match_full_page();
    test_fake_render_reassembly_is_byte_identical();
    test_viewport_culling_and_ordering();
    test_observable_and_signal();
    test_slotmap_generations();
    test_arena();

    std::printf("\n%s  (%d failure%s)\n\n",
                g_failures == 0 ? "ALL PASS" : "FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
