// Retained-scene regression: a title-bar hover repaints chrome, not the PDF.
#include "screens/FolioApp.h"
#include "components/Button.h"
#include "../../folio-render/skia/SkiaPaintTarget.h"
#include "../Harness.h"

#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

using namespace folio::ui;
using namespace folio::render;
using namespace folio::test;

namespace {
struct MixedWidthProbe final : IPageSource {
    int pageCount() const override { return 2; }
    PageSizePt pageSize(int i) const override {
        return i == 0 ? PageSizePt{612.f, 792.f}
                      : PageSizePt{280.f, 792.f};
    }
    void drawPage(IPaintTarget&, int, const Rect&, float, PageTheme) override {}
};
struct CoverGeometryProbe final : IPageSource {
    bool resolved = false;
    int pageCount() const override { return 8; }
    PageSizePt pageSize(int i) const override {
        return resolved && i > 0 && i < 4 ? PageSizePt{600.f, 800.f}
                                             : PageSizePt{200.f, 300.f};
    }
    unsigned geometryRevision() const override { return resolved ? 1u : 0u; }
    void drawPage(IPaintTarget&, int, const Rect&, float, PageTheme) override {}
};
struct ViewportProbe final : IPageSource {
    struct Draw { int page; Rect dest, visible; };
    std::vector<Draw> draws;
    int pageCount() const override { return 3; }
    PageSizePt pageSize(int) const override { return {612.f, 792.f}; }
    void drawPage(IPaintTarget&, int, const Rect&, float, PageTheme) override {}
    bool cachesOwnRasters() const override { return true; }
    void drawVisiblePage(IPaintTarget&, int page, const Rect& dest,
                         const Rect& visible, float, PageTheme) override {
        draws.push_back({page, dest, visible});
    }
};
struct ThumbRasterProbe final : IPageSource {
    std::vector<Rect> visible;
    std::vector<PageGlyph> noText;
    int pageCount() const override { return 1; }
    PageSizePt pageSize(int) const override { return {600.f, 800.f}; }
    void drawPage(IPaintTarget&, int, const Rect&, float, PageTheme) override {}
    bool cachesOwnRasters() const override { return true; }
    const std::vector<PageGlyph>* glyphs(int) override { return &noText; }
    void drawVisiblePage(IPaintTarget& t, int, const Rect&, const Rect& area,
                         float, PageTheme) override {
        visible.push_back(area);
        t.fillRect(area, Color{235, 233, 227, 255});
    }
};
}

FOLIO_TEST_MAIN("retained chrome redraw") {
    auto surface = SkSurfaces::Raster(SkImageInfo::Make(
        1200, 800, kBGRA_8888_SkColorType, kPremul_SkAlphaType));
    check(surface != nullptr, "persistent raster surface available");
    if (!surface) return;
    skia::SkiaPaintTarget target(surface->getCanvas());
    FolioApp app(1200.f, 800.f);
    app.showScreen(Screen::Reader, 1.0);
    app.paint(target, 2.0, 1.f, true);
    app.paint(target, 3.0, 1.f, true);

    // A screen change should release the previous full-screen opacity layer
    // after 220 ms, not keep painting two screens for half a second.
    auto navSurface = SkSurfaces::Raster(SkImageInfo::Make(
        1200, 800, kBGRA_8888_SkColorType, kPremul_SkAlphaType));
    check(navSurface != nullptr, "transition regression has its own render surface");
    if (!navSurface) return;
    skia::SkiaPaintTarget navTarget(navSurface->getCanvas());
    FolioApp navigation(1200.f, 800.f);
    navigation.showScreen(Screen::Reader, 1.0);
    navigation.paint(navTarget, 1.23, 1.f, true);
    ReaderScreen* navigationReader = nullptr;
    WelcomeScreen* navigationHome = nullptr;
    navigation.root().forEach([&](Widget& w) {
        if (auto* r = dynamic_cast<ReaderScreen*>(&w)) navigationReader = r;
        if (auto* h = dynamic_cast<WelcomeScreen*>(&w)) navigationHome = h;
    });
    check(navigationReader && navigationHome && navigationReader->visible &&
          !navigationHome->visible,
          "home-to-reader fade releases home within 220 ms");
    navigation.showScreen(Screen::Welcome, 2.0);
    navigation.paint(navTarget, 2.23, 1.f, true);
    check(navigationReader && navigationHome && navigationHome->visible &&
          !navigationReader->visible,
          "reader-to-home fade releases reader within 220 ms");

    Button* close = nullptr;
    app.root().forEach([&](Widget& w) {
        if (auto* b = dynamic_cast<Button*>(&w);
            b && b->debugName == "win.close" && b->visible &&
            b->parent() && b->parent()->parent() &&
            b->parent()->parent()->debugName == "ReaderScreen") close = b;
    });
    check(close != nullptr, "reader has a close control");
    if (!close) return;
    const auto before = target.rasterCacheStats();
    SkPixmap pm;
    const bool pixels = surface->peekPixels(&pm);
    check(pixels, "persistent pixels are readable");
    const auto paperBefore = pixels ? *pm.addr32(600, 500) : 0u;

    PointerEvent move{};
    move.phase = PointerPhase::Move;
    move.timestamp = 4.0;
    move.pos = move.windowPos = close->toWindow({close->frame.w * 0.5f,
                                                  close->frame.h * 0.5f});
    check(app.pointer(move), "entering the control requests a hover frame");
    app.paint(target, 4.0, 1.f, true);
    const auto partial = app.stats(4.0);
    check(partial.partial && partial.damage.w * partial.damage.h < 1200.f * 800.f * 0.3f,
          "hover paints only a bounded damage rectangle");
    check(target.rasterCacheStats().hits == before.hits &&
          target.rasterCacheStats().misses == before.misses,
          "chrome repaint does not draw or re-rasterize the PDF canvas");
    if (pixels) check(*pm.addr32(600, 500) == paperBefore,
                      "document pixels are retained across a chrome repaint");

    move.timestamp = 4.05;
    check(!app.pointer(move), "stationary hover does not request a new frame");

    // Regression: a popup over the PDF used to clear a rectangular damage
    // region while the chrome-only traversal skipped painting the page.
    Popover* popup = nullptr;
    app.root().forEach([&](Widget& w) {
        if (!popup && dynamic_cast<Popover*>(&w) && w.parent() &&
            w.parent()->debugName == "ReaderScreen")
            popup = static_cast<Popover*>(&w);
    });
    check(popup != nullptr, "reader owns a popover for overlay regression");
    if (popup) {
        popup->setFrame(0.f, 0.f, 180.f, 160.f);
        popup->windowSize = {1200.f, 800.f};
        popup->open({600.f, 370.f}, 5.0);
        app.paint(target, 5.1, 1.f, true);
        check(app.stats(5.1).partial &&
                  app.stats(5.1).damage.w < 1200.f,
              "selection-style popup repaints only its clipped PDF underlay");
        popup->close(5.2);
        app.paint(target, 5.25, 1.f, true);
        check(app.stats(5.25).partial,
              "dismissal repaints only the PDF beneath the former popup");
        app.paint(target, 5.50, 1.f, true);
        if (pixels) check(*pm.addr32(600, 500) == paperBefore,
                          "last exit frame restores cached paper, without a grey hole");
    }
    app.resize(1100.f, 800.f);
    app.paint(target, 5.6, 1.f, true);
    check(!app.stats(5.6).partial, "layout resize forces a full repaint");

    auto probe = std::make_shared<ViewportProbe>();
    PageStage stage;
    stage.setFrame(0.f, 0.f, 390.f, 640.f);
    stage.setSource(probe, 0.0);
    stage.setZoom(3.f, 0.0);
    const PaintCtx ctx{app.state().theme.effective(2.0), 2.0, 1.f};
    stage.paintTree(target, ctx);
    check(!probe->draws.empty(), "main canvas supplies source with a visible rectangle");
    for (const auto& d : probe->draws)
        check(d.visible.w > 0.f && d.visible.h > 0.f &&
              d.visible.x >= d.dest.x && d.visible.y >= d.dest.y &&
              d.visible.right() <= d.dest.right() &&
              d.visible.bottom() <= d.dest.bottom() &&
              d.visible.h <= stage.frame.h && d.visible.h < d.dest.h,
              "stage passes only the on-screen portion of a high-zoom page");

    section("short desktop zoom and panel-driven fit reuse a stable raster scale");
    auto zoomProbe = std::make_shared<ViewportProbe>();
    PageStage zoomStage;
    zoomStage.setFrame(0.f, 0.f, 480.f, 640.f);
    zoomStage.setSource(zoomProbe, 10.0);
    zoomStage.paintTree(target, PaintCtx{ctx.pal, 10.5, 1.f});
    zoomProbe->draws.clear();
    zoomStage.setZoom(2.f, 11.0);
    zoomStage.paintTree(target, PaintCtx{ctx.pal, 11.04, 1.f});
    check(!zoomProbe->draws.empty() &&
          zoomProbe->draws.front().dest.w > 700.f,
          "desktop zoom makes perceptible progress in the first 40 ms");
    zoomProbe->draws.clear();
    zoomStage.paintTree(target, PaintCtx{ctx.pal, 11.16, 1.f});
    check(!zoomStage.animating(11.16) && !zoomProbe->draws.empty() &&
          zoomProbe->draws.front().dest.w > 1200.f,
          "desktop zoom completes in 150 ms without a 400 ms tail");
    zoomStage.setZoomMode(ZoomMode::FitWidth, 12.0);
    zoomProbe->draws.clear();
    zoomStage.paintTree(target, PaintCtx{ctx.pal, 12.2, 1.f});
    zoomStage.setFrame(0.f, 0.f, 430.f, 640.f);
    zoomProbe->draws.clear();
    zoomStage.paintTree(target, PaintCtx{ctx.pal, 12.22, 1.f});
    check(!zoomProbe->draws.empty() &&
          std::fabs(zoomProbe->draws.front().dest.w - 374.f) < 2.f &&
          zoomStage.animating(12.22),
          "Fit width follows the panel immediately and schedules final sharp raster");
    zoomStage.paintTree(target, PaintCtx{ctx.pal, 12.24, 1.f});
    check(!zoomStage.animating(12.24),
          "panel fit adopts the new raster scale once, then sleeps");

    section("panel content stays inside the moving edge on a real Skia surface");
    struct OutsideChild : Widget {
        void onPaint(IPaintTarget& paint, const PaintCtx&) override {
            paint.fillRect({0.f, 0.f, 190.f, 44.f}, Color{255, 0, 255, 255});
        }
    };
    auto panelSurface = SkSurfaces::Raster(SkImageInfo::Make(
        280, 200, kBGRA_8888_SkColorType, kPremul_SkAlphaType));
    check(panelSurface != nullptr, "pixel-level panel clip target exists");
    if (panelSurface) {
        skia::SkiaPaintTarget panelTarget(panelSurface->getCanvas());
        panelTarget.clear(Color{16, 28, 51, 255});
        SidePanel smallPanel(SidePanel::Side::Left);
        smallPanel.setFrame(0.f, 0.f, 70.f, 160.f);
        auto* bleed = smallPanel.emplace<OutsideChild>();
        bleed->setFrame(10.f, 30.f, 190.f, 44.f);
        smallPanel.paintTree(panelTarget, PaintCtx{ctx.pal, 13., 1.f});
        SkPixmap panelPixels;
        if (panelSurface->peekPixels(&panelPixels)) {
            const auto inside = *panelPixels.addr32(20, 45);
            const auto outside = *panelPixels.addr32(150, 45);
            check(inside != outside && outside == 0xff101c33u,
                  "thumbnail-like child is clipped at panel edge, not drawn on the PDF");
        }
    }

    // At minimum zoom the viewport midpoint lands on the second, NARROW
    // page. Increasing zoom makes the first page wider than the viewport
    // while the anchor page still fits. The entire shared-width document
    // must remain horizontally centred, not snap against the left edge.
    PageStage mixed;
    mixed.setFrame(0.f, 0.f, 390.f, 640.f);
    mixed.setSource(std::make_shared<MixedWidthProbe>(), 3.0);
    mixed.setZoom(0.25f, 3.0);
    mixed.paintTree(target, PaintCtx{ctx.pal, 3.5, 1.f});
    int anchorPage = -1;
    check(mixed.hitPage({195.f, 320.f}, &anchorPage, nullptr) && anchorPage == 1,
          "minimum zoom centre anchors to the narrow second page");
    mixed.setZoom(1.f, 3.5);
    mixed.paintTree(target, PaintCtx{ctx.pal, 4.0, 1.f});
    const float expectedCentre = (mixed.contentWidth() - mixed.frame.w) * 0.5f;
    check(expectedCentre > 0.f &&
              std::fabs(mixed.scrollX() - expectedCentre) < 1.f,
          "zooming in from minimum keeps mixed-width PDF centred, not left pinned");
    int afterPage = -1;
    Point atCentre{};
    check(mixed.hitPage({195.f, 320.f}, &afterPage, &atCentre) &&
              afterPage == 1 && std::fabs(atCentre.x - 0.5f) < 0.01f,
          "narrow anchor page stays under the midpoint after zooming in");

    section("real page dimensions replace a tiny cover without jumping the scroll position");
    auto cover = std::make_shared<CoverGeometryProbe>();
    PageStage coverStage;
    coverStage.setFrame(0.f, 0.f, 390.f, 640.f);
    coverStage.setSource(cover, 1.0);
    coverStage.setZoom(1.f, 1.0);
    coverStage.snapZoomToTarget(1.0);
    coverStage.goToPage(5, 1.0);
    coverStage.paintTree(target, PaintCtx{ctx.pal, 2.0, 1.f});
    const float beforeGeometry = coverStage.scrollY();
    check(coverStage.currentPage() == 5 && beforeGeometry > 500.f,
          "fast scroll has moved beyond the cover and into the book");
    cover->resolved = true;
    check(coverStage.geometryChanged(), "async source publishes corrected page geometry");
    coverStage.paintTree(target, PaintCtx{ctx.pal, 3.0, 1.f});
    check(coverStage.currentPage() == 5 &&
              std::fabs(coverStage.scrollY() - beforeGeometry - 1500.f) < 3.f,
          "geometry change keeps the same visible page anchored across three resized pages");

    section("immersive controls sleep once, wake by page tap/corner, and never trap exit");
    FolioApp phone(390.f, 800.f);
    phone.showScreen(Screen::Reader, 1.0);
    phone.paint(target, 22.0, 1.f, true);
    phone.paint(target, 22.8, 1.f, true);
    ReaderScreen* reader = nullptr;
    Button* focus = nullptr;
    Button* panelFocus = nullptr;
    Button* inspectorHandle = nullptr;
    Button* exit = nullptr;
    Button* prev = nullptr;
    Widget* wake = nullptr;
    phone.root().forEach([&](Widget& w) {
        if (auto* r = dynamic_cast<ReaderScreen*>(&w)) reader = r;
        if (w.debugName == "Focus wake zone") wake = &w;
        if (auto* b = dynamic_cast<Button*>(&w)) {
            if (b->tooltip == "Focus mode  (F)") focus = b;
            if (b->tooltip == "Focus mode (right panel)") panelFocus = b;
            if (b->tooltip == "Tool options") inspectorHandle = b;
            if (b->tooltip == "Leave focus mode  (Esc)") exit = b;
            if (b->tooltip == "Previous page") prev = b;
        }
    });
    check(reader && focus && panelFocus && inspectorHandle && exit && prev && wake,
          "phone exposes Focus in both toolbar and inspector, plus a recovery corner");
    if (reader && focus && panelFocus && inspectorHandle && exit && prev && wake) {
        inspectorHandle->onClick();
        phone.paint(target, 23.0, 1.f, true);
        check(panelFocus->visible && !phone.state().rightPanelCollapsed,
              "opening the phone right sheet exposes its pinned Focus button");
        reader->stage()->setSource(std::make_shared<MixedWidthProbe>(), 23.0);
        panelFocus->onClick();
        phone.paint(target, 23.5, 1.f, true);
        phone.paint(target, 23.9, 1.f, true);
        check(exit->visible && prev->parent()->visible && !wake->visible,
              "Focus starts with icon exit and bottom controls revealed");
        check(!reader->animating(25.9) && reader->animating(26.01),
              "auto-hide waits for 3 seconds without continuous idle frames");
        phone.paint(target, 26.01, 1.f, true);
        check(!exit->visible && !prev->parent()->visible && wake->visible &&
                  !reader->animating(26.02),
              "one idle frame hides exit and bottom bar, then sleeps");

        PointerEvent tap{};
        tap.pointerType = PointerType::Touch;
        tap.pointerId = 12;
        tap.windowPos = reader->stage()->toWindow(
            {reader->stage()->frame.w * 0.5f, reader->stage()->frame.h * 0.5f});
        tap.pos = tap.windowPos;
        tap.timestamp = 26.3;
        tap.phase = PointerPhase::Down;
        phone.pointer(tap);
        tap.timestamp = 26.38;
        tap.phase = PointerPhase::Up;
        phone.pointer(tap);
        check(exit->visible && prev->parent()->visible,
              "single paper tap reveals both controls without leaving Focus");
        tap.timestamp = 26.9;
        tap.phase = PointerPhase::Down;
        phone.pointer(tap);
        tap.timestamp = 26.98;
        tap.phase = PointerPhase::Up;
        phone.pointer(tap);
        check(!exit->visible && !prev->parent()->visible,
              "second deliberate paper tap hides both controls immediately");

        tap.windowPos = wake->toWindow({22.f, 22.f});
        tap.pos = tap.windowPos;
        tap.timestamp = 27.2;
        tap.phase = PointerPhase::Down;
        phone.pointer(tap);
        tap.timestamp = 27.28;
        tap.phase = PointerPhase::Up;
        phone.pointer(tap);
        check(exit->visible && prev->parent()->visible,
              "top-corner touch restores exit without forwarding the tap to paper");
        phone.paint(target, 27.3, 1.f, true);
        tap.pointerId = 13;
        tap.windowPos = reader->stage()->toWindow(
            {reader->stage()->frame.w * 0.5f, reader->stage()->frame.h * 0.5f});
        tap.pos = tap.windowPos;
        tap.timestamp = 27.5;
        tap.phase = PointerPhase::Down;
        phone.pointer(tap);
        phone.paint(target, 30.7, 1.f, true);
        check(exit->visible && prev->parent()->visible,
              "auto-hide never runs while a finger holds the page");
        tap.timestamp = 30.8;
        tap.phase = PointerPhase::Up;
        phone.pointer(tap);
        phone.state().autoHideFocusControls = false;
        phone.paint(target, 34.0, 1.f, true);
        check(exit->visible && prev->parent()->visible && !reader->animating(34.0),
              "opted-out controls stay visible at idle without animation");
        exit->onClick();
        check(!phone.state().focusMode && !wake->visible,
              "icon exit restores normal reading chrome");
        phone.state().autoHideFocusControls = true;
        focus->onClick();
        phone.paint(target, 34.2, 1.f, true);
        phone.paint(target, 37.01, 1.f, true);
        check(wake->visible && !exit->visible, "re-entering Focus can hide controls again");
        KeyEvent back{};
        back.key = Key::Escape; // Android Back maps to Escape
        back.pressed = true;
        phone.key(back);
        check(!phone.state().focusMode && !wake->visible,
              "Back/Escape always exits hidden-chrome Focus mode");
    }

    section("the held thumb popup shares the small retained PDF damage path");
    FolioApp hud(390.f, 800.f);
    hud.showScreen(Screen::Reader, 98.0);
    hud.state().leftPanelCollapsed = true;
    hud.state().rightPanelCollapsed = true;
    hud.root().forEach([&](Widget& w) {
        if (auto* panel = dynamic_cast<SidePanel*>(&w))
            panel->setCollapsedImmediately(true);
    });
    hud.resize(390.f, 800.f);
    ReaderScreen* hudReader = nullptr;
    hud.root().forEach([&](Widget& w) {
        if (auto* reader = dynamic_cast<ReaderScreen*>(&w)) hudReader = reader;
    });
    check(hudReader != nullptr, "thumb canvas is available in the reader");
    if (hudReader) {
        auto hudPdf = std::make_shared<ThumbRasterProbe>();
        PageStage* page = hudReader->stage();
        page->setSource(hudPdf, 100.0);
        page->setZoom(1.f, 100.0);
        hud.paint(target, 101.0, 1.f, true);
        hud.paint(target, 102.0, 1.f, true);
        check(page->hitPage({190.f, 400.f}, nullptr, nullptr),
              "known blank point is on the raster-backed PDF");
        PointerEvent hold{};
        hold.pointerType = PointerType::Touch;
        hold.pointerId = 77;
        hold.phase = PointerPhase::Down;
        hold.pos = hold.windowPos = page->toWindow({190.f, 400.f});
        hold.timestamp = 103.0;
        hud.pointer(hold);
        hud.paint(target, 103.0, 1.f, true);
        hudPdf->visible.clear();
        hud.paint(target, 103.70, 1.f, true);
        hud.paint(target, 103.82, 1.f, true);
        const auto frame = hud.stats(103.82);
        check(frame.partial && frame.damage.w < 390.f &&
                  frame.damage.w * frame.damage.h < 390.f * 800.f * .55f,
              "popover reveal damages a bounded part of the phone, not the whole PDF");
        check(!hudPdf->visible.empty() &&
                  hudPdf->visible.back().h < page->frame.h * .75f,
              "PDFium is asked to composite only tiles beneath the popup");
        hold.phase = PointerPhase::Up;
        hold.timestamp = 103.85;
        hud.pointer(hold);
        hud.paint(target, 104.2, 1.f, true);
    }
}
