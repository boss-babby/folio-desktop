// Viewport-only PDFium work and visual-cache lifetime, without a display server.
#include "pdfium/PdfiumPageSource.h"
#include "pdfium/PdfiumRasterizer.h"
#include "components/Edits.h"
#include "components/Controls.h"
#include "../Harness.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>
#include <vector>

using namespace folio::render;
using namespace folio::test;

namespace {
class SelectionSource final : public folio::ui::IPageSource {
public:
    std::vector<folio::ui::PageGlyph> text{
        {U'A', .10f, .08f, .15f, .105f},
        {U'B', .15f, .08f, .20f, .105f},
        {U'C', .10f, .14f, .15f, .165f},
        {U'D', .15f, .14f, .20f, .165f},
    };
    int pageCount() const override { return 1; }
    folio::ui::PageSizePt pageSize(int) const override { return {600.f, 800.f}; }
    void drawPage(IPaintTarget&, int, const Rect&, float,
                  folio::ui::PageTheme) override {}
    const std::vector<folio::ui::PageGlyph>* glyphs(int) override { return &text; }
};

class CachedSelectionSource final : public folio::ui::IPageSource {
public:
    std::vector<folio::ui::PageGlyph> text{
        {U'A', .10f, .08f, .15f, .105f},
        {U'B', .15f, .08f, .20f, .105f},
        {U'\n', 0.f, 0.f, 0.f, 0.f},
        {U'C', .10f, .14f, .15f, .165f},
        {U'D', .15f, .14f, .20f, .165f},
    };
    int cachedBlits = 0;
    std::vector<Rect> cachedSamples;
    int pageCount() const override { return 1; }
    folio::ui::PageSizePt pageSize(int) const override { return {600.f, 800.f}; }
    void drawPage(IPaintTarget&, int, const Rect&, float,
                  folio::ui::PageTheme) override {}
    bool drawCachedRegion(IPaintTarget&, int, const Rect&, const Rect& sample,
                          float, folio::ui::PageTheme) override {
        cachedSamples.push_back(sample);
        ++cachedBlits;
        return true;
    }
    const std::vector<folio::ui::PageGlyph>* glyphs(int) override { return &text; }
};

// A punctuation glyph can sit below the body font's tight boxes. This is a
// synthetic analogue of the comma fragments in the reported phone capture;
// the reporter's actual 335-page PDF has not been supplied.
// Almost touching baselines reproduce the paragraph-wide rectangle from the
// screenshot: a growable vertical tolerance would absorb each following row.
class TightParagraphSource final : public folio::ui::IPageSource {
public:
    std::vector<folio::ui::PageGlyph> text;
    explicit TightParagraphSource(bool withTextSeparators = false) {
        for (int row = 0; row < 10; ++row) {
            const float y = .10f + static_cast<float>(row) * .035f;
            for (int col = 0; col < 8; ++col) {
                const float x = .10f + static_cast<float>(col) * .026f;
                text.push_back({U'A', x, y, x + .025f, y + .031f});
            }
            // Its centre sits below the body box, but must not join row+1.
            text.push_back({U',', .308f, y + .029f, .319f, y + .039f});
            if (withTextSeparators)
                text.push_back({U'\n', 0.f, 0.f, 0.f, 0.f});
        }
    }
    int pageCount() const override { return 1; }
    folio::ui::PageSizePt pageSize(int) const override { return {600.f, 800.f}; }
    void drawPage(IPaintTarget&, int, const Rect&, float,
                  folio::ui::PageTheme) override {}
    const std::vector<folio::ui::PageGlyph>* glyphs(int) override { return &text; }
};

// One line, three words with font-dependent glyph heights/vertical origins
// and a generous PDF word-space; the next row begins at the left margin.
class MixedFontWordSource final : public folio::ui::IPageSource {
public:
    std::vector<folio::ui::PageGlyph> text{
        {U'R', .10f, .10f, .12f, .13f},
        {U'o', .12f, .10f, .14f, .13f},
        {U'c', .14f, .10f, .16f, .13f},
        {U'k', .16f, .10f, .18f, .13f},
        {U' ', 0.f, 0.f, 0.f, 0.f},
        {U'M', .213f, .114f, .233f, .144f},
        {U'a', .233f, .114f, .253f, .144f},
        {U's', .253f, .114f, .273f, .144f},
        {U's', .273f, .114f, .293f, .144f},
        {U'e', .293f, .114f, .313f, .144f},
        {U's', .313f, .114f, .333f, .144f},
        {U'T', .10f, .169f, .12f, .199f},
        {U'e', .12f, .169f, .14f, .199f},
        {U's', .14f, .169f, .16f, .199f},
        {U't', .16f, .169f, .18f, .199f},
    };
    int pageCount() const override { return 1; }
    folio::ui::PageSizePt pageSize(int) const override { return {600.f, 800.f}; }
    void drawPage(IPaintTarget&, int, const Rect&, float,
                  folio::ui::PageTheme) override {}
    const std::vector<folio::ui::PageGlyph>* glyphs(int) override { return &text; }
};

class CommaLineSource final : public folio::ui::IPageSource {
public:
    std::vector<folio::ui::PageGlyph> text{
        {U'A', .10f, .080f, .15f, .105f},
        {U'B', .15f, .080f, .20f, .105f},
        {U',', .20f, .108f, .208f, .118f},
        {U'C', .208f, .080f, .25f, .105f},
        {U'D', .10f, .135f, .15f, .160f},
        {U'E', .15f, .135f, .20f, .160f},
        {U',', .20f, .163f, .208f, .173f},
        {U'F', .208f, .135f, .25f, .160f},
    };
    int pageCount() const override { return 1; }
    folio::ui::PageSizePt pageSize(int) const override { return {600.f, 800.f}; }
    void drawPage(IPaintTarget&, int, const Rect&, float,
                  folio::ui::PageTheme) override {}
    const std::vector<folio::ui::PageGlyph>* glyphs(int) override { return &text; }
};

folio::ui::PointerEvent finger(folio::ui::PointerPhase phase,
                               folio::ui::PointerId id, Point p, double time) {
    folio::ui::PointerEvent e;
    e.pointerType = folio::ui::PointerType::Touch;
    e.pointerId = id;
    e.phase = phase;
    e.pos = e.windowPos = p;
    e.timestamp = time;
    return e;
}

class Spy : public IPaintTarget {
public:
    struct Image { int w, h; Rect dst; ImageFit fit; };
    struct Band { Rect rect; Color top, bottom; BlendMode mode; };
    struct Outline { Rect rect; Corners corners; Color color; float width; };
    struct Dot { Point center; float radius; Color color; };
    struct Fill { Rect rect; Color color; BlendMode mode; };
    std::vector<Image> images;
    std::vector<Band> bands;
    std::vector<Outline> outlines;
    std::vector<Dot> dots;
    std::vector<Fill> fills;
    std::vector<BlendMode> blendCalls;
    BlendMode currentBlend = BlendMode::SrcOver;
    void save() override {}
    void restore() override {}
    void translate(float, float) override {}
    void scale(float, float) override {}
    void rotateDegrees(float) override {}
    void clipRect(const Rect&, bool) override {}
    void clipRoundedRect(const Rect&, const Corners&, bool) override {}
    void saveLayerAlpha(float) override {}
    void clear(const Color&) override {}
    void fillRect(const Rect&, const Color&) override {}
    void fillRoundedRect(const Rect& r, const Corners&, const Color& c) override {
        fills.push_back({r, c, currentBlend});
    }
    void strokeRoundedRect(const Rect& r, const Corners& c, const Color& col,
                           float width) override { outlines.push_back({r, c, col, width}); }
    void drawShadow(const Rect&, const Corners&, const Shadow&) override {}
    void fillCircle(const Point& p, float radius, const Color& c) override {
        dots.push_back({p, radius, c});
    }
    void drawLine(const Point&, const Point&, const Color&, float) override {}
    void fillRectVerticalGradient(const Rect& r, const Color& top,
                                  const Color& bottom) override {
        bands.push_back({r, top, bottom, currentBlend});
    }
    void setBlendMode(BlendMode mode) override {
        currentBlend = mode;
        blendCalls.push_back(mode);
    }
    void strokePath(std::span<const PathVerb>, const Color&, float, LineCap, LineJoin) override {}
    void fillPath(std::span<const PathVerb>, const Color&) override {}
    void drawText(std::string_view, const Point&, const TextStyle&, TextBaseline) override {}
    void drawTextInBox(std::string_view, const Rect&, const TextStyle&, TextAlign) override {}
    float measureText(std::string_view, const TextStyle&) override { return 0; }
    FontMetrics fontMetrics(const TextStyle&) override { return {}; }
    bool drawCached(std::uint64_t, const Rect&, int, int,
                    const std::function<void(IPaintTarget&)>&) override { return false; }
    void drawImageBGRA(const void*, int w, int h, std::size_t, const Rect& dst,
                       std::uint64_t, ImageFit fit, ImageTint) override {
        images.push_back({w, h, dst, fit});
    }
    void invalidateRasterCache() override {}
    RasterCacheStats rasterCacheStats() const override { return {}; }
    int width() const override { return 900; }
    int height() const override { return 700; }
    std::string_view backendName() const override { return "spy"; }
};

struct TextSpy final : Spy {
    std::vector<std::string> rows;
    void drawTextInBox(std::string_view s, const Rect&, const TextStyle&,
                       TextAlign) override { rows.emplace_back(s); }
    float measureText(std::string_view s, const TextStyle&) override {
        return static_cast<float>(s.size()) * 8.f;
    }
};

bool awaitTiles(folio::engine::PdfiumPageSource& pdf, Spy& spy,
                const Rect& dest, const Rect& view, float scale, int required) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline) {
        // Mirror ReaderScreen: begin frame before PageStage ticks and paints.
        // A completion must be installed before the old wanted-set is reset.
        pdf.beginVisibleFrame();
        pdf.tick();
        spy.images.clear();
        pdf.drawVisiblePage(spy, 1, dest, view, scale, folio::ui::PageTheme::Light);
        pdf.endVisibleFrame();
        int found = 0;
        for (const auto& im : spy.images) if (im.fit == IPaintTarget::ImageFit::Final) ++found;
        if (found == required) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
}

FOLIO_TEST_MAIN("visible-only tiles and tab lifetime") {
    folio::engine::PdfiumPageSource pdf;
    Spy spy;
    check(pdf.open("folio-test.pdf"), "PDF tab opens one mapped document");
    if (!pdf.isOpen()) return;
    pdf.setFocusPage(1);
    // A path open publishes page-one geometry; nearby pages are resolved
    // asynchronously. Wait so the destination and the PDFium grid agree.
    for (int i = 0; i < 300 && pdf.pageSize(1).w != 612.f; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(pdf.pageSize(1).w == 612.f, "visible page geometry resolves lazily");
    // The page at 16x is 9792x12672 device pixels (~496 MB if rasterised
    // whole); only the four tiles intersecting this 700x700 viewport exist.
    const Rect page{0, 0, 9792, 12672};
    const Rect visible{4096, 4096, 700, 700};
    pdf.beginVisibleFrame();
    check(awaitTiles(pdf, spy, page, visible, 16.f, 4),
          "the four on-screen tiles resolve without rendering the whole page");
    for (const auto& im : spy.images)
        if (im.fit == IPaintTarget::ImageFit::Final)
            check(im.w <= 512 && im.h <= 512,
                  "main canvas stores only 512-device-pixel final tiles");
    section("grid viewport keeps all visible pages, even >2 from focus, in small rasters");
    {
        folio::engine::PdfiumPageSource gridPdf;
        Spy gridSpy;
        check(gridPdf.open("folio-grid.pdf") && gridPdf.pageCount() == 36,
              "grid fixture has enough pages to exceed the focus +/-2 window");
        gridPdf.setFocusPage(0);
        gridPdf.setVisibleGridRange(0, 17, 0, 23, 138, 176);
        // A sidebar Pages rail may show a completely different page range.
        // Changing that range must not cancel grid-page raster jobs.
        gridPdf.setVisibleThumbnailRange(29, 31);
        // Simulate a fast wheel changing the focused page *while* the whole
        // 18-cell viewport still requests its pages. Old dropQueuedOutside
        // and tick discarded pages beyond focus +/-2.
        gridPdf.setFocusPage(1);
        auto drawGrid = [&] {
            gridSpy.images.clear();
            gridPdf.beginVisibleFrame();
            gridPdf.tick();
            for (int p = 0; p < 18; ++p) {
                const auto ps = gridPdf.pageSize(p);
                const float scale = std::min(138.f / ps.w, 176.f / ps.h);
                const Rect dst{0.f, 0.f, ps.w * scale, ps.h * scale};
                gridPdf.drawPage(gridSpy, p, dst, scale,
                                 folio::ui::PageTheme::Light);
            }
            gridPdf.endVisibleFrame();
        };
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::seconds(8);
        do {
            drawGrid();
            if (gridSpy.images.size() >= 18) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < until);
        check(gridSpy.images.size() == 18,
              "every visible grid cell, not only the +/-2 pages, finishes rasterizing");
        check(std::all_of(gridSpy.images.begin(), gridSpy.images.end(),
                          [](const Spy::Image& im) { return im.w <= 138 && im.h <= 176; }),
              "grid pages use cell-sized bitmaps instead of viewport-sized tiles");
        check(gridPdf.cacheStats().bytes < 8u * 1024u * 1024u,
              "grid plus a nearby row remain under an 8 MiB image budget");
        gridPdf.setVisibleGridRange(17, 26, 11, 32, 138, 176);
        gridPdf.setFocusPage(20);
        gridPdf.setVisibleGridRange(-1, -1, -1, -1, 0, 0);
        check(gridPdf.cacheStats().bytes < 8u * 1024u * 1024u,
              "grid caches trim again after leaving the multi-page viewport");
    }
    constexpr std::size_t kPreviewBytes = 512u * 768u * 4u;
    // One preview on either side, plus one ahead of a fling. Each is bounded
    // to <=240x320 even if its provisional page geometry is wrong.
    constexpr std::size_t kAdjacentBytes = 3u * 240u * 320u * 4u;
    check(pdf.cacheStats().bytes <= 4u * 512u * 512u * 4u +
                                    kPreviewBytes + kAdjacentBytes,
          "large page uses visible tiles and at most four bounded proxies");

    // Loupe reads the resident tiles without submitting any work or
    // incrementing normal viewport hit/miss counters.
    const auto beforeLoupe = pdf.cacheStats();
    spy.images.clear();
    check(pdf.drawCachedRegion(spy, 1, page, visible, 16.f,
                              folio::ui::PageTheme::Light) && !spy.images.empty(),
          "loupe samples real resident PDF tiles");
    const auto afterLoupe = pdf.cacheStats();
    check(afterLoupe.bytes == beforeLoupe.bytes &&
              afterLoupe.hits == beforeLoupe.hits &&
              afterLoupe.misses == beforeLoupe.misses,
          "cached-only loupe neither allocates raster memory nor schedules viewport work");

    pdf.beginVisibleFrame();
    const Rect next{7168, 7168, 700, 700};
    check(awaitTiles(pdf, spy, page, next, 16.f, 4),
          "scrolling requests only the new visible tiles");
    pdf.trimIdle();
    check(pdf.cacheStats().bytes <= 4u * 512u * 512u * 4u +
                                    kPreviewBytes + kAdjacentBytes,
          "idle pass keeps at most visible tiles and four small proxies");
    // At a newly crossed zoom level the old generation must remain visible
    // until its replacement tiles resolve (no white flashing during zoom).
    pdf.beginVisibleFrame();
    spy.images.clear();
    pdf.drawVisiblePage(spy, 1, Rect{0, 0, 10404, 13464},
                        Rect{7616, 7616, 700, 700}, 17.f,
                        folio::ui::PageTheme::Light);
    bool oldProxy = false;
    for (const auto& im : spy.images)
        if (im.fit == IPaintTarget::ImageFit::Transient) oldProxy = true;
    check(oldProxy, "previous on-screen tiles bridge a continuous zoom-level change");

    // 7x7 fully visible 512px tiles exceed the desktop's 48 MiB soft source
    // budget. Evicting even one of them makes each completed frame request
    // it again, so the viewport never becomes fully painted.
    const Rect dense{4096, 4096, 3584, 3584};
    check(awaitTiles(pdf, spy, page, dense, 16.f, 49),
          "all visible tiles finish even when the viewport exceeds the cache budget");
    pdf.trimIdle();
    check(pdf.cacheStats().bytes >= 49u * 512u * 512u * 4u,
          "idle pruning retains every tile used in the last presented viewport");

    pdf.parkVisualCaches(1);
    check(pdf.isOpen() && pdf.cacheStats().bytes <= 512u * 768u * 4u &&
              pdf.textPagesResident() == 0,
          "inactive tab keeps at most one bounded page preview, not its tiles/text");
    pdf.parkVisualCaches(1, true);
    check(pdf.cacheStats().bytes <= (512u * 768u + 3u * 240u * 320u) * 4u,
          "minimise retains only one page preview and at most three tiny neighbours");
    pdf.suspendVisualCaches();
    check(pdf.isOpen() && pdf.cacheStats().bytes == 0 && pdf.textPagesResident() == 0,
          "tab suspension drops all visual and text caches, not the document");
    pdf.beginVisibleFrame();
    check(awaitTiles(pdf, spy, page, next, 16.f, 4),
          "returning to a suspended tab recreates its on-screen tiles");

    folio::engine::PdfiumPageSource cold;
    check(cold.open("folio-test.pdf"), "cold document opens for preview test");
    if (!cold.isOpen()) return;
    cold.setFocusPage(1);
    const Rect bigView{4096, 4096, 3584, 3584};
    bool previewSeen = false;
    for (int i = 0; i < 300 && !previewSeen; ++i) {
        cold.beginVisibleFrame();
        spy.images.clear();
        cold.drawVisiblePage(spy, 1, page, bigView, 16.f,
                             folio::ui::PageTheme::Light);
        cold.endVisibleFrame();
        for (const auto& im : spy.images)
            if (im.fit == IPaintTarget::ImageFit::Transient &&
                im.dst.w == page.w && im.w <= 512 && im.h <= 768)
                previewSeen = true;
        if (!previewSeen) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(previewSeen, "a bounded whole-page preview covers cold white tiles first");

    section("repeated animation frames coalesce identical tile requests");
    {
        folio::engine::PdfiumWorker worker;
        check(worker.openFile("folio-test.pdf", nullptr), "tile worker opens PDF");
        const folio::core::TileGrid grid(300.f, 400.f, 1.f, 0);
        const auto tiles = grid.tilesForViewport({0, 0, 300, 400});
        check(!tiles.empty(), "fixture has a visible tile");
        if (!tiles.empty()) {
            for (int i = 0; i < 30; ++i) {
                worker.requestTile(0, 1.f, grid.deviceWidth(), tiles[0], 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            int completed = 0;
            const auto end = std::chrono::steady_clock::now() +
                             std::chrono::seconds(4);
            while (std::chrono::steady_clock::now() < end && !worker.hasResults())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            for (auto& result : worker.drain())
                if (result.kind == folio::engine::PdfiumWorker::Kind::Tile)
                    ++completed;
            check(completed == 1,
                  "same visible tile is rendered once, not once per frame");
        }
    }

    section("focused page arrives before bounded neighbouring previews");
    folio::engine::PdfiumPageSource nearby;
    check(nearby.open("folio-test.pdf"), "adjacent-prefetch document opens");
    if (!nearby.isOpen()) return;
    nearby.setFocusPage(0);
    const Rect nearPage{0.f, 0.f, 300.f, 400.f};
    bool firstReady = false;
    bool neighbourReady = false;
    const auto nearDeadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < nearDeadline && !neighbourReady) {
        nearby.beginVisibleFrame();
        spy.images.clear();
        nearby.drawVisiblePage(spy, 0, nearPage, nearPage, 1.5f,
                               folio::ui::PageTheme::Light);
        nearby.endVisibleFrame();
        firstReady |= !spy.images.empty() || nearby.hasCachedPageProxy(0);
        neighbourReady = nearby.hasCachedPageProxy(1);
        if (!neighbourReady) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(firstReady && neighbourReady,
          "foreground page resolves before its adjacent background proxy");
    bool aheadReady = nearby.hasCachedPageProxy(2);
    const auto aheadDeadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(8);
    while (!aheadReady && std::chrono::steady_clock::now() < aheadDeadline) {
        nearby.beginVisibleFrame();
        nearby.drawVisiblePage(spy, 0, nearPage, nearPage, 1.5f,
                               folio::ui::PageTheme::Light);
        nearby.endVisibleFrame();
        aheadReady = nearby.hasCachedPageProxy(2);
        if (!aheadReady) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(aheadReady, "one extra page ahead gets only a tiny fling proxy");
    nearby.trimIdle();
    check(nearby.hasCachedPageProxy(1) && nearby.hasCachedPageProxy(2) &&
              nearby.cacheStats().bytes < 4u * 1024u * 1024u,
          "idle pruning keeps bounded nearby previews for an instant scroll");
    nearby.suspendVisualCaches();
    check(!nearby.hasCachedPageProxy(0) && !nearby.hasCachedPageProxy(1),
          "tab suspension releases foreground and adjacent visual proxies");

    section("sidebar thumbnails load even when scrolled far from the canvas page");
    folio::engine::PdfiumPageSource sidebar;
    check(sidebar.open("../../documents/lshort.pdf"), "multi-page PDF opens for thumbnail test");
    if (!sidebar.isOpen()) return;
    check(sidebar.pageCount() > 60, "test PDF has distant sidebar pages");
    sidebar.setFocusPage(0);
    sidebar.setVisibleThumbnailRange(45, 48);
    spy.images.clear();
    sidebar.drawPage(spy, 45, Rect{0, 0, 108, 154}, 0.18f,
                     folio::ui::PageTheme::Light);
    // Changing the canvas page must not cancel the visible thumbnail job.
    sidebar.setFocusPage(2);
    bool thumbArrived = false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (!thumbArrived && std::chrono::steady_clock::now() < until) {
        sidebar.tick();
        spy.images.clear();
        sidebar.drawPage(spy, 45, Rect{0, 0, 108, 154}, 0.18f,
                         folio::ui::PageTheme::Light);
        thumbArrived = !spy.images.empty();
        if (!thumbArrived) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(thumbArrived, "distant visible thumbnail completes instead of being discarded");
    sidebar.setVisibleThumbnailRange(-1, -1);
    sidebar.trimTo(2, 2);
    check(sidebar.cacheStats().bytes < 1024 * 1024,
          "closing the sidebar releases its distant thumbnail cache");

    section("text highlights track the actual raster on cropped and rotated PDFs");
    folio::engine::PdfiumPageSource textPdf;
    check(textPdf.open("text-coordinate-fixture.pdf"),
          "opens reproducible non-zero CropBox and /Rotate fixture");
    if (!textPdf.isOpen()) return;
    folio::engine::PdfiumRasterizer raster;
    check(raster.openFile("text-coordinate-fixture.pdf"),
          "independent PDFium raster opens coordinate fixture");
    for (int pageNo = 0; pageNo < 3; ++pageNo) {
        textPdf.setFocusPage(pageNo);
        const std::vector<folio::ui::PageGlyph>* gs = nullptr;
        for (int attempts = 0; attempts < 300; ++attempts) {
            textPdf.tick();
            gs = textPdf.glyphs(pageNo);
            if (gs && !gs->empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(gs && !gs->empty(), "real PDF text layer resolves on page " +
                                  std::to_string(pageNo));
        if (!gs || gs->empty()) continue;
        const auto it = std::find_if(gs->begin(), gs->end(),
                                     [](const auto& g) { return g.code == U'F'; });
        check(it != gs->end(), "visible text is extracted in reading order");
        if (it == gs->end()) continue;
        const auto& g = *it;
        check(g.l >= 0.f && g.r <= 1.f && g.t >= 0.f && g.b <= 1.f &&
                  g.r > g.l && g.b > g.t,
              "visible character geometry is finite and inside the displayed page");
        const auto hidden = std::find_if(gs->begin(), gs->end(),
                              [](const auto& c) { return c.code == U'H'; });
        check(hidden != gs->end() && hidden->l == 0.f && hidden->r == 0.f,
              "searchable glyphs wholly outside CropBox cannot paint overlays");
        const auto sz = raster.pageInfo(static_cast<folio::core::PageId>(pageNo));
        const auto tile = raster.renderFullPage(
            static_cast<folio::core::PageId>(pageNo), 2.f, 0,
            folio::core::ColorScheme::Light);
        check(tile && sz.widthPt > 0 && sz.heightPt > 0,
              "cropped and rotated PDF text rasters at display size");
        if (!tile) continue;
        // A highlighted glyph must actually cover dark raster text on every
        // intrinsic rotation, not just be within the displayed page bounds.
        const int x0 = std::max(0, static_cast<int>(g.l * static_cast<float>(tile->width)) - 2);
        const int y0 = std::max(0, static_cast<int>(g.t * static_cast<float>(tile->height)) - 2);
        const int x1 = std::min(tile->width, static_cast<int>(g.r * static_cast<float>(tile->width)) + 3);
        const int y1 = std::min(tile->height, static_cast<int>(g.b * static_cast<float>(tile->height)) + 3);
        int ink = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const auto pos = static_cast<std::size_t>(y) * tile->stride +
                                 static_cast<std::size_t>(x) * 4;
                if (std::to_integer<int>(tile->pixels[pos]) < 160) ++ink;
            }
        check(ink > 3, "selection covers PDF raster glyph at page " +
                           std::to_string(pageNo));
    }

    section("reference-style amber selection and touch-adjustable end dots");
    folio::ui::PageStage selectionStage;
    selectionStage.setFrame(0, 0, 390, 640);
    selectionStage.setSource(std::make_shared<SelectionSource>(), 0);
    selectionStage.setZoom(.5f, 0);
    selectionStage.snapZoomToTarget(1);
    selectionStage.selectAll();
    int clearedPopup = 0, committed = 0;
    selectionStage.onSelectionCleared = [&] { ++clearedPopup; };
    selectionStage.onSelectionCommitted = [&](Point) { ++committed; };
    selectionStage.setPageTheme(folio::ui::PageTheme::Sepia);
    spy.bands.clear(); spy.outlines.clear(); spy.dots.clear();
    selectionStage.onPaint(spy, {folio::ui::tokens::kLight, 2., 1.f});
    const auto amber = std::count_if(spy.bands.begin(), spy.bands.end(),
                           [](const Spy::Band& b) {
                               return b.top.a == 48 && b.bottom.a == 69;
                           });
    const auto fineEdges = std::count_if(spy.outlines.begin(), spy.outlines.end(),
                           [](const Spy::Outline& o) {
                               return o.color.a == 137 && o.corners.tl <= 2.5f &&
                                      o.corners.tl > 0.f && o.width < 1.f;
                           });
    check(amber == 2 && fineEdges == 0,
          "sepia keeps clipped amber bands without per-line outline glitches");
    check(std::all_of(spy.bands.begin(), spy.bands.end(),
              [](const Spy::Band& b) {
                  return b.top.a != 48 || b.mode == BlendMode::Multiply;
              }) && spy.currentBlend == BlendMode::SrcOver,
          "sepia selection multiplies into paper without leaving blend state on text");
    const auto firstWash = std::find_if(spy.bands.begin(), spy.bands.end(),
                           [](const Spy::Band& b) { return b.top.a == 48; });
    check(firstWash != spy.bands.end() && firstWash->rect.h > 10.f,
          "selection band has breathing room around tight PDFium glyph boxes");

    section("mobile selection loupe, outward grips, release and toggle");
    auto cached = std::make_shared<CachedSelectionSource>();
    folio::ui::PageStage loupeStage;
    loupeStage.setFrame(0, 0, 390, 640);
    loupeStage.setSource(cached, 0);
    loupeStage.setZoom(.5f, 0);
    loupeStage.snapZoomToTarget(1);
    loupeStage.selectAll();
    Spy loupeSpy;
    loupeStage.onPaint(loupeSpy, {folio::ui::tokens::kLight, 2., 1.f});
    const auto& dots = loupeSpy.dots;
    const auto& bands = loupeSpy.bands;
    const auto selectedBand = std::find_if(bands.begin(), bands.end(),
        [](const Spy::Band& b) { return b.top.a == 51; });
    if (dots.size() >= 4 && selectedBand != bands.end()) {
        const Point start = dots[1].center, end = dots[3].center;
        check(start.x < selectedBand->rect.x && start.y < selectedBand->rect.y &&
                  end.x > selectedBand->rect.right() &&
                  end.y > selectedBand->rect.bottom(),
              "start/end dots sit outwards at upper-left/lower-right glyph corners");
    } else {
        check(false, "outward grips and selection band paint");
    }
    loupeStage.setSelectionMagnifierEnabled(true);
    loupeStage.clearSelection();
    const Point magnifiedWord = selectedBand == bands.end() ? Point{195.f, 180.f}
        : Point{selectedBand->rect.x + 5.f,
                selectedBand->rect.y + selectedBand->rect.h * .5f};
    loupeSpy.bands.clear();
    loupeStage.onPointer(finger(folio::ui::PointerPhase::Down, 121, magnifiedWord, 3.));
    loupeStage.onPaint(loupeSpy, {folio::ui::tokens::kLight, 3.7, 1.f});
    const auto liveBands = std::count_if(loupeSpy.bands.begin(), loupeSpy.bands.end(),
        [](const Spy::Band& band) {
            return band.top.a == 51 && band.mode == BlendMode::Multiply;
        });
    check(liveBands == 2 && loupeSpy.currentBlend == BlendMode::SrcOver,
          "magnifier shows the SAME highlighted word as the page, with sharp-text blend");
    check(cached->cachedBlits == 1 && !loupeStage.selectedText().empty(),
          "long-press shows a cached-pixel loupe while keeping the word selection");
    loupeStage.onPointer(finger(folio::ui::PointerPhase::Move, 121,
                                {magnifiedWord.x + 12.f, magnifiedWord.y}, 3.8));
    loupeStage.onPaint(loupeSpy, {folio::ui::tokens::kLight, 3.8, 1.f});
    check(cached->cachedBlits == 2, "touch drag moves the loupe without a new PDF render");
    loupeStage.onPointer(finger(folio::ui::PointerPhase::Up, 121, magnifiedWord, 3.9));
    loupeStage.onPaint(loupeSpy, {folio::ui::tokens::kLight, 4., 1.f});
    check(cached->cachedBlits == 2, "release hides the loupe immediately");
    loupeSpy.dots.clear();
    loupeStage.onPaint(loupeSpy, {folio::ui::tokens::kLight, 4.1, 1.f});
    if (loupeSpy.dots.size() >= 4) {
        const Point grip = loupeSpy.dots[3].center;
        const int beforeGrip = cached->cachedBlits;
        const auto original = loupeStage.selectedText();
        loupeStage.onPointer(finger(folio::ui::PointerPhase::Down, 123, grip, 5.));
        loupeStage.onPaint(loupeSpy, {folio::ui::tokens::kLight, 5.1, 1.f});
        check(cached->cachedBlits == beforeGrip + 1,
              "grabbing an outward end handle shows loupe without beginning a pan");
        loupeStage.onPointer(finger(folio::ui::PointerPhase::Down, 124,
                                    {grip.x + 60.f, grip.y + 60.f}, 5.2));
        loupeStage.onPaint(loupeSpy, {folio::ui::tokens::kLight, 5.3, 1.f});
        check(cached->cachedBlits == beforeGrip + 1 &&
                  loupeStage.selectedText() == original,
              "second finger cancels handle loupe and preserves selection for pinch");
        loupeStage.onPointer(finger(folio::ui::PointerPhase::Up, 124,
                                    {grip.x + 60.f, grip.y + 60.f}, 5.4));
        loupeStage.onPointer(finger(folio::ui::PointerPhase::Up, 123, grip, 5.5));
    } else {
        check(false, "selection handle is painted for touch adjustment");
    }
    const int beforeDisable = cached->cachedBlits;
    loupeStage.setSelectionMagnifierEnabled(false);
    loupeStage.onPointer(finger(folio::ui::PointerPhase::Down, 122, magnifiedWord, 6.));
    loupeStage.onPaint(loupeSpy, {folio::ui::tokens::kLight, 6.7, 1.f});
    check(cached->cachedBlits == beforeDisable && !loupeStage.selectedText().empty(),
          "disabled setting leaves mobile selection intact without a loupe");
    loupeStage.onPointer(finger(folio::ui::PointerPhase::Up, 122, magnifiedWord, 6.8));

    section("opt-in desktop mouse selection magnifies resident PDF pixels");
    auto mousePixels = std::make_shared<CachedSelectionSource>();
    folio::ui::PageStage mouseStage;
    mouseStage.setFrame(0, 0, 390, 640);
    mouseStage.setSource(mousePixels, 0);
    mouseStage.setZoom(.5f, 0);
    mouseStage.snapZoomToTarget(1);
    Spy mouseSpy;
    mouseStage.onPaint(mouseSpy, {folio::ui::tokens::kLight, 7., 1.f});
    auto mouse = [&](folio::ui::PointerPhase phase, Point point, double seconds) {
        folio::ui::PointerEvent e;
        e.pointerType = folio::ui::PointerType::Mouse;
        e.pointerId = 0;
        e.button = folio::ui::MouseButton::Left;
        e.phase = phase;
        e.pos = e.windowPos = point;
        e.timestamp = seconds;
        return e;
    };
    int mousePage = -1;
    Point mouseNorm{};
    check(mouseStage.hitPage(magnifiedWord, &mousePage, &mouseNorm),
          "mouse probe lies within desktop page");
    mouseStage.pressed = true; // EventRouter owns this state in production
    mouseStage.onPointer(mouse(folio::ui::PointerPhase::Down, magnifiedWord, 7.1));
    mouseStage.onPaint(mouseSpy, {folio::ui::tokens::kLight, 7.2, 1.f});
    check(mousePixels->cachedBlits == 0,
          "desktop magnifier stays off by default while text selection works");
    mouseStage.onPointer(mouse(folio::ui::PointerPhase::Up, magnifiedWord, 7.3));
    mouseStage.clearSelection();
    mouseStage.setSelectionMagnifierEnabled(true);
    mouseStage.onPointer(mouse(folio::ui::PointerPhase::Down, magnifiedWord, 8.));
    mouseStage.onPointer(mouse(folio::ui::PointerPhase::Move,
                               {magnifiedWord.x + 14.f, magnifiedWord.y}, 8.03));
    mouseStage.onPaint(mouseSpy, {folio::ui::tokens::kLight, 8.1, 1.f});
    const int mouseStartBlits = mousePixels->cachedBlits;
    check(mouseStartBlits == 1 && !mouseStage.selectedText().empty(),
          "enabled desktop mouse drag selects text and shows cached loupe");
    mouseStage.onPointer(mouse(folio::ui::PointerPhase::Move,
                               {magnifiedWord.x + 17.f, magnifiedWord.y}, 8.2));
    mouseStage.onPaint(mouseSpy, {folio::ui::tokens::kLight, 8.2, 1.f});
    check(mousePixels->cachedBlits == mouseStartBlits + 1,
          "moving mouse selection follows cursor without requesting new raster");
    mouseStage.onPointer(mouse(folio::ui::PointerPhase::Up,
                               {magnifiedWord.x + 14.f, magnifiedWord.y}, 8.3));
    mouseStage.pressed = false;
    mouseStage.onPaint(mouseSpy, {folio::ui::tokens::kLight, 8.4, 1.f});
    check(mousePixels->cachedBlits == mouseStartBlits + 1,
          "mouse release hides magnifier and keeps committed text selection");

    section("annotation highlighter previews before stroke and tracks its cursor");
    auto markerPixels = std::make_shared<CachedSelectionSource>();
    folio::ui::PageStage markerStage;
    markerStage.setFrame(0, 0, 390, 640);
    markerStage.setSource(markerPixels, 0);
    markerStage.setZoom(.5f, 0);
    markerStage.snapZoomToTarget(1);
    folio::ui::AnnotationStore markerStore;
    markerStage.setStore(&markerStore);
    markerStage.setEditTool(folio::ui::PageStage::EditTool::Highlight);
    markerStage.setSelectionMagnifierEnabled(true);
    const Color markerColor{238, 188, 72, 255};
    markerStage.setAnnotStyle(markerColor, 4.f, 1.f);
    Spy markerSpy;
    markerStage.onPaint(markerSpy, {folio::ui::tokens::kLight, 2., 1.f});
    const Point down{195.f, 200.f};
    int markerPage = -1;
    Point markerNorm{};
    check(markerStage.hitPage(down, &markerPage, &markerNorm),
          "highlighter starts on actual page coordinates");
    const int firstBlits = markerPixels->cachedBlits;
    markerSpy.fills.clear();
    markerStage.onPointer(finger(folio::ui::PointerPhase::Down, 201, down, 3.));
    const auto* pendingMarker = markerStore.draft();
    check(pendingMarker && pendingMarker->kind == folio::ui::AnnotKind::Highlight &&
              pendingMarker->box.w == 0.f && markerStore.countOn(markerPage) == 0,
          "press creates only an empty draft; no document highlight before drag");
    markerStage.onPaint(markerSpy, {folio::ui::tokens::kLight, 3.1, 1.f});
    const auto markerFill = [&](const Spy::Fill& f) {
        return f.color.r == markerColor.r &&
               f.color.g == markerColor.g &&
               f.color.b == markerColor.b && f.color.a == 97;
    };
    const auto preview = std::find_if(markerSpy.fills.begin(), markerSpy.fills.end(),
                                      markerFill);
    const float previewH = preview == markerSpy.fills.end() ? 0.f : preview->rect.h;
    check(markerPixels->cachedBlits == firstBlits + 1 &&
              preview != markerSpy.fills.end() &&
              preview->mode == BlendMode::Multiply && previewH > 0.f,
          "touch-down immediately magnifies the exact-color, actual-width stroke preview");
    check(!markerPixels->cachedSamples.empty(), "preview reads cached PDF only");
    const Rect before = markerPixels->cachedSamples.empty() ? Rect{} :
                         markerPixels->cachedSamples.back();
    // Until there is deliberate horizontal travel, the marker is just a
    // movable cursor. Vertical repositioning updates the loupe but creates
    // neither document pixels nor a saved annotation.
    markerStage.onPointer(finger(folio::ui::PointerPhase::Move, 201,
                                  {down.x + 3.f, down.y + 50.f}, 3.2));
    const auto* preparing = markerStore.draft();
    check(preparing && preparing->empty() &&
              preparing->box.y > markerNorm.y + .05f &&
              markerStore.countOn(markerPage) == 0,
          "vertical reposition moves empty draft without drawing or committing");
    markerSpy.fills.clear();
    markerStage.onPaint(markerSpy, {folio::ui::tokens::kLight, 3.21, 1.f});
    const Rect positioned = markerPixels->cachedSamples.back();
    const auto movedPreview = std::find_if(markerSpy.fills.begin(),
                                           markerSpy.fills.end(), markerFill);
    check(markerPixels->cachedBlits == firstBlits + 2 &&
              positioned.y > before.y + 35.f &&
              movedPreview != markerSpy.fills.end() &&
              std::fabs(movedPreview->rect.h - previewH) < .01f,
          "magnifier follows vertical reposition with unchanged highlight thickness");
    markerStage.onPointer(finger(folio::ui::PointerPhase::Move, 201,
                                  {down.x + 7.f, down.y + 50.f}, 3.25));
    check(markerStore.draft() && markerStore.draft()->empty(),
          "small sideways finger jitter stays in positioning mode");
    markerStage.onPointer(finger(folio::ui::PointerPhase::Move, 201,
                                  {down.x + 60.f, down.y + 75.f}, 3.3));
    const auto* growing = markerStore.draft();
    const float lockedLine = growing ? growing->box.y : -1.f;
    check(growing && growing->box.w > .02f &&
              lockedLine > markerNorm.y + .05f,
          "diagonal horizontal start locks the previously previewed row");
    markerSpy.fills.clear();
    markerStage.onPaint(markerSpy, {folio::ui::tokens::kLight, 3.35, 1.f});
    const Rect after = markerPixels->cachedSamples.back();
    check(markerPixels->cachedBlits == firstBlits + 3 &&
              after.x > positioned.x + 30.f &&
              std::fabs(after.y - positioned.y) < 1.f,
          "after horizontal activation loupe tracks stroke endpoint at locked row");
    const auto tip = std::find_if(markerSpy.fills.rbegin(), markerSpy.fills.rend(),
                                  markerFill);
    check(tip != markerSpy.fills.rend() &&
              std::fabs(tip->rect.h - previewH) < .01f &&
              tip->mode == BlendMode::Multiply &&
              markerSpy.currentBlend == BlendMode::SrcOver,
          "moving loupe draws actual draft plus same-thickness prospective tip");
    markerStage.onPointer(finger(folio::ui::PointerPhase::Move, 201,
                                  {down.x + 82.f, down.y + 110.f}, 3.5));
    markerStage.onPaint(markerSpy, {folio::ui::tokens::kLight, 3.55, 1.f});
    const Rect drift = markerPixels->cachedSamples.back();
    check(markerStore.draft() && markerStore.draft()->box.w > .02f &&
              std::fabs(markerStore.draft()->box.y - lockedLine) < .001f &&
              drift.x > after.x + 15.f && std::fabs(drift.y - after.y) < 1.f,
          "later vertical finger drift moves neither stroke baseline nor loupe sample Y");
    markerStage.onPointer(finger(folio::ui::PointerPhase::Up, 201,
                                  {down.x + 82.f, down.y + 110.f}, 3.6));
    markerStage.onPaint(markerSpy, {folio::ui::tokens::kLight, 3.7, 1.f});
    check(markerPixels->cachedBlits == firstBlits + 4 &&
              markerStore.countOn(markerPage) == 1 && !markerStore.draft(),
          "release hides marker loupe and commits only the dragged highlight");
    markerStage.setPageTheme(folio::ui::PageTheme::Dark);
    markerStage.onPointer(finger(folio::ui::PointerPhase::Down, 202, down, 4.));
    markerSpy.fills.clear();
    markerStage.onPaint(markerSpy, {folio::ui::tokens::kDark, 4.1, 1.f});
    check(std::any_of(markerSpy.fills.begin(), markerSpy.fills.end(),
              [&](const Spy::Fill& f) {
                  return markerFill(f) && f.mode == BlendMode::Screen;
              }), "dark-page highlighter preview uses same screen blend as committed mark");
    const int beforePinch = markerPixels->cachedBlits;
    markerStage.onPointer(finger(folio::ui::PointerPhase::Down, 203,
                                  {down.x - 75.f, down.y + 25.f}, 4.2));
    markerStage.onPaint(markerSpy, {folio::ui::tokens::kDark, 4.3, 1.f});
    check(markerPixels->cachedBlits == beforePinch && !markerStore.draft(),
          "second finger cancels uncommitted marker and its magnifier for pinch");
    markerStage.onPointer(finger(folio::ui::PointerPhase::Up, 203,
                                  {down.x - 75.f, down.y + 25.f}, 4.4));
    markerStage.onPointer(finger(folio::ui::PointerPhase::Up, 202, down, 4.5));
    markerStage.setSelectionMagnifierEnabled(false);
    const int beforeOff = markerPixels->cachedBlits;
    markerStage.onPointer(finger(folio::ui::PointerPhase::Down, 204, down, 5.));
    markerStage.onPaint(markerSpy, {folio::ui::tokens::kDark, 5.1, 1.f});
    check(markerPixels->cachedBlits == beforeOff && markerStore.draft(),
          "saved shared magnifier toggle hides marker preview without disabling drawing");
    markerStage.onPointer(finger(folio::ui::PointerPhase::Move, 204,
                                  {down.x, down.y + 40.f}, 5.2));
    markerStage.onPaint(markerSpy, {folio::ui::tokens::kDark, 5.25, 1.f});
    check(markerPixels->cachedBlits == beforeOff &&
              markerStore.draft() && markerStore.draft()->empty() &&
              markerStore.draft()->box.y > markerNorm.y,
          "vertical reposition also works with magnifier switched off");
    markerStage.onPointer(finger(folio::ui::PointerPhase::Up, 204,
                                  {down.x, down.y + 40.f}, 5.3));
    check(markerStore.countOn(markerPage) == 1 && !markerStore.draft(),
          "reposition and release without horizontal stroke saves no mark");
    markerStage.setSelectionMagnifierEnabled(true);
    // Near the top there is no space above the cursor; still show a small
    // side-docked preview rather than making the first text row untargetable.
    const Point topCursor{195.f, 40.f};
    int topPage = -1;
    check(markerStage.hitPage(topCursor, &topPage, nullptr),
          "first-row marker cursor lies on the actual page");
    const int beforeTop = markerPixels->cachedBlits;
    markerStage.onPointer(finger(folio::ui::PointerPhase::Down, 205, topCursor, 6.));
    markerStage.onPaint(markerSpy, {folio::ui::tokens::kDark, 6.1, 1.f});
    check(markerPixels->cachedBlits == beforeTop + 1,
          "top-of-page highlighter still previews on touch-down beside the cursor");
    markerStage.onPointer(finger(folio::ui::PointerPhase::Cancel, 205, topCursor, 6.2));
    auto mouseMarker = [&](folio::ui::PointerPhase phase, Point pos, double time) {
        auto e = finger(phase, 206, pos, time);
        e.pointerType = folio::ui::PointerType::Mouse;
        return e;
    };
    markerStage.onPointer(mouseMarker(folio::ui::PointerPhase::Down, down, 7.));
    markerStage.onPointer(mouseMarker(folio::ui::PointerPhase::Move,
                                      {down.x + 5.f, down.y + 45.f}, 7.1));
    const auto* mouseDraft = markerStore.draft();
    check(mouseDraft && mouseDraft->box.w > .005f &&
              std::fabs(mouseDraft->box.y + mouseDraft->box.h * .5f -
                        markerNorm.y) < .001f,
          "mouse highlighting retains immediate stroke and original row");
    markerStage.onPointer(mouseMarker(folio::ui::PointerPhase::Cancel, down, 7.2));

    section("opt-in desktop mouse highlighter magnifies its live draft");
    auto mouseMarkerPixels = std::make_shared<CachedSelectionSource>();
    folio::ui::PageStage desktopMarkerStage;
    desktopMarkerStage.setFrame(0, 0, 390, 640);
    desktopMarkerStage.setSource(mouseMarkerPixels, 0);
    desktopMarkerStage.setZoom(.5f, 0);
    desktopMarkerStage.snapZoomToTarget(1);
    folio::ui::AnnotationStore mouseMarks;
    desktopMarkerStage.setStore(&mouseMarks);
    desktopMarkerStage.setEditTool(folio::ui::PageStage::EditTool::Highlight);
    desktopMarkerStage.setSelectionMagnifierEnabled(true);
    desktopMarkerStage.onPaint(mouseSpy, {folio::ui::tokens::kLight, 9., 1.f});
    desktopMarkerStage.pressed = true;
    desktopMarkerStage.onPointer(mouse(folio::ui::PointerPhase::Down, down, 9.1));
    desktopMarkerStage.onPaint(mouseSpy, {folio::ui::tokens::kLight, 9.2, 1.f});
    check(mouseMarkerPixels->cachedBlits == 1 && mouseMarks.draft() &&
              mouseMarks.draft()->kind == folio::ui::AnnotKind::Highlight,
          "mouse highlight starts a live magnified preview when opted in");
    desktopMarkerStage.onPointer(mouse(folio::ui::PointerPhase::Move,
                                {down.x + 50.f, down.y}, 9.3));
    desktopMarkerStage.onPaint(mouseSpy, {folio::ui::tokens::kLight, 9.4, 1.f});
    check(mouseMarkerPixels->cachedBlits == 2,
          "mouse highlighter loupe follows its horizontal stroke");
    desktopMarkerStage.onPointer(mouse(folio::ui::PointerPhase::Up,
                                {down.x + 50.f, down.y}, 9.5));
    desktopMarkerStage.pressed = false;
    desktopMarkerStage.onPaint(mouseSpy, {folio::ui::tokens::kLight, 9.6, 1.f});
    check(mouseMarkerPixels->cachedBlits == 2 && mouseMarks.countOn(0) == 1,
          "releasing mouse hides loupe and commits highlighter annotation");

    section("night selection and saved text markup preserve ink contrast");
    folio::ui::PageStage nightStage;
    nightStage.setFrame(0, 0, 390, 640);
    nightStage.setSource(std::make_shared<SelectionSource>(), 0);
    nightStage.setPageTheme(folio::ui::PageTheme::Dark);
    nightStage.selectAll();
    Spy nightSpy;
    nightStage.onPaint(nightSpy, {folio::ui::tokens::kDark, 2., 1.f});
    check(std::any_of(nightSpy.bands.begin(), nightSpy.bands.end(),
              [](const Spy::Band& b) { return b.mode == BlendMode::Screen; }) &&
              nightSpy.currentBlend == BlendMode::SrcOver,
          "night selection screens into dark paper and leaves subsequent drawing normal");
    folio::ui::Annotation mark;
    mark.kind = folio::ui::AnnotKind::Highlight;
    mark.box = {.1f, .1f, .2f, .04f};
    mark.color = {255, 210, 50, 255};
    Spy annotationSpy;
    folio::ui::drawAnnotation(annotationSpy, mark, {0.f, 0.f, 200.f, 300.f}, 1.f);
    check(annotationSpy.blendCalls == std::vector<BlendMode>{BlendMode::Multiply,
                                                              BlendMode::SrcOver},
          "committed text highlighter multiplies on light/sepia without tinting ink");
    annotationSpy.blendCalls.clear();
    folio::ui::drawAnnotation(annotationSpy, mark, {0.f, 0.f, 200.f, 300.f},
                              1.f, true);
    check(annotationSpy.blendCalls == std::vector<BlendMode>{BlendMode::Screen,
                                                              BlendMode::SrcOver},
          "committed night highlight screens over dark paper without dimming white ink");

    section("comma belongs to its line and amber bands scale with zoom");
    folio::ui::PageStage commaStage;
    commaStage.setFrame(0, 0, 390, 640);
    commaStage.setSource(std::make_shared<CommaLineSource>(), 0);
    commaStage.selectAll();
    std::vector<std::pair<int, Rect>> commaQuads;
    commaStage.selectionQuads(commaQuads);
    check(commaQuads.size() == 2 && commaQuads[0].second.right() >= .25f &&
              commaQuads[1].second.right() >= .25f &&
              commaQuads[0].second.h < .041f && commaQuads[1].second.h < .041f,
          "lower comma is covered inside each line without swallowing another line");
    Spy commaSpy;
    commaStage.setZoom(1.f, 1.0);
    commaStage.snapZoomToTarget(1.0);
    commaStage.onPaint(commaSpy, {folio::ui::tokens::kLight, 1.2, 1.f});
    const auto selectedBands = [](const Spy& s) {
        std::vector<Spy::Band> result;
        for (const auto& band : s.bands)
            if (band.top.a == 51) result.push_back(band);
        return result;
    };
    const auto high = selectedBands(commaSpy);
    commaStage.setZoom(.25f, 2.0);
    commaStage.snapZoomToTarget(2.0);
    commaSpy.bands.clear();
    commaStage.onPaint(commaSpy, {folio::ui::tokens::kLight, 2.2, 1.f});
    const auto low = selectedBands(commaSpy);
    check(high.size() == 2 && low.size() == 2 &&
              high[0].rect.bottom() < high[1].rect.y &&
              low[0].rect.bottom() < low[1].rect.y,
          "selection has exactly two separated lines even when zoomed out");
    if (high.size() == 2 && low.size() == 2) {
        check(std::fabs(high[0].rect.h / low[0].rect.h - 4.f) < .25f &&
                  std::fabs((high[1].rect.y - high[0].rect.y) /
                            (low[1].rect.y - low[0].rect.y) - 4.f) < .12f,
              "wash and line spacing shrink in lockstep with 4x page zoom");
    }
    commaQuads.clear();
    commaStage.selectionQuads(commaQuads);
    check(commaQuads.size() == 2 && commaQuads[0].second.right() >= .25f,
          "normalised highlight/underline quads still include commas after zoom");
    if (low.size() == 2) {
        commaStage.clearSelection();
        const Point commaTap{low[0].rect.x + .104f * 150.f,
                             low[0].rect.y + 7.f};
        commaStage.onPointer(finger(folio::ui::PointerPhase::Down, 55,
                                    commaTap, 4.0));
        commaStage.onPaint(commaSpy, {folio::ui::tokens::kLight, 4.7, 1.f});
        check(commaStage.selectedText() == "AB,",
              "long-press on a comma selects its adjacent word with punctuation");
        commaStage.onPointer(finger(folio::ui::PointerPhase::Up, 55,
                                    commaTap, 4.8));
    }

    folio::ui::PageStage mixedStage;
    mixedStage.setFrame(0, 0, 390, 640);
    mixedStage.setSource(std::make_shared<MixedFontWordSource>(), 1);
    mixedStage.selectAll();
    std::vector<std::pair<int, Rect>> mixedQuads;
    mixedStage.selectionQuads(mixedQuads);
    check(mixedQuads.size() == 2 &&
              mixedQuads[0].second.x <= .101f &&
              mixedQuads[0].second.right() >= .332f &&
              mixedQuads[0].second.h < .05f,
          "font shifts and generous spaces form one continuous band per row");

    folio::ui::PageStage denseStage;
    denseStage.setFrame(0, 0, 390, 640);
    denseStage.setSource(std::make_shared<TightParagraphSource>(), 1);
    denseStage.selectAll();
    std::vector<std::pair<int, Rect>> denseQuads;
    denseStage.selectionQuads(denseQuads);
    check(denseQuads.size() == 10 &&
              std::all_of(denseQuads.begin(), denseQuads.end(),
                          [](const auto& q) { return q.second.h < .041f; }),
          "ten tightly led text rows stay ten bounded markup rectangles");
    bool quadsSeparated = denseQuads.size() == 10;
    for (std::size_t k = 1; k < denseQuads.size(); ++k)
        quadsSeparated = quadsSeparated &&
                         denseQuads[k - 1].second.bottom() <=
                             denseQuads[k].second.y + .0001f;
    check(quadsSeparated, "highlight and underline quads do not join adjacent rows");
    denseStage.setZoom(.25f, 2.);
    denseStage.snapZoomToTarget(2.);
    Spy denseSpy;
    denseStage.onPaint(denseSpy, {folio::ui::tokens::kLight, 2.2, 1.f});
    const auto denseBands = selectedBands(denseSpy);
    bool separated = denseBands.size() == 10;
    for (std::size_t i = 1; i < denseBands.size(); ++i)
        separated = separated &&
                    denseBands[i - 1].rect.bottom() <= denseBands[i].rect.y + .01f;
    check(separated, "zoomed-out selection never fuses densely spaced lines");
    if (!denseBands.empty()) {
        folio::ui::PageStage wordStage;
        wordStage.setFrame(0, 0, 390, 640);
        wordStage.setSource(std::make_shared<TightParagraphSource>(true), 1);
        wordStage.setZoom(.25f, 2.);
        wordStage.snapZoomToTarget(2.);
        wordStage.onPaint(denseSpy, {folio::ui::tokens::kLight, 2.2, 1.f});
        const Point word{denseBands[0].rect.x + 2.f,
                         denseBands[0].rect.y + denseBands[0].rect.h * .5f};
        wordStage.onPointer(finger(folio::ui::PointerPhase::Down, 61, word, 3.0));
        wordStage.onPaint(denseSpy, {folio::ui::tokens::kLight, 3.7, 1.f});
        check(wordStage.selectedText() == "AAAAAAAA",
              "long-press selects one word, not adjacent lines or the whole paragraph");
        std::vector<std::pair<int, Rect>> oneWord;
        wordStage.selectionQuads(oneWord);
        check(oneWord.size() == 1 && oneWord[0].second.h < .034f,
              "single-word selection yields one bounded markup quad");
        wordStage.onPointer(finger(folio::ui::PointerPhase::Up, 61, word, 3.8));
    }

    section("anchored markup does not move; free-form handles resize and delete");
    folio::ui::PageStage editing;
    editing.setFrame(0, 0, 390, 640);
    editing.setSource(std::make_shared<SelectionSource>(), 0);
    editing.setZoom(.45f, 0.);
    editing.snapZoomToTarget(0.);
    folio::ui::AnnotationStore marks;
    editing.setStore(&marks);
    Spy editSpy;
    editing.onPaint(editSpy, {folio::ui::tokens::kLight, 1., 1.f});
    int pageNo = -1;
    Point n0{}, n1{};
    const bool measured = editing.hitPage({195.f, 300.f}, &pageNo, &n0) &&
                          editing.hitPage({205.f, 310.f}, &pageNo, &n1) &&
                          n1.x > n0.x && n1.y > n0.y;
    check(measured && pageNo == 0, "page-to-screen mapping measured for touch gestures");
    if (measured) {
        const float pw = 10.f / (n1.x - n0.x), ph = 10.f / (n1.y - n0.y);
        const auto screen = [=](float x, float y) {
            return Point{195.f + (x - n0.x) * pw,
                         300.f + (y - n0.y) * ph};
        };
        int changes = 0, deleted = 0;
        editing.onAnnotationMoved = [&](const auto&, const auto&) { ++changes; };
        folio::ui::UndoStack undo;
        editing.onAnnotationGroupDeleted = [&](std::vector<folio::ui::Annotation> copies) {
            deleted += static_cast<int>(copies.size());
            undo.push(std::make_unique<folio::ui::RemoveManyCommand>(marks,
                                                                     std::move(copies)));
        };
        folio::ui::Annotation anchor;
        anchor.kind = folio::ui::AnnotKind::Highlight;
        anchor.textLinked = true;
        anchor.box = {.20f, .40f, .20f, .035f};
        const auto aId = marks.add(anchor);
        auto second = anchor;
        second.box.y = .46f;
        second.group = aId;
        const auto bId = marks.add(second);
        auto first = *marks.get(aId);
        first.group = aId;
        marks.replace(aId, first);
        editing.selectAnnot(aId);
        const Point onText = screen(.30f, .417f);
        editing.onPointer(finger(folio::ui::PointerPhase::Down, 71, onText, 2.));
        editing.onPointer(finger(folio::ui::PointerPhase::Move, 71,
                                 screen(.38f, .46f), 2.1));
        editing.onPointer(finger(folio::ui::PointerPhase::Up, 71,
                                 screen(.38f, .46f), 2.2));
        check(marks.get(aId) && std::fabs(marks.get(aId)->box.x - .20f) < 1e-5f &&
                  changes == 0, "text-linked highlights ignore drag and create no move undo");
        editing.selectAnnot(aId);
        check(editing.deleteSelectedAnnot() && !marks.get(aId) &&
                  !marks.get(bId) && deleted == 2,
              "Delete removes every quad of a grouped text markup");
        check(undo.undo() && marks.get(aId) && marks.get(bId) &&
                  undo.redo() && !marks.get(aId) && !marks.get(bId),
              "one Undo restores a whole grouped markup; Redo deletes it again");
        folio::ui::Annotation ink;
        ink.kind = folio::ui::AnnotKind::Ink;
        ink.points = {{.2f, .2f}, {.3f, .25f}, {.4f, .3f}};
        const auto inkId = marks.add(ink);
        editing.selectAnnot(inkId);
        const Point corner = screen(.4f, .3f);
        editing.onPointer(finger(folio::ui::PointerPhase::Down, 72, corner, 3.));
        editing.onPointer(finger(folio::ui::PointerPhase::Move, 72,
                                 screen(.48f, .35f), 3.1));
        editing.onPointer(finger(folio::ui::PointerPhase::Up, 72,
                                 screen(.48f, .35f), 3.2));
        check(marks.get(inkId) && marks.get(inkId)->points.back().x > .47f &&
                  marks.get(inkId)->points.back().y > .34f && changes == 1,
              "freehand corner grip rescales all points and records one undo step");
    }

    section("hollow shape grips, outside tap vs pan, and private comment recovery");
    folio::ui::PageStage hollow;
    hollow.setFrame(0, 0, 390, 640);
    hollow.setSource(std::make_shared<SelectionSource>(), 0);
    hollow.setZoom(.45f, 0.);
    hollow.snapZoomToTarget(0.);
    folio::ui::AnnotationStore shapeStore;
    hollow.setStore(&shapeStore);
    Spy hollowSpy;
    hollow.onPaint(hollowSpy, {folio::ui::tokens::kLight, 1., 1.f});
    Point pn0{}, pn1{};
    if (hollow.hitPage({195.f, 300.f}, nullptr, &pn0) &&
        hollow.hitPage({205.f, 310.f}, nullptr, &pn1)) {
        const float pw = 10.f / (pn1.x - pn0.x), ph = 10.f / (pn1.y - pn0.y);
        const auto screen = [=](float x, float y) {
            return Point{195.f + (x - pn0.x) * pw,
                         300.f + (y - pn0.y) * ph};
        };
        folio::ui::Annotation circle;
        circle.kind = folio::ui::AnnotKind::Ellipse;
        circle.box = {.18f, .25f, .3f, .22f};
        const auto circleId = shapeStore.add(circle);
        hollow.selectAnnot(circleId);
        const Point blank = screen(.78f, .73f);
        hollow.onPointer(finger(folio::ui::PointerPhase::Down, 84, blank, 5.));
        hollow.onPointer(finger(folio::ui::PointerPhase::Move, 84,
                                {blank.x, blank.y + 38.f}, 5.1));
        hollow.onPointer(finger(folio::ui::PointerPhase::Up, 84,
                                {blank.x, blank.y + 38.f}, 5.2));
        check(hollow.selectedAnnot() == circleId,
              "holding and panning outside a selected ellipse does not deselect");
        const Point grip = screen(.48f, .47f);
        hollow.onPointer(finger(folio::ui::PointerPhase::Down, 85, grip, 6.));
        hollow.onPointer(finger(folio::ui::PointerPhase::Move, 85,
                                screen(.57f, .54f), 6.1));
        hollow.onPointer(finger(folio::ui::PointerPhase::Up, 85,
                                screen(.57f, .54f), 6.2));
        check(shapeStore.get(circleId) && shapeStore.get(circleId)->box.w > .36f,
              "empty corner outside ellipse ink still grabs its 44dp resize grip");
        hollow.onPointer(finger(folio::ui::PointerPhase::Down, 86, blank, 7.));
        hollow.onPointer(finger(folio::ui::PointerPhase::Up, 86, blank, 7.1));
        check(hollow.selectedAnnot() == 0,
              "unmoved outside tap deselects an annotation");
        hollow.selectAll();
        const std::string original = hollow.selectedText();
        hollow.onPointer(finger(folio::ui::PointerPhase::Down, 87, blank, 8.));
        hollow.onPointer(finger(folio::ui::PointerPhase::Move, 87,
                                {blank.x, blank.y + 40.f}, 8.1));
        hollow.onPointer(finger(folio::ui::PointerPhase::Up, 87,
                                {blank.x, blank.y + 40.f}, 8.2));
        check(!original.empty() && hollow.selectedText() == original,
              "text selection survives a page-pan gesture");
        hollow.onPointer(finger(folio::ui::PointerPhase::Down, 88, blank, 9.));
        hollow.onPointer(finger(folio::ui::PointerPhase::Up, 88, blank, 9.1));
        check(!hollow.hasSelection(),
              "outside tap clears text range rather than only its popup");
        const auto author = [&](folio::ui::PageStage::EditTool tool,
                                folio::ui::AnnotKind expected, int pointer) {
            hollow.setEditTool(tool);
            const Point start = screen(.16f, .58f), end = screen(.46f, .67f);
            hollow.onPointer(finger(folio::ui::PointerPhase::Down, pointer, start, 9.5));
            hollow.onPointer(finger(folio::ui::PointerPhase::Move, pointer, end, 9.6));
            hollow.onPointer(finger(folio::ui::PointerPhase::Up, pointer, end, 9.7));
            const auto& all = shapeStore.all();
            check(!all.empty() && all.back().kind == expected &&
                      all.back().points.size() == 2,
                  expected == folio::ui::AnnotKind::Line
                      ? "straight line is a two-endpoint native annotation"
                      : "arrow is a two-endpoint editable annotation");
        };
        author(folio::ui::PageStage::EditTool::Line,
               folio::ui::AnnotKind::Line, 90);
        author(folio::ui::PageStage::EditTool::Arrow,
               folio::ui::AnnotKind::Arrow, 91);
        hollow.setEditTool(folio::ui::PageStage::EditTool::None);
        hollow.selectAll();
        const std::string selectedMouse = hollow.selectedText();
        auto mouse = [&](folio::ui::PointerPhase phase, Point at) {
            folio::ui::PointerEvent e;
            e.pointerType = folio::ui::PointerType::Mouse;
            e.button = folio::ui::MouseButton::Left;
            e.phase = phase;
            e.pos = e.windowPos = at;
            return e;
        };
        hollow.onPointer(mouse(folio::ui::PointerPhase::Down, blank));
        hollow.pressed = true; // the production router owns press capture
        hollow.onPointer(mouse(folio::ui::PointerPhase::Move,
                               {blank.x, blank.y + 35.f}));
        hollow.onPointer(mouse(folio::ui::PointerPhase::Up,
                               {blank.x, blank.y + 35.f}));
        hollow.pressed = false;
        check(hollow.selectedText() == selectedMouse,
              "desktop mouse page pan also retains selected text");
        hollow.onPointer(mouse(folio::ui::PointerPhase::Down, blank));
        hollow.onPointer(mouse(folio::ui::PointerPhase::Up, blank));
        check(!hollow.hasSelection(),
              "desktop outside click deselects on release");
        circle.comment = "First line\nUTF-8: \xE0\xA6\xA8\xE0\xA7\x8B\xE0\xA6\x9F";
        shapeStore.replace(circleId, circle);
        const auto sidecar = std::filesystem::temp_directory_path() /
                             "folio-note-sidecar-test.txt";
        check(shapeStore.saveSidecar(sidecar.string(), "test/document.pdf"),
              "native Folio comment sidecar saves atomically");
        folio::ui::AnnotationStore recovered;
        check(recovered.loadSidecar(sidecar.string(), "test/document.pdf") &&
                  recovered.get(circleId) &&
                  recovered.get(circleId)->comment == circle.comment,
              "comment and annotation geometry survive reopening in Folio");
        std::filesystem::remove(sidecar);
    }

    check(spy.dots.size() == 4 && spy.dots[0].radius == 9.f &&
              spy.dots[1].radius < 7.f && spy.dots[2].radius == 9.f,
          "start and end dots have quiet visible halos, not 44dp painted disks");
    if (spy.dots.size() < 4 || spy.bands.size() < 3) return;
    const Point end = spy.dots[3].center;
    const auto firstBand = std::find_if(spy.bands.begin(), spy.bands.end(),
                           [](const Spy::Band& b) { return b.top.a == 48; });
    if (firstBand == spy.bands.end()) return;
    const Point firstWord{firstBand->rect.x + firstBand->rect.w * .85f,
                          firstBand->rect.y + firstBand->rect.h * .5f};
    const float originalScroll = selectionStage.scrollY();
    selectionStage.onPointer(finger(folio::ui::PointerPhase::Down, 21,
                                    {end.x + 16.f, end.y + 5.f}, 3.));
    selectionStage.onPointer(finger(folio::ui::PointerPhase::Move, 21,
                                    firstWord, 3.05));
    selectionStage.onPointer(finger(folio::ui::PointerPhase::Up, 21,
                                    firstWord, 3.1));
    check(selectionStage.selectedText() == "AB" && clearedPopup == 1 &&
              committed == 1 && selectionStage.scrollY() == originalScroll,
          "22dp-radius touch grip adjusts the end instead of panning, then reopens markup");

    selectionStage.selectAll();
    selectionStage.onPointer(finger(folio::ui::PointerPhase::Down, 31,
                                    end, 4.));
    selectionStage.onPointer(finger(folio::ui::PointerPhase::Move, 31,
                                    firstWord, 4.03));
    selectionStage.onPointer(finger(folio::ui::PointerPhase::Down, 32,
                                    {end.x - 40.f, end.y - 30.f}, 4.04));
    check(selectionStage.selectedText() == "ABCD",
          "a second finger cancels the unfinished grip move before pinch");
    selectionStage.onPointer(finger(folio::ui::PointerPhase::Up, 32,
                                    {end.x - 40.f, end.y - 30.f}, 4.06));
    selectionStage.onPointer(finger(folio::ui::PointerPhase::Up, 31,
                                    firstWord, 4.07));
    selectionStage.onPointer(finger(folio::ui::PointerPhase::Down, 41,
                                    end, 4.2));
    selectionStage.onPointer(finger(folio::ui::PointerPhase::Move, 41,
                                    firstWord, 4.23));
    selectionStage.onPointer(finger(folio::ui::PointerPhase::Cancel, 0,
                                    firstWord, 4.24));
    check(selectionStage.selectedText() == "ABCD",
          "window-wide touch cancel releases the grip and restores the selection");
    selectionStage.setPageTheme(folio::ui::PageTheme::Dark);
    spy.bands.clear(); spy.dots.clear();
    selectionStage.onPaint(spy, {folio::ui::tokens::kLight, 5., 1.f});
    check(std::any_of(spy.bands.begin(), spy.bands.end(),
                      [](const Spy::Band& b) { return b.top.a == 52; }) &&
              spy.dots.size() == 4 && spy.dots[1].color.r == 255,
          "dark paper uses its own contrast palette, not sepia's fixed amber ink");

    section("multiline comment editing keeps the caret visible and touch-positionable");
    folio::ui::TextField note("Add a note");
    note.setFrame(0, 0, 160, 78);
    note.multiline = true;
    note.text = "top\nmiddle\nbottom\nlast";
    note.focused = true;
    note.setCaretToEnd();
    TextSpy textSpy;
    note.onPaint(textSpy, {folio::ui::tokens::kLight, 0., 1.f});
    check(!textSpy.rows.empty() && textSpy.rows.front() == "bottom",
          "long note initially scrolls to its most recent rows");
    folio::ui::KeyEvent home{};
    home.key = folio::ui::Key::Home;
    note.onKey(home);
    textSpy.rows.clear();
    note.onPaint(textSpy, {folio::ui::tokens::kLight, 0., 1.f});
    check(!textSpy.rows.empty() && textSpy.rows.front() == "top",
          "Home scrolls the editor back to the caret's first row");
    folio::ui::PointerEvent tap{};
    tap.phase = folio::ui::PointerPhase::Down;
    tap.pos = {24.f, 10.f};
    tap.timestamp = 1.;
    note.onPointer(tap);
    folio::ui::KeyEvent insert{};
    insert.text = "X";
    note.onKey(insert);
    check(note.text.rfind("toXp\nmiddle", 0) == 0,
          "tapping mid-line edits at the tapped glyph, not only at the end");
}
