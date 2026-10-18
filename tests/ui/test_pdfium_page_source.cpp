// End-to-end, headless regression for the white-page-at-max-zoom bug.
// Runs against the real PDFium worker without needing a GPU/display server.
#include "pdfium/PdfiumPageSource.h"
#include "../Harness.h"

#include <chrono>
#include <thread>

using namespace folio::render;
using namespace folio::test;

namespace {
class SpyPaint final : public IPaintTarget {
public:
    int images = 0;
    int lastWidth = 0;
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
    void fillRoundedRect(const Rect&, const Corners&, const Color&) override {}
    void strokeRoundedRect(const Rect&, const Corners&, const Color&, float) override {}
    void drawShadow(const Rect&, const Corners&, const Shadow&) override {}
    void fillCircle(const Point&, float, const Color&) override {}
    void drawLine(const Point&, const Point&, const Color&, float) override {}
    void fillRectVerticalGradient(const Rect&, const Color&, const Color&) override {}
    void setBlendMode(BlendMode) override {}
    void strokePath(std::span<const PathVerb>, const Color&, float,
                    LineCap, LineJoin) override {}
    void fillPath(std::span<const PathVerb>, const Color&) override {}
    void drawText(std::string_view, const Point&, const TextStyle&, TextBaseline) override {}
    void drawTextInBox(std::string_view, const Rect&, const TextStyle&, TextAlign) override {}
    float measureText(std::string_view, const TextStyle&) override { return 0; }
    FontMetrics fontMetrics(const TextStyle&) override { return {}; }
    bool drawCached(std::uint64_t, const Rect&, int, int,
                    const std::function<void(IPaintTarget&)>&) override { return false; }
    void drawImageBGRA(const void*, int width, int, std::size_t, const Rect&,
                       std::uint64_t, ImageFit, ImageTint) override {
        ++images;
        lastWidth = width;
    }
    void invalidateRasterCache() override {}
    RasterCacheStats rasterCacheStats() const override { return {}; }
    int width() const override { return 800; }
    int height() const override { return 600; }
    std::string_view backendName() const override { return "spy"; }
};

bool waitForImage(folio::engine::PdfiumPageSource& source, SpyPaint& spy,
                  int page, float scale, int minWidth) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline) {
        source.tick();
        spy.images = 0;
        source.drawPage(spy, page, Rect{0, 0, 200, 100}, scale,
                        folio::ui::PageTheme::Light);
        if (spy.images && spy.lastWidth >= minWidth) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

FOLIO_TEST_MAIN("PDFium source zoom and eviction") {
    folio::engine::PdfiumPageSource source;
    SpyPaint spy;
    check(source.open("folio-test.pdf"), "opens a real mapped PDF");
    if (!source.isOpen()) return;
    check(source.pageCount() == 3 && source.pageSize(0).w == 200.f,
          "open publishes page count and first page geometry");
    bool loadedAnnotations = false;
    for (int i = 0; i < 200 && !loadedAnnotations; ++i) {
        source.tick();
        loadedAnnotations = source.annotationsForPage(0) != nullptr;
        if (!loadedAnnotations) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(loadedAnnotations, "imported annotation metadata can be indexed on demand");
    source.parkVisualCaches(0);
    check(source.annotationsForPage(0) == nullptr,
          "parking a tab drops indexed imported annotations as well as visual pixels");
    loadedAnnotations = false;
    for (int i = 0; i < 200 && !loadedAnnotations; ++i) {
        source.tick();
        loadedAnnotations = source.annotationsForPage(0) != nullptr;
        if (!loadedAnnotations) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(loadedAnnotations, "returning to a parked tab can re-index imported marks");
    check(source.geometryRevision() == 0 && source.pageSize(1).w == 200.f,
          "distant geometry is provisional rather than eagerly scanned");
    source.setFocusPage(0);
    check(waitForImage(source, spy, 0, 1.f, 200), "first page draws bitmap");
    const auto bytesBeforeSwitch = source.cacheStats().bytes;
    source.suppressSourceAnnotations({}); // tab return with the same sidecar
    check(source.cacheStats().bytes == bytesBeforeSwitch &&
              source.hasCachedPageProxy(0),
          "unchanged imported annotations do not delete the parked preview");
    source.suppressSourceAnnotations({{0, {{999, 1}}}});
    check(source.cacheStats().bytes == 0 && !source.hasCachedPageProxy(0),
          "a real imported-annotation change invalidates old visual pixels");
    source.suppressSourceAnnotations({});
    for (int i = 0; i < 300 && source.geometryRevision() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(source.geometryRevision() > 0 && source.pageSize(1).w == 612.f,
          "nearby page geometry loads on demand after the first page");
    check(waitForImage(source, spy, 0, 32.f, 4000),
          "max-zoom page resolves to a high-resolution bitmap");
    bool thumbReady = false;
    for (int i = 0; i < 300 && !thumbReady; ++i) {
        source.tick();
        spy.images = 0;
        source.drawPage(spy, 0, Rect{0, 0, 120, 60}, 0.6f,
                        folio::ui::PageTheme::Light);
        thumbReady = spy.images == 1 && spy.lastWidth == 120;
        if (!thumbReady) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(thumbReady, "thumbnail retains a separate scale for the same page");
    const auto stableBytes = source.cacheStats().bytes;
    for (int i = 0; i < 20; ++i) {
        spy.images = 0;
        source.drawPage(spy, 0, Rect{0, 0, 200, 100}, 32.f,
                        folio::ui::PageTheme::Light);
        check(spy.images == 1 && spy.lastWidth >= 4000,
              "canvas scale stays sharp beside a thumbnail");
        spy.images = 0;
        source.drawPage(spy, 0, Rect{0, 0, 120, 60}, 0.6f,
                        folio::ui::PageTheme::Light);
        check(spy.images == 1 && spy.lastWidth == 120,
              "thumbnail stays sharp without replacing the canvas request");
    }
    check(source.cacheStats().bytes == stableBytes,
          "alternating thumbnail/canvas draws do not churn PDFium rasters");
    check(waitForImage(source, spy, 1, 32.f, 3000),
          "adjacent high-resolution page loads while the first stays in view");
    spy.images = 0;
    source.drawPage(spy, 0, Rect{0, 0, 200, 100}, 32.f,
                    folio::ui::PageTheme::Light);
    check(spy.images == 1,
          "cache pressure retains a proxy for both visible pages, not a white flash");
    source.trimTo(2, 0); // cache eviction must invalidate the request marker
    source.setFocusPage(0);
    check(waitForImage(source, spy, 0, 32.f, 4000),
          "revisit at the same max zoom re-renders instead of staying white");
}
