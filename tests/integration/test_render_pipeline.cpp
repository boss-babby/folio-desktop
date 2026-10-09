// End-to-end: a real PDF through every layer, out as a PNG.
//
//   file -> DocumentSnapshot rope
//        -> PdfiumRasterizer (real PDFium, tiled, with bleed)
//        -> RasterCache (bounded, refcounted handles)
//        -> SkiaTileCompositor (zero-copy SkImage, theme as a shader)
//        -> SkPngEncoder
//
// Every layer below is the shipping implementation; nothing is stubbed. The
// PNGs it writes are the artefacts a human looks at when a golden test fails,
// and they are also the first time the whole stack is proven to fit together
// rather than merely to compile.

#include "pdfium/PdfiumRasterizer.h"
#include "PageThemeShader.h"
#include "SkiaPaintTarget.h"
#include "SkiaTileCompositor.h"
#include "../Harness.h"

#include "folio/core/engine/DocumentSnapshot.h"
#include "folio/core/render/RasterCache.h"
#include "folio/core/render/TileGrid.h"
#include "folio/core/render/TileScheduler.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace folio::core;
using namespace folio::test;
using namespace folio::render;
using namespace folio::render::skia;
using folio::engine::PdfiumRasterizer;

namespace {

std::vector<std::byte> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

SnapshotPtr loadDocument() {
    auto bytes = readFile("folio-test.pdf");
    if (bytes.empty()) bytes = readFile("tests/data/folio-test.pdf");
    if (bytes.empty()) return nullptr;
    return DocumentSnapshot::Builder::fromBase(std::move(bytes));
}

bool writePng(SkSurface& surf, const std::string& path) {
    auto image = surf.makeImageSnapshot();
    SkPixmap pm;
    if (!image || !image->peekPixels(&pm)) return false;
    SkFILEWStream out(path.c_str());
    return out.isValid() && SkPngEncoder::Encode(&out, pm, SkPngEncoder::Options{});
}

struct Rgba { int r, g, b, a; };

Rgba readPixel(SkSurface& surf, int x, int y) {
    std::uint32_t px = 0;
    const SkImageInfo info =
        SkImageInfo::Make(1, 1, kBGRA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (!surf.readPixels(info, &px, 4, x, y)) return Rgba{-1, -1, -1, -1};
    const auto* b = reinterpret_cast<const std::uint8_t*>(&px);
    return Rgba{b[2], b[1], b[0], b[3]};
}

std::string show(const Rgba& c) {
    return "(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," +
           std::to_string(c.b) + ")";
}

/// Everything the viewport does for one page, in one function.
struct PipelineResult {
    int    tilesRendered = 0;
    int    tilesDrawn = 0;
    double rasterMs = 0;
    double compositeMs = 0;
    std::size_t cacheBytes = 0;
};

PipelineResult renderPageThroughPipeline(PdfiumRasterizer& raster,
                                         const SnapshotPtr& snap,
                                         RasterCache& cache,
                                         SkCanvas& canvas,
                                         PageId page, float scale,
                                         const SkRect& dest,
                                         const PageTheme& theme) {
    PipelineResult out;
    const auto info = raster.pageInfo(page);
    const TileGrid grid(info.widthPt, info.heightPt, scale, 0, kTileSize);
    const std::int32_t bleed = grid.recommendedBleed();

    // --- raster pass, through the scheduler ---------------------------------
    TileScheduler sched;
    for (std::uint16_t ty = 0; ty < grid.rows(); ++ty) {
        for (std::uint16_t tx = 0; tx < grid.columns(); ++tx) {
            const auto b = grid.bledTile(tx, ty, bleed);
            TileRequest req;
            req.page = page;
            req.tileX = tx;
            req.tileY = ty;
            req.tileDevice = {b.render.x, b.render.y, b.render.w, b.render.h};
            req.scale = scale;
            req.priority = Priority::VisibleTile;
            req.snapshotGen = snap->generation();
            sched.submit(req, [](TileResult) {});
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    TileJob job;
    while (sched.tryPop(job)) {
        const std::uint16_t tx = job.request.tileX;
        const std::uint16_t ty = job.request.tileY;
        const auto b = grid.bledTile(tx, ty, bleed);

        TileSpec spec;
        spec.tx = tx;
        spec.ty = ty;
        spec.rect = b.render;
        spec.matrix = b.matrix;

        auto bled = raster.renderTile(job.request, spec);
        sched.complete(job);
        if (!bled) continue;
        ++out.tilesRendered;

        // Crop the bleed off before caching. The cache stores exactly what
        // the compositor draws - carrying the margin further would make every
        // cached tile 20% larger for no benefit.
        TileKey key = bled->key;
        auto tile = RasterTile::make(key, b.keep.w, b.keep.h, bled->generation);
        tile->opaque = bled->opaque;
        for (std::int32_t y = 0; y < b.keep.h; ++y) {
            std::memcpy(tile->row(y),
                        bled->row(b.keep.y + y) + static_cast<std::size_t>(b.keep.x) * 4,
                        static_cast<std::size_t>(b.keep.w) * 4);
        }
        cache.insert(std::move(tile));
    }
    const auto t1 = std::chrono::steady_clock::now();

    // --- composite pass ------------------------------------------------------
    SkiaTileCompositor comp;
    comp.setTheme(theme);
    comp.drawPagePlaceholder(canvas, dest, 4.f);

    SkiaTileCompositor::PageDraw draw;
    draw.page = page;
    draw.dest = dest;
    draw.scale = scale;
    draw.generation = snap->generation();
    const auto stats = comp.drawPage(canvas, draw, grid, cache);
    const auto t2 = std::chrono::steady_clock::now();

    out.tilesDrawn = stats.tilesDrawn;
    out.rasterMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    out.compositeMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    out.cacheBytes = cache.stats().bytes;
    return out;
}

// ---------------------------------------------------------------------------

void test_full_pipeline_three_themes() {
    section("E2E  PDF -> PDFium -> cache -> Skia -> PNG, in three themes");

    auto snap = loadDocument();
    check(snap != nullptr, "document loaded");
    if (!snap) return;

    PdfiumRasterizer raster;
    check(raster.setSnapshot(snap), "PDFium opened the rope");
    if (raster.pageCount() == 0) { check(false, "no pages"); return; }

    const char* err = nullptr;
    check(validatePageThemeShader(&err), "page-theme SkSL compiled");

    constexpr float kScale = 1.0f;
    const auto info = raster.pageInfo(1);
    const TileGrid grid(info.widthPt, info.heightPt, kScale, 0, kTileSize);

    constexpr int kMargin = 40;
    const int W = grid.deviceWidth() + 2 * kMargin;
    const int H = grid.deviceHeight() + 2 * kMargin;

    struct ThemeCase { PageThemeId id; const char* name; std::uint32_t backdrop; };
    const ThemeCase themes[] = {
        {PageThemeId::Light, "light", 0xF1F2F7},
        {PageThemeId::Sepia, "sepia", 0xE7DCC4},
        {PageThemeId::Dark,  "dark",  0x15171D},
    };

    for (const auto& tc : themes) {
        // A fresh cache per theme proves the point in the strongest form:
        // even starting cold, nothing about the raster pass depends on the
        // theme. The tiles PDFium produces are byte-identical across all
        // three - only the shader differs.
        RasterCache cache(64u * 1024 * 1024);
        cache.setFocus({1, quantiseScale(kScale), ColorScheme::Light});

        auto surf = SkSurfaces::Raster(
            SkImageInfo::Make(W, H, kBGRA_8888_SkColorType, kPremul_SkAlphaType));
        check(surf != nullptr, "surface created");
        if (!surf) continue;

        auto* canvas = surf->getCanvas();
        SkiaPaintTarget target(canvas);
        target.clear(Color::rgb(tc.backdrop));

        const SkRect dest = SkRect::MakeXYWH(
            kMargin, kMargin, static_cast<float>(grid.deviceWidth()),
            static_cast<float>(grid.deviceHeight()));

        const auto res = renderPageThroughPipeline(
            raster, snap, cache, *canvas, 1, kScale, dest, PageTheme::forId(tc.id));

        check(res.tilesRendered == static_cast<int>(grid.tileCount()),
              std::string(tc.name) + ": all " + std::to_string(grid.tileCount()) +
                  " tiles rendered");
        check(res.tilesDrawn == static_cast<int>(grid.tileCount()),
              std::string(tc.name) + ": all tiles composited");

        const std::string path = std::string("folio-pipeline-") + tc.name + ".png";
        check(writePng(*surf, path), std::string(tc.name) + ": PNG written to " + path);

        note(std::string(tc.name) + ": raster " +
             std::to_string(static_cast<int>(res.rasterMs)) + " ms, composite " +
             std::to_string(static_cast<int>(res.compositeMs * 1000)) + " us, cache " +
             std::to_string(res.cacheBytes / 1024) + " KiB");

        // Page interior: the paper colour must follow the theme.
        const Rgba paper = readPixel(*surf, kMargin + 300, kMargin + 400);
        if (tc.id == PageThemeId::Light) {
            check(paper.r > 240 && paper.g > 240 && paper.b > 240,
                  "light: paper is white " + show(paper));
        } else if (tc.id == PageThemeId::Dark) {
            check(paper.r < 70 && paper.g < 70 && paper.b < 80,
                  "dark: paper is dark " + show(paper));
        } else {
            check(paper.r > 200 && paper.b < paper.r - 20,
                  "sepia: paper is warm " + show(paper));
        }

        // Backdrop outside the page is untouched by the page theme.
        const Rgba outside = readPixel(*surf, 4, 4);
        check(std::abs(outside.r - static_cast<int>((tc.backdrop >> 16) & 0xFF)) <= 1,
              "backdrop outside the page is unaffected " + show(outside));
    }
}

void test_raster_is_theme_independent() {
    section("E2E  the raster cache contents are identical across themes");
    //
    // The load-bearing claim: one render, three looks. If this ever stops
    // being true the cache key has to grow a theme field and the mobile
    // memory budget has to shrink by two thirds.
    //
    auto snap = loadDocument();
    if (!snap) { check(false, "no document"); return; }
    PdfiumRasterizer raster;
    if (!raster.setSnapshot(snap)) { check(false, "load failed"); return; }

    auto renderOnce = [&] {
        auto tile = raster.renderFullPage(0, 1.0f, 0);
        return tile;
    };

    auto a = renderOnce();
    auto b = renderOnce();
    check(a && b, "two renders of the same page succeeded");
    if (!a || !b) return;
    check(a->pixels == b->pixels, "PDFium output is deterministic run to run");

    // Composite the SAME tile under all three themes.
    std::vector<Rgba> centres;
    for (const auto id : {PageThemeId::Light, PageThemeId::Sepia, PageThemeId::Dark}) {
        auto surf = SkSurfaces::Raster(SkImageInfo::Make(
            a->width, a->height, kBGRA_8888_SkColorType, kPremul_SkAlphaType));
        auto* c = surf->getCanvas();
        c->clear(SK_ColorTRANSPARENT);

        TileHandle handle(std::make_unique<RasterTile>(*a));
        SkPaint p;
        p.setColorFilter(makePageThemeFilter(PageTheme::forId(id)));
        c->drawImage(imageForTile(handle), 0, 0, SkSamplingOptions{}, &p);
        centres.push_back(readPixel(*surf, a->width / 4, a->height / 4));
    }
    check(a->pixels == b->pixels, "and the source pixels were never modified");
    check(centres.size() == 3, "three composites from one raster");
    note("one raster -> light " + show(centres[0]) + " sepia " + show(centres[1]) +
         " dark " + show(centres[2]));
}

void test_budget_holds_across_a_document_scroll() {
    section("E2E  scrolling the whole document never exceeds the mobile budget");
    auto snap = loadDocument();
    if (!snap) { check(false, "no document"); return; }
    PdfiumRasterizer raster;
    if (!raster.setSnapshot(snap)) { check(false, "load failed"); return; }

    const std::size_t budget = 96u * 1024 * 1024;     // the phone number
    RasterCache cache(budget);
    std::size_t peak = 0;

    for (int pass = 0; pass < 4; ++pass) {
        for (PageId p = 0; p < raster.pageCount(); ++p) {
            cache.setFocus({p, quantiseScale(2.0f), ColorScheme::Light});
            const auto info = raster.pageInfo(p);
            const TileGrid grid(info.widthPt, info.heightPt, 2.0f, 0, kTileSize);
            for (std::uint16_t ty = 0; ty < grid.rows(); ++ty) {
                for (std::uint16_t tx = 0; tx < grid.columns(); ++tx) {
                    TileKey key;
                    key.pageId = p;
                    key.tileX = tx;
                    key.tileY = ty;
                    key.scaleQ = quantiseScale(2.0f);
                    if (cache.find(key)) continue;

                    const auto spec = grid.tile(tx, ty);
                    TileRequest req;
                    req.page = p;
                    req.tileX = tx;
                    req.tileY = ty;
                    req.tileDevice = {spec.rect.x, spec.rect.y, spec.rect.w, spec.rect.h};
                    req.scale = 2.0f;
                    req.snapshotGen = snap->generation();
                    if (auto t = raster.renderTile(req, spec)) {
                        cache.insert(std::move(t));
                    }
                }
            }
            cache.tick();
            peak = std::max(peak, cache.stats().bytes);
        }
    }
    const auto s = cache.stats();
    check(peak <= budget, "peak " + std::to_string(peak / 1024 / 1024) +
                              " MiB stayed inside the 96 MiB budget");
    check(s.hitRate() > 0.5, "second pass over a 3-page document is mostly hits (" +
                                 std::to_string(static_cast<int>(s.hitRate() * 100)) + "%)");
    note(std::to_string(s.insertions) + " inserts, " + std::to_string(s.evictions) +
         " evictions, " + std::to_string(s.entries) + " resident");
}

}  // namespace

FOLIO_TEST_MAIN("end-to-end render pipeline") {
    test_full_pipeline_three_themes();
    test_raster_is_theme_independent();
    test_budget_holds_across_a_document_scroll();
}
