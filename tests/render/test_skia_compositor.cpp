// Skia render backend tests.
//
// Everything here runs on a CPU raster surface. That is not a compromise: it
// is the same SkCanvas the GPU path uses, so these tests exercise the real
// drawing code, deterministically, with no display server - which is what
// makes them usable as the cross-platform golden gate (plan §16.1).

#include "PageThemeShader.h"
#include "SkiaPaintTarget.h"
#include "SkiaTileCompositor.h"
#include "../Harness.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"

#include <cstdio>
#include <vector>

using namespace folio::core;
using namespace folio::test;
using namespace folio::render;
using namespace folio::render::skia;

namespace {

sk_sp<SkSurface> makeSurface(int w, int h) {
    return SkSurfaces::Raster(
        SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType));
}

struct Rgba { int r, g, b, a; };

Rgba readPixel(SkSurface& surf, int x, int y) {
    std::uint32_t px = 0;
    const SkImageInfo info =
        SkImageInfo::Make(1, 1, kBGRA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (!surf.readPixels(info, &px, 4, x, y)) {
        return Rgba{-1, -1, -1, -1};
    }
    const auto* b = reinterpret_cast<const std::uint8_t*>(&px);
    return Rgba{b[2], b[1], b[0], b[3]};   // BGRA in memory
}

std::string show(const Rgba& c) {
    return "(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," +
           std::to_string(c.b) + "," + std::to_string(c.a) + ")";
}

bool near(const Rgba& a, const Rgba& b, int tol) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol &&
           std::abs(a.b - b.b) <= tol;
}

/// Push one solid colour through a theme's colour filter and read it back.
Rgba throughTheme(const PageTheme& theme, int r, int g, int b) {
    auto surf = makeSurface(4, 4);
    auto* c = surf->getCanvas();
    c->clear(SK_ColorTRANSPARENT);

    SkPaint p;
    p.setColor(SkColorSetARGB(255, static_cast<std::uint8_t>(r),
                              static_cast<std::uint8_t>(g),
                              static_cast<std::uint8_t>(b)));
    p.setColorFilter(makePageThemeFilter(theme));
    c->drawRect(SkRect::MakeWH(4, 4), p);
    return readPixel(*surf, 2, 2);
}

std::unique_ptr<RasterTile> solidTile(PageId page, std::uint16_t tx, std::uint16_t ty,
                                      std::int32_t w, std::int32_t h,
                                      std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    TileKey k;
    k.pageId = page;
    k.tileX = tx;
    k.tileY = ty;
    k.scaleQ = 8;
    auto t = RasterTile::make(k, w, h, 1);
    for (std::int32_t y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<std::uint8_t*>(t->row(y));
        for (std::int32_t x = 0; x < w; ++x) {
            row[x * 4 + 0] = b;      // BGRA, premultiplied; alpha 255 so == straight
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
    return t;
}

// --- the shader ------------------------------------------------------------

void test_sksl_compiles() {
    section("Skia  the page-theme SkSL compiles at start-up");
    //
    // A runtime shader that fails to compile is a blank page, on one
    // platform, for one driver. Compiling it eagerly at launch turns that
    // into a message at start-up on the developer's machine.
    //
    const char* err = nullptr;
    const bool ok = validatePageThemeShader(&err);
    check(ok, std::string("SkSL compiled: ") + (err && *err ? err : "no errors"));
    check(pageThemeSkSL() != nullptr && *pageThemeSkSL() != '\0', "shader source present");
}

void test_light_theme_is_a_true_identity() {
    section("Skia  light mode is byte-exact passthrough, with no shader bound");
    //
    // Not "visually identical" - exactly identical, and with no filter object
    // at all, so the common case does not pay for a fragment pass.
    //
    check(makePageThemeFilter(PageTheme::light()) == nullptr,
          "light mode returns no colour filter");

    const struct { int r, g, b; } samples[] = {
        {255, 255, 255}, {0, 0, 0}, {128, 128, 128}, {237, 58, 124}, {17, 34, 51}};
    for (const auto& s : samples) {
        const Rgba out = throughTheme(PageTheme::light(), s.r, s.g, s.b);
        check(out.r == s.r && out.g == s.g && out.b == s.b,
              "identity for (" + std::to_string(s.r) + "," + std::to_string(s.g) +
                  "," + std::to_string(s.b) + ") -> " + show(out));
    }
}

void test_dark_theme_inverts_luminance_not_colour() {
    section("Skia  dark mode maps luminance onto the paper/ink ramp");
    const auto dark = PageTheme::dark();

    // Paper white -> theme paper (#22252E). Black ink -> theme ink (#E1E3EA).
    const Rgba white = throughTheme(dark, 255, 255, 255);
    const Rgba black = throughTheme(dark, 0, 0, 0);
    check(near(white, Rgba{0x22, 0x25, 0x2E, 255}, 3),
          "white paper -> #22252E, got " + show(white));
    check(near(black, Rgba{0xE1, 0xE3, 0xEA, 255}, 3),
          "black ink -> #E1E3EA, got " + show(black));

    // Mid grey lands between them and stays grey (no colour cast).
    const Rgba mid = throughTheme(dark, 128, 128, 128);
    check(mid.r > 0x22 && mid.r < 0xE1, "mid grey lands on the ramp: " + show(mid));
    // "Neutral" here means "no cast of its own", not "r == g == b". The ramp
    // runs between #22252E and #E1E3EA, both of which are deliberately cool,
    // so its midpoint inherits that cast - which is the designed behaviour,
    // not a bug. What must not happen is a hue appearing out of nowhere.
    const int ramped = (0x22 + 0xE1) / 2;
    check(std::abs(mid.r - ramped) < 24 && std::abs(mid.b - mid.r) < 16,
          "and carries only the ramp's own cool cast, " + show(mid));

    // The test that naive inversion fails: a saturated red must still be red.
    // 1 - rgb would turn (220,40,40) into (35,215,215), which is cyan.
    const Rgba red = throughTheme(dark, 220, 40, 40);
    check(red.r > red.g && red.r > red.b,
          "saturated red is still predominantly red: " + show(red));
    check(red.r > 100, "and still vivid, not muddied: " + show(red));
    note("red (220,40,40) -> " + show(red) + "; naive 1-x would give (35,215,215)");
}

void test_sepia_warms_without_destroying_contrast() {
    section("Skia  sepia keeps text readable");
    const auto sepia = PageTheme::sepia();
    const Rgba paper = throughTheme(sepia, 255, 255, 255);
    const Rgba ink = throughTheme(sepia, 0, 0, 0);

    check(near(paper, Rgba{0xF1, 0xE4, 0xC8, 255}, 3),
          "white -> sepia paper, got " + show(paper));
    check(near(ink, Rgba{0x4A, 0x3A, 0x22, 255}, 3),
          "black -> sepia ink, got " + show(ink));
    check(paper.r > ink.r + 80, "contrast between paper and ink survives");
}

void test_theme_switch_reuses_the_same_pixels() {
    section("Skia  switching theme does NOT re-rasterise");
    //
    // The architectural claim from plan §8.4, asserted rather than assumed:
    // one RasterTile, three themes, three different outputs, zero renders.
    //
    auto tile = solidTile(0, 0, 0, 8, 8, 255, 255, 255);
    const TileHandle handle(std::move(tile));
    const auto* pixelsBefore = handle->pixels.data();

    std::vector<Rgba> outs;
    for (const auto id : {PageThemeId::Light, PageThemeId::Sepia, PageThemeId::Dark}) {
        auto surf = makeSurface(8, 8);
        auto* c = surf->getCanvas();
        c->clear(SK_ColorTRANSPARENT);

        SkPaint p;
        p.setColorFilter(makePageThemeFilter(PageTheme::forId(id)));
        auto image = imageForTile(handle);
        check(image != nullptr, "zero-copy image created");
        c->drawImageRect(image, SkRect::MakeWH(8, 8),
                         SkSamplingOptions{SkFilterMode::kNearest}, &p);
        outs.push_back(readPixel(*surf, 4, 4));
    }

    check(handle->pixels.data() == pixelsBefore,
          "the tile's pixel buffer was never touched");
    check(!(outs[0].r == outs[2].r && outs[0].g == outs[2].g && outs[0].b == outs[2].b),
          "light and dark produce different output from identical input");
    check(!(outs[1].r == outs[2].r && outs[1].g == outs[2].g && outs[1].b == outs[2].b),
          "sepia and dark differ too");
    note("one tile -> light " + show(outs[0]) + "  sepia " + show(outs[1]) +
         "  dark " + show(outs[2]));
}

// --- zero-copy lifetime ----------------------------------------------------

void test_image_keeps_the_tile_alive_past_eviction() {
    section("Skia  an SkImage holds its tile alive after the cache evicts it");
    RasterCache cache(2u * 1024 * 1024);
    cache.setFocus({0, 8, ColorScheme::Light});

    auto handle = cache.insert(solidTile(1, 0, 0, 64, 64, 10, 20, 30));
    check(handle != nullptr, "tile cached");
    auto image = imageForTile(handle);
    check(image != nullptr, "image wraps it");
    handle.reset();                       // caller drops its reference

    // Evict by flooding.
    for (PageId p = 100; p < 200; ++p) {
        cache.setFocus({p, 8, ColorScheme::Light});
        cache.insert(solidTile(p, 0, 0, 256, 256, 0, 0, 0));
        cache.tick();
    }
    TileKey gone;
    gone.pageId = 1;
    gone.scaleQ = 8;
    check(!cache.contains(gone), "the tile was evicted");

    // Drawing it must still work - this is the mid-frame eviction race.
    auto surf = makeSurface(64, 64);
    surf->getCanvas()->drawImage(image, 0, 0);
    const Rgba px = readPixel(*surf, 32, 32);
    check(near(px, Rgba{10, 20, 30, 255}, 1),
          "pixels are intact after eviction: " + show(px));
}

// --- compositing -----------------------------------------------------------

void test_tiles_composite_without_seams_or_colour_shift() {
    section("Skia  a 2x2 tile grid composites with no seam and no colour shift");
    RasterCache cache(16u * 1024 * 1024);
    cache.setFocus({0, 8, ColorScheme::Light});

    // Four 64x64 tiles in four colours, covering a 128x128 page.
    const std::uint8_t cols[4][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
    for (int i = 0; i < 4; ++i) {
        const auto tx = static_cast<std::uint16_t>(i % 2);
        const auto ty = static_cast<std::uint16_t>(i / 2);
        cache.insert(solidTile(0, tx, ty, 64, 64, cols[i][0], cols[i][1], cols[i][2]));
    }

    auto surf = makeSurface(128, 128);
    auto* canvas = surf->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);

    SkiaTileCompositor comp;
    comp.setTheme(PageTheme::light());

    const TileGrid grid(128.f, 128.f, 1.0f, 0, 64);
    SkiaTileCompositor::PageDraw draw;
    draw.page = 0;
    draw.dest = SkRect::MakeWH(128, 128);
    draw.scale = 1.0f;

    const auto stats = comp.drawPage(*canvas, draw, grid, cache);
    check(stats.tilesDrawn == 4, "four tiles drawn");
    check(stats.tilesExact == 4, "all four were exact-scale hits");
    check(stats.tilesMissing == 0, "nothing missing");

    // Colours survive the round trip untouched in light mode.
    check(near(readPixel(*surf, 32, 32), Rgba{255, 0, 0, 255}, 0), "top-left red exact");
    check(near(readPixel(*surf, 96, 32), Rgba{0, 255, 0, 255}, 0), "top-right green exact");
    check(near(readPixel(*surf, 32, 96), Rgba{0, 0, 255, 255}, 0), "bottom-left blue exact");
    check(near(readPixel(*surf, 96, 96), Rgba{255, 255, 0, 255}, 0), "bottom-right yellow exact");

    // The seam: every pixel on the boundary column must belong to one tile or
    // the other, never to the transparent background.
    int transparentOnSeam = 0;
    for (int y = 0; y < 128; ++y) {
        for (const int x : {63, 64}) {
            if (readPixel(*surf, x, y).a < 255) ++transparentOnSeam;
        }
    }
    check(transparentOnSeam == 0, "no transparent pixels along the tile seam");
}

void test_missing_tiles_fall_back_instead_of_blanking() {
    section("Skia  a missing tile draws a lower-resolution substitute");
    RasterCache cache(16u * 1024 * 1024);
    cache.setFocus({0, 16, ColorScheme::Light});

    // Only a half-scale version exists. The compositor must find and use it.
    TileKey low;
    low.pageId = 0;
    low.tileX = 0;
    low.tileY = 0;
    low.scaleQ = 8;
    auto t = RasterTile::make(low, 64, 64, 1);
    for (std::int32_t y = 0; y < 64; ++y) {
        auto* row = reinterpret_cast<std::uint8_t*>(t->row(y));
        for (std::int32_t x = 0; x < 64; ++x) {
            row[x * 4 + 0] = 0; row[x * 4 + 1] = 128; row[x * 4 + 2] = 255; row[x * 4 + 3] = 255;
        }
    }
    cache.insert(std::move(t));

    auto surf = makeSurface(128, 128);
    surf->getCanvas()->clear(SK_ColorTRANSPARENT);

    SkiaTileCompositor comp;
    comp.setTheme(PageTheme::light());
    const TileGrid grid(128.f, 128.f, 2.0f, 0, 256);   // one tile at 2x
    SkiaTileCompositor::PageDraw draw;
    draw.page = 0;
    draw.dest = SkRect::MakeWH(128, 128);
    draw.scale = 2.0f;

    const auto stats = comp.drawPage(*surf->getCanvas(), draw, grid, cache);
    check(stats.tilesFallback == 1, "the half-scale tile was used as a fallback");
    check(stats.tilesMissing == 0, "nothing was left blank");
    const Rgba px = readPixel(*surf, 64, 64);
    check(px.a == 255, "the page area is painted, not transparent: " + show(px));
    check(near(px, Rgba{255, 128, 0, 255}, 2), "and shows the substitute's colour");
}

// --- the paint target ------------------------------------------------------

void test_paint_target_primitives() {
    section("Skia  IPaintTarget primitives land where the design tokens say");
    auto surf = makeSurface(200, 120);
    auto* canvas = surf->getCanvas();
    SkiaPaintTarget target(canvas);

    check(target.width() == 200 && target.height() == 120, "reports its size");
    check(target.backendName() == "skia", "names itself");

    target.clear(Color::rgb(0xF6F7FB));
    check(near(readPixel(*surf, 5, 5), Rgba{0xF6, 0xF7, 0xFB, 255}, 1), "clear works");

    // radius.card = 16 from design-tokens.json.
    target.fillRoundedRect(Rect{20, 20, 160, 80}, Corners::all(16),
                           Color::rgb(0x7C3AED));
    check(near(readPixel(*surf, 100, 60), Rgba{0x7C, 0x3A, 0xED, 255}, 1),
          "the card centre is the accent colour");
    check(near(readPixel(*surf, 21, 21), Rgba{0xF6, 0xF7, 0xFB, 255}, 6),
          "and the corner is rounded away, not square");

    target.save();
    target.setBlendMode(BlendMode::SrcOver);
    target.fillCircle(Point{100, 60}, 20, Color::rgb(0xFFFFFF));
    check(near(readPixel(*surf, 100, 60), Rgba{255, 255, 255, 255}, 1), "circle drawn");
    target.restore();

    target.fillRectVerticalGradient(Rect{0, 100, 200, 20},
                                    Color::rgb(0x000000), Color::rgb(0xFFFFFF));
    const Rgba top = readPixel(*surf, 100, 101);
    const Rgba bot = readPixel(*surf, 100, 118);
    check(bot.r > top.r + 100, "gradient runs dark to light: " + show(top) + " -> " + show(bot));

    check(nearly(cssBlurToSigma(28.f), 14.0, 1e-6), "CSS blur 28 -> sigma 14");
}

void test_shadow_matches_the_css_token() {
    section("Skia  elevation.card renders as a blurred offset rrect");
    auto surf = makeSurface(200, 200);
    auto* canvas = surf->getCanvas();
    SkiaPaintTarget target(canvas);
    target.clear(Color::rgb(0xFFFFFF));

    // elevation.card = "0 6px 28px {shadow}, 0 0 0 1px {ring}"
    target.drawShadow(Rect{50, 50, 100, 100}, Corners::all(16),
                      Shadow{0.f, 6.f, 28.f, 0.f, Color{0, 0, 0, 40}});
    target.fillRoundedRect(Rect{50, 50, 100, 100}, Corners::all(16), Color::rgb(0xFFFFFF));
    target.strokeRoundedRect(Rect{50, 50, 100, 100}, Corners::all(16),
                             Color{0, 0, 0, 20}, 1.f);

    const Rgba below = readPixel(*surf, 100, 162);   // under the card, in the shadow
    const Rgba far = readPixel(*surf, 100, 198);     // well clear of it
    check(below.r < 250, "there is a shadow below the card: " + show(below));
    check(far.r > 250, "and it has faded out by the bottom: " + show(far));
    check(below.r < far.r, "shadow is darker nearer the card");
}

}  // namespace

FOLIO_TEST_MAIN("Skia render backend") {
    test_sksl_compiles();
    test_light_theme_is_a_true_identity();
    test_dark_theme_inverts_luminance_not_colour();
    test_sepia_warms_without_destroying_contrast();
    test_theme_switch_reuses_the_same_pixels();
    test_image_keeps_the_tile_alive_past_eviction();
    test_tiles_composite_without_seams_or_colour_shift();
    test_missing_tiles_fall_back_instead_of_blanking();
    test_paint_target_primitives();
    test_shadow_matches_the_css_token();
}
