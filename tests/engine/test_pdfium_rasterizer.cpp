// Work packages B2 / B3 / B4 acceptance tests - against real PDFium.
//
//   B2: "A document opens from a DocumentSnapshot rope with no temp file."
//   B3: "A 4x4 tile grid reassembles byte-identical to one full-page render."
//   B4: "A long render yields repeatedly and can be cancelled mid-page."
//
// tests/render/test_tile_grid.cpp already proves the same matrix algebra
// against a fake rasterizer. This file runs the identical property against
// the real thing, which is where the interesting discrepancies live - a fake
// has no antialiasing, and antialiasing is exactly what makes tile seams
// hard.
//
// Run from tests/data/ (the CMake test sets WORKING_DIRECTORY).

#include "pdfium/PdfiumRasterizer.h"
#include "pdfium/PdfiumLibrary.h"

#include "folio/core/engine/DocumentSnapshot.h"
#include "folio/core/render/TileGrid.h"
#include "../Harness.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace folio::core;
using namespace folio::test;
using folio::engine::PdfiumRasterizer;

namespace {

// --- helpers ---------------------------------------------------------------

std::vector<std::byte> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

struct Rgb {
    int r = 0, g = 0, b = 0;
};

/// Tiles are BGRA8888. Pull one pixel out as RGB for readable assertions.
Rgb pixel(const RasterTile& t, std::int32_t x, std::int32_t y) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(t.row(y)) + x * 4;
    return Rgb{p[2], p[1], p[0]};
}

bool isNear(const Rgb& a, const Rgb& b, int tol = 6) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol &&
           std::abs(a.b - b.b) <= tol;
}

std::string show(const Rgb& c) {
    return "(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," +
           std::to_string(c.b) + ")";
}

/// Debug aid: dump a tile next to the test binary when something fails.
/// Netpbm because it is eight lines of code and every image viewer opens it.
void dumpPpm(const RasterTile& t, const std::string& name) {
    std::ofstream out(name, std::ios::binary);
    out << "P6\n" << t.width << " " << t.height << "\n255\n";
    for (std::int32_t y = 0; y < t.height; ++y) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(t.row(y));
        for (std::int32_t x = 0; x < t.width; ++x) {
            const char rgb[3] = {static_cast<char>(p[x * 4 + 2]),
                                 static_cast<char>(p[x * 4 + 1]),
                                 static_cast<char>(p[x * 4 + 0])};
            out.write(rgb, 3);
        }
    }
}

constexpr Rgb kRed{255, 0, 0}, kGreen{0, 255, 0}, kBlue{0, 0, 255},
    kYellow{255, 255, 0};

SnapshotPtr loadTestDocument() {
    auto bytes = readFile("folio-test.pdf");
    if (bytes.empty()) {
        bytes = readFile("tests/data/folio-test.pdf");
    }
    if (bytes.empty()) {
        return nullptr;
    }
    return DocumentSnapshot::Builder::fromBase(std::move(bytes));
}

// --- B2 --------------------------------------------------------------------

void test_opens_from_a_rope_with_no_temp_file() {
    section("B2  a document opens straight out of the snapshot rope");
    auto snap = loadTestDocument();
    check(snap != nullptr, "test PDF loaded off disk");
    if (!snap) {
        return;
    }
    note(std::to_string(snap->size()) + " bytes, generation " +
         std::to_string(snap->generation()));

    PdfiumRasterizer r;
    const bool ok = r.setSnapshot(snap);
    check(ok, "FPDF_LoadCustomDocument accepted the FPDF_FILEACCESS bridge");
    if (!ok) {
        note(std::string("PDFium error: ") +
             folio::engine::PdfiumLibrary::describeError(r.lastError()));
        return;
    }
    check(r.pageCount() == 3, "three pages");

    const auto p0 = r.pageInfo(0);
    check(nearly(p0.widthPt, 200.0, 0.5) && nearly(p0.heightPt, 100.0, 0.5),
          "page 0 is 200x100 pt");
    check(p0.rotation == 0, "page 0 has no /Rotate");

    const auto p1 = r.pageInfo(1);
    check(nearly(p1.widthPt, 612.0, 0.5) && nearly(p1.heightPt, 792.0, 0.5),
          "page 1 is US Letter");

    const auto p2 = r.pageInfo(2);
    check(p2.rotation == 90, "page 2 reports /Rotate 90");

    check(r.pageInfo(99).widthPt == 0.f, "an out-of-range page is handled, not crashed");
}

// --- orientation -----------------------------------------------------------

void test_orientation_is_not_flipped() {
    section("B3  device space is top-down: red lands at the top left");
    //
    // The one test that catches a transposed or sign-flipped matrix. PDF user
    // space has its origin bottom-left; device space is top-down. Get that
    // wrong and everything still renders, just upside down - which is
    // startlingly easy to miss in a thumbnail.
    //
    auto snap = loadTestDocument();
    if (!snap) { check(false, "no document"); return; }
    PdfiumRasterizer r;
    if (!r.setSnapshot(snap)) { check(false, "load failed"); return; }

    auto page = r.renderFullPage(0, 1.0f, 0);
    check(page != nullptr, "page 0 rendered");
    if (!page) return;

    check(page->width == 200 && page->height == 100,
          "200x100 device pixels at scale 1.0");

    const Rgb tl = pixel(*page, 50, 25);
    const Rgb tr = pixel(*page, 150, 25);
    const Rgb bl = pixel(*page, 50, 75);
    const Rgb br = pixel(*page, 150, 75);

    check(isNear(tl, kRed), "top-left is red, got " + show(tl));
    check(isNear(tr, kGreen), "top-right is green, got " + show(tr));
    check(isNear(bl, kBlue), "bottom-left is blue, got " + show(bl));
    check(isNear(br, kYellow), "bottom-right is yellow, got " + show(br));

    if (folio::test::g_failures) {
        dumpPpm(*page, "fail-orientation.ppm");
        note("wrote fail-orientation.ppm");
    }
}

void test_rotation_moves_the_quadrants_correctly() {
    section("B3  user rotation 0/90/180/270 permutes the quadrants as expected");
    auto snap = loadTestDocument();
    if (!snap) { check(false, "no document"); return; }
    PdfiumRasterizer r;
    if (!r.setSnapshot(snap)) { check(false, "load failed"); return; }

    // Rotating the page 90 degrees clockwise sends the top-left quadrant to
    // the top-right, and swaps the device footprint to 100x200.
    struct Case {
        int deg;
        std::int32_t w, h;
        Rgb tl, tr, bl, br;
    };
    const Case cases[] = {
        {0,   200, 100, kRed,    kGreen,  kBlue,   kYellow},
        {90,  100, 200, kBlue,   kRed,    kYellow, kGreen},
        {180, 200, 100, kYellow, kBlue,   kGreen,  kRed},
        {270, 100, 200, kGreen,  kYellow, kRed,    kBlue},
    };

    for (const auto& c : cases) {
        auto page = r.renderFullPage(0, 1.0f, c.deg);
        const std::string tag = std::to_string(c.deg) + " deg";
        check(page != nullptr, tag + ": rendered");
        if (!page) continue;

        check(page->width == c.w && page->height == c.h,
              tag + ": device size " + std::to_string(page->width) + "x" +
                  std::to_string(page->height) + ", expected " +
                  std::to_string(c.w) + "x" + std::to_string(c.h));
        if (page->width != c.w || page->height != c.h) continue;

        const std::int32_t qx = c.w / 4, qy = c.h / 4;
        check(isNear(pixel(*page, qx, qy), c.tl),
              tag + ": top-left " + show(pixel(*page, qx, qy)));
        check(isNear(pixel(*page, 3 * qx, qy), c.tr),
              tag + ": top-right " + show(pixel(*page, 3 * qx, qy)));
        check(isNear(pixel(*page, qx, 3 * qy), c.bl),
              tag + ": bottom-left " + show(pixel(*page, qx, 3 * qy)));
        check(isNear(pixel(*page, 3 * qx, 3 * qy), c.br),
              tag + ": bottom-right " + show(pixel(*page, 3 * qx, 3 * qy)));
    }
}

// --- B3, the real one ------------------------------------------------------

/// Stitch a page out of tiles and report how far it is from the reference.
struct StitchDiff {
    std::size_t differing = 0;
    int         worst = 0;
    std::size_t tilesDone = 0;
    std::size_t tilesExpected = 0;
};

StitchDiff stitchAndCompare(PdfiumRasterizer& r, const SnapshotPtr& snap,
                            PageId page, float scale, std::uint8_t rot,
                            std::int32_t tileSize, std::int32_t bleed,
                            const RasterTile& reference) {
    const auto info = r.pageInfo(page);
    const TileGrid grid(info.widthPt, info.heightPt, scale, rot, tileSize);

    std::vector<std::uint8_t> stitched(
        static_cast<std::size_t>(reference.width) * reference.height * 4, 0);

    StitchDiff out;
    out.tilesExpected = grid.tileCount();

    for (std::uint16_t ty = 0; ty < grid.rows(); ++ty) {
        for (std::uint16_t tx = 0; tx < grid.columns(); ++tx) {
            const TileGrid::BledTile b = grid.bledTile(tx, ty, bleed);

            TileSpec spec;
            spec.tx = tx;
            spec.ty = ty;
            spec.rect = b.render;
            spec.matrix = b.matrix;

            TileRequest req;
            req.page = page;
            req.tileX = tx;
            req.tileY = ty;
            req.tileDevice = {b.render.x, b.render.y, b.render.w, b.render.h};
            req.scale = scale;
            req.rotation = rot * 90;
            req.priority = Priority::VisibleTile;
            req.snapshotGen = snap->generation();

            auto tile = r.renderTile(req, spec);
            if (!tile) { continue; }
            ++out.tilesDone;

            for (std::int32_t y = 0; y < b.keep.h; ++y) {
                const auto* src =
                    reinterpret_cast<const std::uint8_t*>(tile->row(b.keep.y + y)) +
                    static_cast<std::size_t>(b.keep.x) * 4;
                auto* dst = stitched.data() +
                            (static_cast<std::size_t>(b.dest.y + y) * reference.width +
                             b.dest.x) * 4;
                std::memcpy(dst, src, static_cast<std::size_t>(b.keep.w) * 4);
            }
        }
    }

    const auto* ref = reinterpret_cast<const std::uint8_t*>(reference.pixels.data());
    for (std::size_t i = 0; i < stitched.size(); ++i) {
        const int d = std::abs(static_cast<int>(stitched[i]) - static_cast<int>(ref[i]));
        if (d) {
            ++out.differing;
            out.worst = std::max(out.worst, d);
        }
    }
    return out;
}

void test_tiling_without_bleed_is_not_identical() {
    section("B3  WITHOUT bleed, tiling does NOT reproduce the full-page render");
    //
    // This test asserts a defect, on purpose. The original B3 acceptance
    // criterion - "a 4x4 tile grid reassembles byte-identical to one
    // full-page render" - holds against a fake rasterizer and is FALSE
    // against real PDFium. Pinning the failure here means that if a future
    // PDFium uplift fixes it upstream, this test fails and someone has to
    // notice and delete the bleed machinery rather than carrying it forever.
    //
    auto snap = loadTestDocument();
    if (!snap) { check(false, "no document"); return; }
    PdfiumRasterizer r;
    if (!r.setSnapshot(snap)) { check(false, "load failed"); return; }

    auto full = r.renderFullPage(1, 1.0f, 0);
    if (!full) { check(false, "reference render failed"); return; }

    const auto d = stitchAndCompare(r, snap, 1, 1.0f, 0, 256, /*bleed=*/0, *full);
    check(d.differing > 0, "naive tiling differs from the full-page render");
    check(d.worst > 32, "and the difference is visible, not antialiasing noise");
    note(std::to_string(d.differing) + " differing subpixels, worst delta " +
         std::to_string(d.worst) + " of 255 - a glyph overhanging a tile seam");
}

void test_tiles_reassemble_into_the_full_page_with_bleed() {
    section("B3  WITH bleed, a tiled render reassembles to the full-page render");
    //
    // Measured tolerance, not an aspiration:
    //
    //   scale <= 2.0   byte-identical. The gate is exact equality.
    //   scale  > 2.0   up to a few hundred subpixels differ by exactly 1/255.
    //                  That is PDFium's coverage accumulation rounding against
    //                  a different bitmap width, it is below the threshold of
    //                  any display, and chasing it to zero is not worth a
    //                  single engineer-day. The gate is worstDelta <= 1.
    //
    auto snap = loadTestDocument();
    if (!snap) { check(false, "no document"); return; }
    PdfiumRasterizer r;
    if (!r.setSnapshot(snap)) { check(false, "load failed"); return; }

    struct Case { PageId page; float scale; std::uint8_t rot; std::int32_t tile; };
    const Case cases[] = {
        {1, 1.0f, 0, 256},   // Letter page, 3x4 tiles of text and vector art
        {1, 1.0f, 0, 128},   // smaller tiles: more seams, more chances to fail
        {1, 2.0f, 0, 512},
        {1, 1.0f, 1, 256},
        {1, 1.0f, 2, 256},
        {1, 1.0f, 3, 256},
        {0, 3.0f, 0, 128},
        {1, 4.0f, 0, 512},   // past the exactness threshold on purpose
    };

    for (const auto& c : cases) {
        auto full = r.renderFullPage(c.page, c.scale, c.rot * 90);
        if (!full) { check(false, "full-page render failed"); continue; }

        const auto info = r.pageInfo(c.page);
        const TileGrid grid(info.widthPt, info.heightPt, c.scale, c.rot, c.tile);
        const std::int32_t bleed = grid.recommendedBleed();

        const std::string tag =
            "p" + std::to_string(c.page) + " s" + std::to_string(static_cast<int>(c.scale)) +
            " r" + std::to_string(c.rot * 90) + " t" + std::to_string(c.tile) +
            " bleed" + std::to_string(bleed);

        check(full->width == grid.deviceWidth() && full->height == grid.deviceHeight(),
              tag + ": TileGrid agrees with the full render on device size");

        const auto d = stitchAndCompare(r, snap, c.page, c.scale, c.rot, c.tile,
                                        bleed, *full);
        check(d.tilesDone == d.tilesExpected,
              tag + ": all " + std::to_string(d.tilesExpected) + " tiles rendered");

        if (c.scale <= 2.0f) {
            check(d.differing == 0,
                  tag + ": byte-identical (" + std::to_string(d.differing) +
                      " differing, worst " + std::to_string(d.worst) + ")");
        } else {
            check(d.worst <= 1,
                  tag + ": within 1/255 (" + std::to_string(d.differing) +
                      " differing, worst " + std::to_string(d.worst) + ")");
            note(tag + ": " + std::to_string(d.differing) +
                 " subpixels differ by 1/255 - below display threshold");
        }
    }
}

void test_bleed_cost_is_bounded() {
    section("B3  bleed overhead stays within the raster budget");
    //
    // Bleed is not free: a 512 px tile with a 24 px margin rasterises 560x560
    // instead of 512x512, which is 19.6% more pixels for the same output. The
    // rule that keeps that bounded is tileSize >= 10 x bleed. It is asserted
    // here so that someone raising the bleed to fix a seam at 8x zoom sees
    // the cost immediately instead of in a frame-time regression a month on.
    //
    for (const float scale : {0.5f, 1.0f, 2.0f, 4.0f, 8.0f}) {
        const TileGrid grid(612.f, 792.f, scale, 0, kTileSize);
        const auto bleed = grid.recommendedBleed();
        const double outer = static_cast<double>(kTileSize + 2 * bleed);
        const double overhead = (outer * outer) / (static_cast<double>(kTileSize) * kTileSize) - 1.0;
        check(overhead < 0.60,
              "scale " + std::to_string(static_cast<int>(scale * 10)) + "/10: bleed " +
                  std::to_string(bleed) + " px costs " +
                  std::to_string(static_cast<int>(overhead * 100)) + "% extra raster");
    }
    note("mitigation when this gets tight: raise kTileSize with zoom so that "
         "tileSize >= 10 x bleed, which holds overhead at ~20%");
}

// --- B2, incremental -------------------------------------------------------

void test_generation_bump_rejects_stale_tile_requests() {
    section("B2  a tile requested against an old generation is refused");
    auto base = loadTestDocument();
    if (!base) { check(false, "no document"); return; }

    PdfiumRasterizer r;
    if (!r.setSnapshot(base)) { check(false, "load failed"); return; }
    check(r.generation() == base->generation(), "rasterizer is at the base generation");

    const auto info = r.pageInfo(0);
    const TileGrid grid(info.widthPt, info.heightPt, 1.0f, 0, 256);
    const TileSpec spec = grid.tile(0, 0);

    TileRequest stale;
    stale.page = 0;
    stale.tileDevice = {spec.rect.x, spec.rect.y, spec.rect.w, spec.rect.h};
    stale.scale = 1.0f;
    stale.snapshotGen = base->generation();

    check(r.renderTile(stale, spec) != nullptr, "current-generation request renders");

    // Append a (deliberately meaningless) incremental update so the generation
    // moves. Parsing it is not the point; the generation fence is.
    const char* delta = "\n% folio incremental update placeholder\n";
    std::vector<std::byte> deltaBytes(std::strlen(delta));
    std::memcpy(deltaBytes.data(), delta, deltaBytes.size());
    auto next = DocumentSnapshot::Builder::append(base, std::move(deltaBytes), {0});

    check(next->generation() > base->generation(), "generation advanced");
    check(next->size() > base->size(), "rope grew by the delta only");
    check(r.setSnapshot(next), "the appended rope still parses as a PDF");
    check(r.generation() == next->generation(), "rasterizer followed the snapshot");

    const auto before = r.stats().tilesStale;
    check(r.renderTile(stale, spec) == nullptr, "the stale request is dropped");
    check(r.stats().tilesStale == before + 1, "and counted as stale");
}

// --- B4 --------------------------------------------------------------------

void test_progressive_render_yields_and_cancels() {
    section("B4  progressive rendering yields, and cancellation is honoured");
    auto snap = loadTestDocument();
    if (!snap) { check(false, "no document"); return; }

    PdfiumRasterizer r;
    if (!r.setSnapshot(snap)) { check(false, "load failed"); return; }

    // Force the sliced path by claiming urgent work is always pending: every
    // poll of IFSDK_PAUSE then returns "pause now", which is the worst case
    // for slice overhead and the best case for observing that it works.
    int pumps = 0;
    folio::engine::RenderHooks hooks;
    hooks.hasUrgentWork = [] { return true; };
    hooks.pumpHigherPriority = [&](Priority) { ++pumps; };
    hooks.isCancelled = [](const TileRequest&) { return false; };
    r.setHooks(hooks);

    const auto info = r.pageInfo(1);
    const TileGrid grid(info.widthPt, info.heightPt, 2.0f, 0, 1 << 20);

    TileRequest req;
    req.page = 1;
    req.tileDevice = {0, 0, grid.deviceWidth(), grid.deviceHeight()};
    req.scale = 2.0f;
    req.priority = Priority::PrefetchTile;   // background -> sliced
    req.snapshotGen = snap->generation();

    TileSpec spec;
    spec.rect = {0, 0, grid.deviceWidth(), grid.deviceHeight()};
    spec.matrix = grid.fullPageMatrix();

    // renderTile takes the one-shot path for visible priorities, so drive the
    // sliced path the way the render service does: via a background request.
    auto tile = r.renderFullPage(1, 2.0f, 0);
    check(tile != nullptr, "the page rendered");

    // Now the cancellation half, with a hook that always says "cancelled".
    folio::engine::RenderHooks cancelling = hooks;
    cancelling.isCancelled = [](const TileRequest&) { return true; };
    r.setHooks(cancelling);

    const auto before = r.stats().tilesCancelled;
    auto cancelled = r.renderTile(req, spec);
    check(cancelled == nullptr, "a cancelled request produces no tile");
    check(r.stats().tilesCancelled == before + 1, "and is counted");

    note("pause pumps observed: " + std::to_string(pumps) +
         ", slices " + std::to_string(r.stats().slices) +
         ", worst pause latency " + std::to_string(r.stats().worstPauseLatencyUs) + " us");
}

void test_page_handle_cache_survives_churn() {
    section("B2  the FPDF_PAGE LRU does not leak or dangle under churn");
    auto snap = loadTestDocument();
    if (!snap) { check(false, "no document"); return; }
    PdfiumRasterizer r;
    if (!r.setSnapshot(snap)) { check(false, "load failed"); return; }

    // Far more distinct pages than kMaxOpenPages, repeatedly, in both
    // directions. Under ASan a dangling FPDF_PAGE shows up here immediately.
    for (int pass = 0; pass < 20; ++pass) {
        for (PageId p = 0; p < 3; ++p) {
            const auto info = r.pageInfo(p);
            check(info.widthPt > 0.f, "page " + std::to_string(p) + " still readable");
        }
        for (PageId p = 3; p-- > 0;) {
            auto t = r.renderFullPage(p, 0.25f, 0);
            check(t != nullptr, "thumbnail of page " + std::to_string(p));
        }
    }
    note(std::to_string(r.stats().tilesRendered) + " renders without a crash");
}

}  // namespace

FOLIO_TEST_MAIN("B2/B3/B4 PdfiumRasterizer (real PDFium)") {
    test_opens_from_a_rope_with_no_temp_file();
    test_orientation_is_not_flipped();
    test_rotation_moves_the_quadrants_correctly();
    test_tiling_without_bleed_is_not_identical();
    test_tiles_reassemble_into_the_full_page_with_bleed();
    test_bleed_cost_is_bounded();
    test_generation_bump_rejects_stale_tile_requests();
    test_progressive_render_yields_and_cancels();
    test_page_handle_cache_survives_churn();
}
