#include "components/Containers.h"
#include "components/OpenDialog.h"
#include "components/Overlays.h"
#include "components/PageStage.h"
#include "components/Gestures.h"
#include "folio/ui/Widget.h"
#include "../Harness.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace folio::ui;
using namespace folio::render;
using namespace folio::test;

namespace {
struct MixedPages final : IPageSource {
    int pageCount() const override { return 2; }
    PageSizePt pageSize(int i) const override {
        return i == 0 ? PageSizePt{560.f, 800.f}
                      : PageSizePt{280.f, 800.f};
    }
    void drawPage(IPaintTarget&, int, const Rect&, float, PageTheme) override {}
};
struct LongLayoutPages final : IPageSource {
    int pageCount() const override { return 24; }
    PageSizePt pageSize(int i) const override {
        return i % 5 == 0 ? PageSizePt{792.f, 450.f}
                          : PageSizePt{612.f, 792.f};
    }
    void drawPage(IPaintTarget&, int, const Rect&, float, PageTheme) override {}
};
struct PreviewProbe final : IPageSource {
    bool ready = false;
    int pageCount() const override { return 2; }
    PageSizePt pageSize(int) const override { return {612.f, 792.f}; }
    void drawPage(IPaintTarget&, int, const Rect&, float, PageTheme) override {}
    [[nodiscard]] bool visibleFrameComplete() const noexcept override { return ready; }
};
struct GridProbe final : IPageSource {
    struct Paint { int page; Rect dest; float scale; };
    std::vector<Paint> paints;
    int first = -1, last = -1, bufferFirst = -1, bufferLast = -1;
    int pixelW = 0, pixelH = 0, glyphRequests = 0, tileRequests = 0;
    int pageCount() const override { return 60; }
    PageSizePt pageSize(int p) const override {
        return p % 4 == 0 ? PageSizePt{792.f, 450.f}
                          : PageSizePt{612.f, 792.f};
    }
    bool cachesOwnRasters() const override { return true; }
    void drawPage(IPaintTarget&, int page, const Rect& dst,
                  float scale, PageTheme) override {
        paints.push_back({page, dst, scale});
    }
    void drawVisiblePage(IPaintTarget&, int, const Rect&, const Rect&,
                         float, PageTheme) override { ++tileRequests; }
    const std::vector<PageGlyph>* glyphs(int) override {
        ++glyphRequests;
        return nullptr;
    }
    void setVisibleGridRange(int a, int b, int c, int d,
                             int w, int h) override {
        first = a; last = b; bufferFirst = c; bufferLast = d;
        pixelW = w; pixelH = h;
    }
};
struct ColumnText final : IPageSource {
    std::vector<PageGlyph> chars{
        {U'A', .10f, .10f, .14f, .13f},
        {U'B', .14f, .10f, .18f, .13f},
        {U'C', .69f, .10f, .73f, .13f},
        {U'D', .73f, .10f, .77f, .13f},
        {U'X', std::numeric_limits<float>::quiet_NaN(), .1f, .9f, .2f},
    };
    int pageCount() const override { return 1; }
    PageSizePt pageSize(int) const override { return {600.f, 800.f}; }
    void drawPage(IPaintTarget&, int, const Rect&, float, PageTheme) override {}
    const std::vector<PageGlyph>* glyphs(int) override { return &chars; }
};
struct TapTarget final : Widget {
    int taps = 0;
    int cancels = 0;
    TapTarget() { acceptsPointer = true; }
    bool onPointer(const PointerEvent& e) override {
        if (e.phase == PointerPhase::Up) ++taps;
        if (e.phase == PointerPhase::Cancel) ++cancels;
        return true;
    }
};
PointerEvent touch(PointerPhase phase, PointerId id, float x, float y,
                   double seconds) {
    PointerEvent e;
    e.phase = phase;
    e.pointerType = PointerType::Touch;
    e.pointerId = id;
    e.windowPos = e.pos = {x, y};
    e.timestamp = seconds;
    return e;
}
struct GesturePaint final : IPaintTarget {
    std::vector<Rect> roundedFills;
    std::vector<Rect> roundedStrokes;
    std::vector<std::string> labels;
    int icons = 0;
    int circles = 0;
    int arcPaths = 0;
    int layers = 0;
    std::vector<float> rotations;
    std::vector<Rect> revealClips;
    std::vector<Rect> panelClips;
    std::vector<std::string> paintEvents;
    int viewportImages = 0, invalidatedImages = 0;
    struct Transform { float x = 0.f, y = 0.f, sx = 1.f, sy = 1.f; } transform;
    std::vector<Transform> stack;
    std::vector<Point> iconCentres;
    void save() override { stack.push_back(transform); }
    void restore() override { transform = stack.back(); stack.pop_back(); }
    void translate(float x, float y) override {
        transform.x += x * transform.sx; transform.y += y * transform.sy;
    }
    void scale(float x, float y) override {
        transform.sx *= x; transform.sy *= y;
    }
    void rotateDegrees(float angle) override { rotations.push_back(angle); }
    void clipRect(const Rect& r, bool) override {
        panelClips.push_back(r);
        paintEvents.push_back("clip");
    }
    void clipRoundedRect(const Rect& r, const Corners&, bool) override {
        if (std::fabs(r.w - r.h) < .02f &&
            std::fabs(r.x + r.w * .5f - 190.f) < .02f &&
            std::fabs(r.y + r.h * .5f - 400.f) < .02f)
            revealClips.push_back(r);
    }
    void saveLayerAlpha(float) override { ++layers; save(); } void clear(const Color&) override {}
    void fillRect(const Rect&, const Color&) override {}
    void fillRoundedRect(const Rect& r, const Corners&, const Color&) override {
        roundedFills.push_back(r);
    }
    void strokeRoundedRect(const Rect& r, const Corners&, const Color&, float) override {
        roundedStrokes.push_back(r);
    }
    void drawShadow(const Rect&, const Corners&, const Shadow&) override {
        paintEvents.push_back("shadow");
    }
    void fillCircle(const Point&, float, const Color&) override { ++circles; }
    void drawLine(const Point&, const Point&, const Color&, float) override {}
    void fillRectVerticalGradient(const Rect&, const Color&, const Color&) override {}
    void setBlendMode(BlendMode) override {}
    void strokePath(std::span<const PathVerb>, const Color&, float, LineCap, LineJoin) override {
        ++icons;
        iconCentres.push_back({transform.x + 12.f * transform.sx,
                               transform.y + 12.f * transform.sy});
    }
    void fillPath(std::span<const PathVerb> verbs, const Color&) override {
        if (verbs.size() >= 12) ++arcPaths;
    }
    void drawText(std::string_view, const Point&, const TextStyle&, TextBaseline) override {}
    void drawTextInBox(std::string_view text, const Rect&, const TextStyle&, TextAlign) override {
        labels.emplace_back(text);
    }
    float measureText(std::string_view, const TextStyle&) override { return 0.f; }
    FontMetrics fontMetrics(const TextStyle&) override { return {}; }
    bool drawCached(std::uint64_t, const Rect&, int, int,
                    const std::function<void(IPaintTarget&)>&) override { return false; }
    void drawImageBGRA(const void*, int, int, std::size_t, const Rect&,
                       std::uint64_t, ImageFit, ImageTint) override { ++viewportImages; }
    void invalidateImage(std::uint64_t) override { ++invalidatedImages; }
    void invalidateRasterCache() override {}
    RasterCacheStats rasterCacheStats() const override { return {}; }
    int width() const override { return 390; }
    int height() const override { return 640; }
    std::string_view backendName() const override { return "gesture-test"; }
};
} // namespace

FOLIO_TEST_MAIN("mobile input and layouts") {
    section("tap a child; dragging that child scrolls instead of opening it");
    Widget root;
    root.setFrame(0, 0, 390, 800);
    auto* list = root.emplace<ScrollView>();
    list->setFrame(0, 0, 390, 350);
    list->contentHeight = 1000;
    auto* card = list->emplace<TapTarget>();
    card->setFrame(0, 0, 390, 240);
    EventRouter router;
    router.dispatchPointer(root, touch(PointerPhase::Down, 1, 120, 100, 1));
    router.dispatchPointer(root, touch(PointerPhase::Up, 1, 120, 100, 1.1));
    check(card->taps == 1 && list->scrollY() == 0, "stationary tap opens card");
    router.dispatchPointer(root, touch(PointerPhase::Down, 2, 120, 160, 2));
    router.dispatchPointer(root, touch(PointerPhase::Move, 2, 120, 100, 2.05));
    router.dispatchPointer(root, touch(PointerPhase::Up, 2, 120, 100, 2.1));
    check(card->taps == 1, "vertical drag never taps card");
    check(list->scrollY() >= 59.f, "vertical drag scrolls its ancestor");
    check(card->cancels == 1, "drag cancels the child's press");

    section("panel edge clips thumbnail children, not the card shadow");
    SidePanel panel(SidePanel::Side::Left);
    panel.setExpandedWidth(268.f, 0.0);
    panel.setCollapsedImmediately(true);
    panel.setCollapsed(false, 1.0);
    const float panelMid = panel.currentWidth(1.09);
    check(panel.clipsContent && !panel.clipSelfPaint &&
          panelMid > tokens::size::panelRailCollapsed && panelMid < 268.f &&
          panel.contentAlpha(1.09) > .05f && panel.contentAlpha(1.09) < .95f,
          "body fade and clipped edge advance together at panel midpoint");
    panel.setFrame(0.f, 0.f, panelMid, 360.f);
    GesturePaint panelPaint;
    panel.paintTree(panelPaint, {tokens::kLight, 1.09, 1.f});
    const auto shadow = std::find(panelPaint.paintEvents.begin(),
                                  panelPaint.paintEvents.end(), "shadow");
    const auto clip = std::find(panelPaint.paintEvents.begin(),
                                panelPaint.paintEvents.end(), "clip");
    check(shadow != panelPaint.paintEvents.end() &&
          clip != panelPaint.paintEvents.end() && shadow < clip &&
          std::any_of(panelPaint.panelClips.begin(), panelPaint.panelClips.end(),
              [panelMid](const Rect& r) { return std::fabs(r.w - panelMid) < .1f; }),
          "shadow stays outside, thumbnails cannot escape animated panel frame");
    panel.setCollapsed(true, 1.1);
    const float panelClosing = panel.currentWidth(1.19);
    check(panelClosing < panelMid &&
          panel.contentAlpha(1.19) < panel.contentAlpha(1.09) &&
          !panel.animating(1.3),
          "collapse hides body in lockstep and finishes within 180 ms");

    section("Home card hold and mouse context do not open the document");
    RecentCard homeCard("Name.pdf", "Offline copy", 0);
    homeCard.setFrame(0, 0, 210, 196);
    int cardOpens = 0, cardMenus = 0;
    homeCard.onOpen = [&] { ++cardOpens; };
    homeCard.onMenu = [&](Point) { ++cardMenus; };
    homeCard.onPointer(touch(PointerPhase::Down, 71, 70, 60, 8.));
    homeCard.onPointer(touch(PointerPhase::Up, 71, 70, 60, 8.12));
    check(cardOpens == 1 && cardMenus == 0, "quick touch opens a recent PDF");
    homeCard.onPointer(touch(PointerPhase::Down, 72, 70, 60, 9.));
    homeCard.onPointer(touch(PointerPhase::Up, 72, 70, 60, 9.52));
    check(cardOpens == 1 && cardMenus == 1, "0.5 second hold opens only the card menu");
    PointerEvent right{};
    right.phase = PointerPhase::Down;
    right.button = MouseButton::Right;
    right.pos = {70.f, 60.f};
    homeCard.onPointer(right);
    right.phase = PointerPhase::Up;
    homeCard.onPointer(right);
    check(cardOpens == 1 && cardMenus == 2,
          "right-click opens the menu without also opening the PDF");

    section("library sort settles even with twenty cards and no progress bar");
    RecentCard sortedCard("Newest", "Offline copy", 0);
    sortedCard.enter(20., 19);
    check(sortedCard.animating(20.2) && !sortedCard.animating(20.36),
          "last sorted card lands in under 360 ms without phantom idle frames");
    sortedCard.progress = .6f;
    sortedCard.enter(21., 19);
    check(!sortedCard.animating(21.52),
          "reading progress finishes within half a second of library sort");

    section("reader PDF right-click and pen hold share the all-device radial menu");
    PageStage context;
    context.setFrame(0, 0, 390, 640);
    context.setSource(std::make_shared<ColumnText>(), 0);
    context.setZoom(1.f, 0);
    context.relayout(1);
    int contextTool = -1;
    context.onThumbChoice = [&](int t, int, int) { contextTool = t; };
    GesturePaint contextPaint;
    PointerEvent contextMouse{};
    contextMouse.pointerType = PointerType::Mouse;
    contextMouse.button = MouseButton::Right;
    contextMouse.phase = PointerPhase::Down;
    contextMouse.pos = contextMouse.windowPos = {190.f, 400.f};
    contextMouse.timestamp = 20.;
    context.onPointer(contextMouse);
    contextMouse.phase = PointerPhase::Up;
    contextMouse.timestamp = 20.02;
    context.onPointer(contextMouse);
    context.onPaint(contextPaint, {tokens::kLight, 20.5, 1.f});
    check(contextPaint.arcPaths >= 1 && contextPaint.icons >= 4,
          "desktop right-click pins the same non-text palette immediately");
    contextMouse.button = MouseButton::Left;
    contextMouse.phase = PointerPhase::Down;
    contextMouse.pos = contextMouse.windowPos = {124.f, 300.f};
    contextMouse.timestamp = 20.6;
    context.onPointer(contextMouse);
    contextMouse.phase = PointerPhase::Up;
    contextMouse.timestamp = 20.62;
    context.onPointer(contextMouse);
    check(contextTool == 0, "mouse can select Hand from its pinned context palette");
    PageStage penHold;
    penHold.setFrame(0, 0, 390, 640);
    penHold.setSource(std::make_shared<ColumnText>(), 0);
    penHold.setZoom(1.f, 0);
    penHold.relayout(1);
    contextMouse.pointerType = PointerType::Pen;
    contextMouse.button = MouseButton::Left;
    contextMouse.phase = PointerPhase::Down;
    contextMouse.pos = contextMouse.windowPos = {190.f, 400.f};
    contextMouse.timestamp = 22.;
    penHold.onPointer(contextMouse);
    contextPaint.arcPaths = 0;
    penHold.onPaint(contextPaint, {tokens::kLight, 22.7, 1.f});
    penHold.onPaint(contextPaint, {tokens::kLight, 22.9, 1.f});
    check(contextPaint.arcPaths >= 1,
          "stationary pen on a touchscreen gets the same half-second palette");
    contextMouse.phase = PointerPhase::Cancel;
    contextMouse.timestamp = 22.8;
    penHold.onPointer(contextMouse);

    section("hover tooltip cannot survive a hidden screen or age indefinitely");
    OverlayHost tipHost;
    tipHost.setFrame(0, 0, 390, 640);
    Widget tipScreen;
    tipScreen.setFrame(0, 0, 390, 640);
    auto* tipButton = tipScreen.emplace<Button>("Name", "", ButtonStyle::Icon);
    tipButton->setFrame(20, 20, 80, 40);
    tipHost.requestTooltip(tipButton, "Sort alphabetically", 30.);
    GesturePaint tipPaint;
    tipHost.onPaint(tipPaint, {tokens::kLight, 30.6, 1.f});
    tipHost.onPaint(tipPaint, {tokens::kLight, 30.9, 1.f});
    check(std::find(tipPaint.labels.begin(), tipPaint.labels.end(),
                    "Sort alphabetically") != tipPaint.labels.end(),
          "tooltip appears after hover delay");
    tipScreen.visible = false;
    tipPaint.labels.clear();
    tipHost.onPaint(tipPaint, {tokens::kLight, 30.7, 1.f});
    check(tipPaint.labels.empty() && !tipHost.animating(30.8),
          "hidden Home cannot leave a sort tooltip over a PDF");
    tipScreen.visible = true;
    tipHost.requestTooltip(tipButton, "Sort alphabetically", 31.);
    tipPaint.labels.clear();
    tipHost.onPaint(tipPaint, {tokens::kLight, 33.6, 1.f});
    check(tipPaint.labels.empty() && !tipHost.animating(33.6),
          "hover label expires automatically without permanent repaint");

    section("thumbnail-list scrollbar has a generous touch grab and tracks a finger");
    list->setScrollY(0);
    router.dispatchPointer(root, touch(PointerPhase::Down, 70, 379, 300, 2.12));
    router.dispatchPointer(root, touch(PointerPhase::Move, 70, 379, 140, 2.15));
    router.dispatchPointer(root, touch(PointerPhase::Up, 70, 379, 140, 2.17));
    check(list->scrollY() == 0.f,
          "clicking or dragging an empty thumbnail scrollbar rail never jumps");
    router.dispatchPointer(root, touch(PointerPhase::Down, 7, 379, 90, 2.2));
    router.dispatchPointer(root, touch(PointerPhase::Move, 7, 379, 300, 2.23));
    check(list->scrollY() > 300.f, "touch dragging the list thumb scrolls the list");
    router.dispatchPointer(root, touch(PointerPhase::Up, 7, 379, 300, 2.26));
    ScrollView elastic;
    elastic.setFrame(0, 0, 390, 350);
    elastic.contentHeight = 1000.f;
    elastic.onPointer(touch(PointerPhase::Down, 8, 100, 100, 2.5));
    elastic.onPointer(touch(PointerPhase::Move, 8, 100, 170, 2.54));
    check(elastic.scrollY() < 0.f && elastic.scrollY() > -60.f,
          "panel scroll has bounded resistance past the top edge");
    elastic.onPointer(touch(PointerPhase::Up, 8, 100, 170, 2.57));
    check(elastic.animating(2.58), "panel edge release schedules its spring return");

    section("two fingers keep separate captures, and cancel reaches each");
    auto* a = root.emplace<TapTarget>();
    auto* b = root.emplace<TapTarget>();
    a->setFrame(0, 400, 170, 120);
    b->setFrame(200, 400, 170, 120);
    router.dispatchPointer(root, touch(PointerPhase::Down, 3, 50, 450, 3));
    router.dispatchPointer(root, touch(PointerPhase::Down, 4, 250, 450, 3.01));
    router.dispatchPointer(root, touch(PointerPhase::Up, 3, 50, 450, 3.1));
    router.dispatchPointer(root, touch(PointerPhase::Up, 4, 250, 450, 3.2));
    check(a->taps == 1 && b->taps == 1, "each finger releases its own widget");
    router.dispatchPointer(root, touch(PointerPhase::Down, 5, 50, 450, 4));
    router.dispatchPointer(root, touch(PointerPhase::Down, 6, 250, 450, 4.01));
    router.dispatchPointer(root, touch(PointerPhase::Cancel, 0, 0, 0, 4.02));
    check(a->cancels == 1 && b->cancels == 1, "whole-gesture cancel reaches both");

    section("alternating finger events move the pinch midpoint only once");
    GestureTracker gesture;
    gesture.onPointer(touch(PointerPhase::Down, 11, 140, 220, 5));
    gesture.onPointer(touch(PointerPhase::Down, 12, 240, 220, 5.01));
    gesture.onPointer(touch(PointerPhase::Move, 11, 150, 230, 5.02));
    const Point d1 = gesture.pinch().centroidDelta;
    gesture.onPointer(touch(PointerPhase::Move, 12, 250, 230, 5.03));
    const Point d2 = gesture.pinch().centroidDelta;
    check(std::fabs(d1.x - 5.f) < .01f && std::fabs(d1.y - 5.f) < .01f &&
              std::fabs(d2.x - 5.f) < .01f && std::fabs(d2.y - 5.f) < .01f,
          "each finger adds half its own travel, never repeats the other finger");

    section("a pinch keeps the document point under the moving midpoint");
    PageStage stage;
    stage.setFrame(0, 0, 390, 640);
    stage.setSource(std::make_shared<PlaceholderPageSource>(3), 0);
    stage.setZoomMode(ZoomMode::FitWidth, 0);
    stage.relayout(1);
    stage.onPointer(touch(PointerPhase::Down, 21, 120, 170, 1));
    stage.onPointer(touch(PointerPhase::Down, 22, 260, 170, 1.01));
    int first = -1, second = -1;
    Point pointBefore{}, pointAfter{};
    const bool before = stage.hitPage({190, 170}, &first, &pointBefore);
    stage.onPointer(touch(PointerPhase::Move, 21, 80, 170, 1.02));
    const bool after = stage.hitPage({170, 170}, &second, &pointAfter);
    check(before && after && first == second &&
              std::fabs(pointBefore.x - pointAfter.x) < .015f &&
              std::fabs(pointBefore.y - pointAfter.y) < .015f,
          "pinch midpoint anchors the same place on the PDF");
    stage.onPointer(touch(PointerPhase::Move, 21, 120, 170, 1.03));
    check(stage.contentWidth() <= 391.f && std::fabs(stage.scrollX()) < .01f,
          "fitting the page removes any horizontal pan offset");
    stage.onPointer(touch(PointerPhase::Up, 21, 120, 170, 1.04));
    stage.onPointer(touch(PointerPhase::Up, 22, 260, 170, 1.05));

    section("minimum-zoom mixed-width PDF keeps point anchor when zooming back in");
    PageStage mixedZoom;
    mixedZoom.setFrame(0, 0, 390, 640);
    mixedZoom.setSource(std::make_shared<MixedPages>(), 0);
    mixedZoom.setZoom(0.25f, 0);
    mixedZoom.relayout(1);
    int narrowZoom = -1;
    Point beforeMixed{}, afterMixed{};
    check(mixedZoom.hitPage({195.f, 320.f}, &narrowZoom, &beforeMixed) && narrowZoom == 1,
          "zoomed-out midpoint is on the narrow second page");
    mixedZoom.onPointer(touch(PointerPhase::Down, 35, 180, 320, 1.00));
    mixedZoom.onPointer(touch(PointerPhase::Down, 36, 210, 320, 1.01));
    // Expand 30px to 120px in one frame. The first page now overflows the
    // viewport, but the anchor page remains narrower than the viewport.
    mixedZoom.onPointer(touch(PointerPhase::Move, 35, 90, 320, 1.02));
    int afterNarrow = -1;
    check(mixedZoom.contentWidth() > mixedZoom.frame.w + 1.f &&
              mixedZoom.scrollX() > 20.f &&
              mixedZoom.hitPage({150.f, 320.f}, &afterNarrow, &afterMixed) &&
              afterNarrow == narrowZoom &&
              std::fabs(beforeMixed.x - afterMixed.x) < 0.025f,
          "zoom-in tracks the moving midpoint instead of leaving the PDF left pinned");
    mixedZoom.onPointer(touch(PointerPhase::Up, 35, 90, 320, 1.03));
    mixedZoom.onPointer(touch(PointerPhase::Up, 36, 210, 320, 1.04));

    section("grid publishes full viewport and paints pages at cell resolution");
    auto gridSource = std::make_shared<GridProbe>();
    PageStage grid;
    grid.setFrame(0, 0, 780, 590);
    grid.setSource(gridSource, 0);
    grid.setGridSize(132.f);
    grid.setLayout(LayoutMode::Grid, 0);
    GesturePaint gridPaint;
    grid.onPaint(gridPaint, {tokens::kLight, 2., 2.f});
    check(gridSource->first == 0 && gridSource->last >= 9 &&
          gridSource->bufferLast > gridSource->last &&
          gridSource->paints.size() >= 10,
          "all on-screen columns publish their inclusive visible/next-row range");
    check(gridSource->tileRequests == 0 && gridSource->glyphRequests == 0 &&
          std::all_of(gridSource->paints.begin(), gridSource->paints.end(),
              [&](const GridProbe::Paint& p) {
                  const auto pt = gridSource->pageSize(p.page);
                  return p.dest.w < 135.f && p.dest.h < 170.f &&
                         std::fabs(p.scale - 2.f * p.dest.w / pt.w) < .005f;
              }),
          "grid requests tiny full-page device rasters, never reading tiles or text");
    PointerEvent gridWheel{};
    gridWheel.phase = PointerPhase::Wheel;
    gridWheel.wheelY = -3.f;
    gridWheel.pos = gridWheel.windowPos = {200.f, 260.f};
    gridWheel.timestamp = 2.1;
    check(grid.onPointer(gridWheel), "mouse wheel is handled by grid stage");
    gridSource->paints.clear();
    grid.onPaint(gridPaint, {tokens::kLight, 2.25, 2.f});
    check(grid.scrollY() > 310.f && gridSource->first > 0 &&
          gridSource->bufferFirst < gridSource->first,
          "three wheel detents move promptly and reprioritize the new visible rows");
    const float wheelY = grid.scrollY();
    gridWheel.timestamp = 2.3;
    gridWheel.wheelY = -2.f;
    grid.onPointer(gridWheel);
    grid.onPaint(gridPaint, {tokens::kLight, 2.42, 2.f});
    check(grid.scrollY() > wheelY + 100.f,
          "consecutive wheel events accumulate instead of restarting a slow ease-in");

    section("switching single, continuous, spread and grid retains the exact page");
    PageStage modes;
    modes.setFrame(0.f, 0.f, 390.f, 640.f);
    modes.setSource(std::make_shared<LongLayoutPages>(), 0.0);
    modes.setLayout(LayoutMode::Single, 0.0);
    modes.goToPage(18, 0.0); // right-hand page in TwoUp, far beyond page one
    modes.onPaint(gridPaint, {tokens::kLight, 1., 1.f});
    check(modes.currentPage() == 18, "single-page focus begins on page eighteen");
    modes.setLayout(LayoutMode::Continuous, 1.1);
    modes.onPaint(gridPaint, {tokens::kLight, 1.2, 1.f});
    int kept = -1;
    check(modes.currentPage() == 18 && modes.scrollY() > 3000.f &&
          modes.hitPage({195.f, 211.f}, &kept, nullptr) && kept == 17,
          "single-to-continuous scroll anchors page eighteen instead of page one");
    modes.setLayout(LayoutMode::TwoUp, 1.3);
    modes.onPaint(gridPaint, {tokens::kLight, 1.5, 1.f});
    kept = -1;
    check(modes.currentPage() == 18 && modes.scrollX() > 300.f &&
          modes.hitPage({195.f, 211.f}, &kept, nullptr) && kept == 17,
          "two-up centres the right-hand page rather than hiding it horizontally");
    modes.setLayout(LayoutMode::Grid, 1.6);
    modes.onPaint(gridPaint, {tokens::kLight, 1.8, 1.f});
    check(modes.currentPage() == 18 && modes.scrollY() > 500.f,
          "multi-page grid opens at the current row rather than its first row");
    modes.setLayout(LayoutMode::Continuous, 1.9);
    modes.onPaint(gridPaint, {tokens::kLight, 2., 1.f});
    kept = -1;
    check(modes.currentPage() == 18 &&
          modes.hitPage({195.f, 211.f}, &kept, nullptr) && kept == 17,
          "grid-to-reading restores the same focused page");
    modes.setLayout(LayoutMode::Single, 2.1);
    modes.onPaint(gridPaint, {tokens::kLight, 2.3, 1.f});
    check(modes.currentPage() == 18,
          "round-trip through all layouts keeps selected page and skips entrance replay");

    section("zoom completion schedules its own sharp-scale frame without a click");
    struct ZoomRasterProbe final : IPageSource {
        std::vector<float> scales;
        int pageCount() const override { return 1; }
        PageSizePt pageSize(int) const override { return {612.f, 792.f}; }
        bool cachesOwnRasters() const override { return true; }
        void drawPage(IPaintTarget&, int, const Rect&, float, PageTheme) override {}
        void drawVisiblePage(IPaintTarget&, int, const Rect&, const Rect&,
                             float scale, PageTheme) override { scales.push_back(scale); }
    };
    auto zoomProbe = std::make_shared<ZoomRasterProbe>();
    PageStage sharpZoom;
    sharpZoom.setFrame(0, 0, 390, 640);
    sharpZoom.setSource(zoomProbe, 0);
    sharpZoom.setZoom(0.5f, 0);
    GesturePaint zoomPaint;
    sharpZoom.onPaint(zoomPaint, {tokens::kLight, 1.0, 1.f});
    sharpZoom.setZoom(1.0f, 2.0);
    sharpZoom.onPaint(zoomPaint, {tokens::kLight, 2.08, 1.f});
    const float animatedRasterScale = zoomProbe->scales.back();
    // No input event at 2.16: the frame loop must call needsFrame here and
    // issue a final-scale tile request, then return to zero idle frames.
    check(sharpZoom.animating(2.16),
          "settled zoom asks for the final raster frame without pointer input");
    sharpZoom.onPaint(zoomPaint, {tokens::kLight, 2.16, 1.f});
    check(zoomProbe->scales.back() > animatedRasterScale * 1.8f &&
              !sharpZoom.animating(2.17),
          "the automatic final frame requests sharp tiles and then sleeps");

    section("parked long-document geometry restores without per-page PDF metadata locks");
    struct LayoutProbe final : IPageSource {
        mutable int sizeReads = 0;
        int pageCount() const override { return 6000; }
        PageSizePt pageSize(int) const override { ++sizeReads; return {612.f, 792.f}; }
        void drawPage(IPaintTarget&, int, const Rect&, float, PageTheme) override {}
    };
    auto layoutProbe = std::make_shared<LayoutProbe>();
    PageStage cachedLayout;
    cachedLayout.setFrame(0, 0, 390, 640);
    cachedLayout.setSource(layoutProbe, 0);
    cachedLayout.setZoom(0.5f, 0);
    cachedLayout.onPaint(zoomPaint, {tokens::kLight, 1., 1.f});
    const auto cachedView = cachedLayout.captureViewState(1.);
    auto geometry = cachedLayout.takeLayoutSnapshot(1.);
    check(geometry.placed.size() == 6000 && layoutProbe->sizeReads >= 6000,
          "first layout resolves long document geometry exactly once");
    cachedLayout.setSource(std::make_shared<PlaceholderPageSource>(2), 1.1);
    cachedLayout.setSource(layoutProbe, 2., true);
    const int beforeRestore = layoutProbe->sizeReads;
    cachedLayout.restoreViewState(cachedView, 2., &geometry);
    check(layoutProbe->sizeReads == beforeRestore && geometry.placed.empty(),
          "ready tab reuses its lightweight layout without blocking on PDF metadata");
    auto changedGeometry = cachedLayout.takeLayoutSnapshot(2.);
    cachedLayout.setFrame(0, 0, 420, 640);
    cachedLayout.setSource(layoutProbe, 3., true);
    const int beforeResize = layoutProbe->sizeReads;
    cachedLayout.restoreViewState(cachedView, 3., &changedGeometry);
    check(layoutProbe->sizeReads >= beforeResize + 6000,
          "different viewport rejects stale layout and rebuilds correct hit targets");

    section("returning tab shows a bounded actual-viewport snapshot until pixels are ready");
    auto proxySource = std::make_shared<PreviewProbe>();
    PageStage proxyStage;
    proxyStage.setFrame(0, 0, 390, 640);
    proxyStage.setSource(proxySource, 0);
    auto priorViewport = std::make_shared<ViewportPreview>();
    priorViewport->width = 8;
    priorViewport->height = 8;
    priorViewport->bgra.assign(8u * 8u * 4u, 255);
    priorViewport->imageKey = 0xE17A000000000012ull;
    proxyStage.setSwitchPreview(priorViewport);
    GesturePaint previewPaint;
    proxyStage.onPaint(previewPaint, {tokens::kLight, 1., 1.f});
    check(previewPaint.viewportImages == 1 && previewPaint.invalidatedImages == 0,
          "previously displayed viewport covers missing PDF tiles instead of white");
    proxySource->ready = true;
    proxyStage.onPaint(previewPaint, {tokens::kLight, 2., 1.f});
    check(previewPaint.viewportImages == 1 && previewPaint.invalidatedImages == 1,
          "sharp visible frame atomically replaces and frees the viewport proxy");
    proxySource->ready = false;
    proxyStage.setSwitchPreview(priorViewport);
    proxyStage.onPaint(previewPaint, {tokens::kLight, 3., 1.f});
    proxyStage.onPaint(previewPaint, {tokens::kLight, 14., 1.f});
    check(previewPaint.viewportImages == 2 && previewPaint.invalidatedImages == 2,
          "a failed render cannot leave a frozen old viewport over the reader forever");

    section("tab view-state snapshot restores exact zoom, page and XY position");
    PageStage savedStage;
    savedStage.setFrame(0, 0, 390, 640);
    savedStage.setSource(std::make_shared<MixedPages>(), 0);
    savedStage.setZoom(1.6f, 0);
    savedStage.relayout(1);
    KeyEvent arrow{};
    arrow.pressed = true;
    arrow.key = Key::Right;
    savedStage.onKey(arrow);
    arrow.key = Key::Down;
    savedStage.onKey(arrow);
    const auto view = savedStage.captureViewState(1);
    check(view.scrollX > 0.f && view.scrollY > 0.f &&
              view.zoomMode == ZoomMode::Free,
          "live document captures its non-default viewport");
    savedStage.setSource(std::make_shared<MixedPages>(), 2);
    savedStage.restoreViewState(view, 2);
    const auto restored = savedStage.captureViewState(2);
    check(std::fabs(restored.zoom - view.zoom) < 0.001f &&
              std::fabs(restored.scrollX - view.scrollX) < 0.01f &&
              std::fabs(restored.scrollY - view.scrollY) < 0.01f &&
              restored.page == view.page && restored.layout == view.layout,
          "switch restores viewport without re-zooming or jumping to page start");

    section("finger drags an existing shape instead of panning the PDF");
    PageStage movable;
    AnnotationStore placedMarks;
    movable.setFrame(0, 0, 390, 640);
    movable.setSource(std::make_shared<MixedPages>(), 0);
    movable.setStore(&placedMarks);
    movable.setZoom(1.f, 0);
    movable.relayout(1);
    Annotation box;
    box.page = 0;
    box.kind = AnnotKind::Box;
    box.box = {0.3f, 0.2f, 0.2f, 0.2f};
    const auto boxId = placedMarks.add(box);
    int moves = 0;
    movable.onAnnotationMoved = [&](const Annotation&, const Annotation&) { ++moves; };
    const float oldY = movable.scrollY();
    movable.onPointer(touch(PointerPhase::Down, 70, 196, 188, 2.0));
    movable.onPointer(touch(PointerPhase::Move, 70, 216, 208, 2.05));
    movable.onPointer(touch(PointerPhase::Up, 70, 216, 208, 2.1));
    check(moves == 1 && placedMarks.get(boxId) &&
              placedMarks.get(boxId)->box.x > 0.32f &&
              std::fabs(movable.scrollY() - oldY) < 0.01f,
          "shape moves with one finger and creates one undoable move, no page pan");
    const float xBeforeCancel = placedMarks.get(boxId)->box.x;
    movable.onPointer(touch(PointerPhase::Down, 71, 216, 208, 2.2));
    movable.onPointer(touch(PointerPhase::Move, 71, 226, 208, 2.25));
    movable.onPointer(touch(PointerPhase::Down, 72, 290, 208, 2.3));
    check(moves == 1 && placedMarks.get(boxId) &&
              std::fabs(placedMarks.get(boxId)->box.x - xBeforeCancel) < 0.001f,
          "second finger cancels uncommitted annotation drag for pinch");
    movable.onPointer(touch(PointerPhase::Up, 71, 226, 208, 2.35));
    movable.onPointer(touch(PointerPhase::Up, 72, 290, 208, 2.4));
    Annotation stroke;
    stroke.page = 0;
    stroke.kind = AnnotKind::Ink;
    stroke.points = {{0.4f, 0.45f}, {0.55f, 0.45f}};
    const auto inkId = placedMarks.add(stroke);
    movable.onPointer(touch(PointerPhase::Down, 73, 294, 388, 2.5));
    movable.onPointer(touch(PointerPhase::Move, 73, 314, 398, 2.55));
    movable.onPointer(touch(PointerPhase::Up, 73, 314, 398, 2.6));
    check(moves == 2 && placedMarks.get(inkId) &&
              placedMarks.get(inkId)->points[0].x > 0.42f &&
              std::fabs(movable.scrollY() - oldY) < 0.01f,
          "selected ink stroke also moves by touch without changing pages");

    section("large annotation sets use page buckets and view-only layer visibility");
    AnnotationStore many;
    for (int i = 0; i < 12000; ++i) {
        Annotation a;
        a.page = i % 200;
        a.kind = (i % 2) ? AnnotKind::Highlight : AnnotKind::Note;
        a.box = {.1f, .1f, .2f, .2f};
        many.add(a);
    }
    check(many.countOn(35) == 60 && many.forPage(35).size() == 60,
          "per-page index returns only the 60 marks on this page");
    for (int i = 0; i < 200; ++i) {
        many.touch(); // geometry drag must not rebuild the bucket index
        if (many.forPage(35).size() != 60) break;
    }
    check(many.forPage(35).size() == 60, "repeated drag frames retain page lookup");
    many.setKindVisible(AnnotKind::Highlight, false);
    check(many.forPage(35).empty() && many.countOn(35) == 60 &&
              many.hitTest(35, {.15f, .15f}, .01f) == 0 && many.count() == 12000,
          "hiding a Folio type affects paint/hit, not saved annotations");
    many.resetVisibility();
    auto page35 = many.forPage(35);
    check(page35.size() == 60 && many.hitTest(35, {.15f, .15f}, .01f) != 0,
          "showing a layer restores selection");
    const auto id = page35.back()->id;
    Annotation moved = *page35.back();
    moved.page = 36;
    many.replace(id, moved);
    check(many.countOn(35) == 59 && many.countOn(36) == 61,
          "moving an annotation to another page invalidates the bucket index");
    const Annotation* residentMarks = many.all().data();
    auto parkedMarks = many.takeAll();
    check(many.count() == 0 && parkedMarks.data() == residentMarks &&
              parkedMarks.size() == 12000,
          "parking a marked tab transfers annotations without O(n squared) re-insertion");
    many.restoreAll(std::move(parkedMarks));
    check(many.all().data() == residentMarks && many.countOn(36) == 61 &&
              many.count() == 12000,
          "restoring a parked tab preserves mark identities and lazy page index");

    section("rotate view keeps PDF aspect ratio and inverse touch coordinates");
    struct RotationSource final : IPageSource {
        Rect rendered{};
        int draws = 0;
        int pageCount() const override { return 1; }
        PageSizePt pageSize(int) const override { return {560.f, 800.f}; }
        bool cachesOwnRasters() const override { return true; }
        void drawPage(IPaintTarget&, int, const Rect&, float, PageTheme) override {}
        void drawVisiblePage(IPaintTarget&, int, const Rect& dest,
                             const Rect&, float, PageTheme) override {
            rendered = dest; ++draws;
        }
    };
    auto rs = std::make_shared<RotationSource>();
    PageStage rotatedPage;
    rotatedPage.setFrame(0.f, 0.f, 390.f, 640.f);
    rotatedPage.setSource(rs, 0.0);
    rotatedPage.setZoom(.35f, 0.0);
    rotatedPage.rotate(1, 1.0);
    rotatedPage.relayout(2.0);
    GesturePaint rotationPaint;
    rotatedPage.onPaint(rotationPaint, {tokens::kLight, 2.0, 1.f});
    check(rs->draws > 0 && rs->rendered.h > rs->rendered.w &&
              std::any_of(rotationPaint.rotations.begin(), rotationPaint.rotations.end(),
                          [](float a) { return std::fabs(a - 90.f) < .1f; }),
          "PDF bitmap is painted portrait and its full composition turns 90 degrees");
    Point nearTopLeft{};
    bool found = false;
    for (float y = 10.f; y < 620.f && !found; y += 4.f) {
        for (float x = 10.f; x < 380.f; x += 4.f) {
            Point n{};
            if (!rotatedPage.hitPage({x, y}, nullptr, &n)) continue;
            if (n.x < .12f && n.y > .88f) {
                nearTopLeft = n;
                found = true;
                break;
            }
        }
    }
    check(found && nearTopLeft.x < .12f && nearTopLeft.y > .88f,
          "top-left of clockwise page maps to bottom-left of source PDF");
    rotatedPage.rotate(-1, 3.0);
    rotatedPage.relayout(4.0);
    rotatedPage.onPaint(rotationPaint, {tokens::kLight, 4.0, 1.f});
    check(rotatedPage.rotationQuarterTurns() == 0 && rs->rendered.h > rs->rendered.w,
          "opposite view turn restores the original PDF orientation");

    section("a drawing finger cannot pan or jump onto the next page");
    PageStage drawing;
    AnnotationStore marks;
    drawing.setFrame(0, 0, 390, 640);
    drawing.setSource(std::make_shared<MixedPages>(), 0);
    drawing.setZoom(1.f, 0);
    drawing.setStore(&marks);
    drawing.setEditTool(PageStage::EditTool::Ink);
    drawing.relayout(1);
    check(drawing.hitPage({190.f, 80.f}, nullptr, nullptr),
          "drawing begins over page one");
    const int initialPage = drawing.currentPage();
    Point beforeStroke{};
    check(drawing.hitPage({190.f, 80.f}, nullptr, &beforeStroke),
          "stroke origin resolves on its page");
    drawing.onPointer(touch(PointerPhase::Down, 30, 190, 80, 2));
    drawing.onPointer(touch(PointerPhase::Move, 30, 195, 400, 2.03));
    drawing.onPointer(touch(PointerPhase::Move, 30, 195, 870, 2.06));
    drawing.onPointer(touch(PointerPhase::Up, 30, 195, 870, 2.09));
    Point afterStroke{};
    check(drawing.hitPage({190.f, 80.f}, nullptr, &afterStroke),
          "stroke origin remains on its page after dragging");
    check(drawing.currentPage() == initialPage &&
              std::fabs(beforeStroke.y - afterStroke.y) < 0.001f,
          "single-finger annotation never scrolls to a different page");
    check(marks.all().size() == 1 && marks.all().front().page == 0 &&
              marks.all().front().points.back().y <= 1.f,
          "stroke leaving the page remains clamped to its original page");
    drawing.onPointer(touch(PointerPhase::Down, 31, 190, 90, 3));
    drawing.onPointer(touch(PointerPhase::Cancel, 31, 190, 90, 3.05));
    check(marks.all().size() == 1 && marks.draft() == nullptr,
          "interrupted touch discards the draft without a phantom stroke");

    section("different-sized PDF pages share one centred document axis");
    PageStage mixed;
    mixed.setFrame(0, 0, 390, 640);
    mixed.setSource(std::make_shared<MixedPages>(), 0);
    mixed.setZoom(1.f, 0);
    mixed.relayout(1);
    int narrow = -1;
    Point normalised{};
    check(mixed.contentWidth() > 390.f &&
              mixed.hitPage({190.f, 870.f}, &narrow, &normalised) && narrow == 1 &&
              !mixed.hitPage({100.f, 870.f}, nullptr, nullptr),
          "narrow page is centred within the wide page's scroll axis");

    section("page thumb touch drag overrides the pan and preserves the X axis");
    const float initialX = mixed.scrollX();
    const float initialY = mixed.scrollY();
    mixed.onPointer(touch(PointerPhase::Down, 400, 375, 550, 3.8));
    mixed.onPointer(touch(PointerPhase::Move, 400, 375, 150, 3.83));
    mixed.onPointer(touch(PointerPhase::Up, 400, 375, 150, 3.85));
    check(std::fabs(mixed.scrollY() - initialY) < .01f,
          "a vertical PDF rail press never jumps, even if it is dragged afterwards");
    mixed.onPointer(touch(PointerPhase::Down, 401, 355, 625, 3.86));
    mixed.onPointer(touch(PointerPhase::Up, 401, 355, 625, 3.88));
    check(std::fabs(mixed.scrollX() - initialX) < .01f,
          "a horizontal PDF rail press also leaves the page in place");
    auto railMouse = touch(PointerPhase::Down, 0, 380, 550, 3.9);
    railMouse.pointerType = PointerType::Mouse;
    mixed.onPointer(railMouse);
    railMouse.phase = PointerPhase::Up;
    railMouse.timestamp = 3.93;
    mixed.onPointer(railMouse);
    check(std::fabs(mixed.scrollY() - initialY) < .01f,
          "desktop clicking the empty PDF track does not seek the page");
    mixed.onPointer(touch(PointerPhase::Down, 40, 375, 90, 4));
    mixed.onPointer(touch(PointerPhase::Move, 40, 375, 400, 4.03));
    check(mixed.scrollY() > 400.f && std::fabs(mixed.scrollX() - initialX) < .01f,
          "vertical scrollbar scrolls the PDF without horizontal drift");
    mixed.onPointer(touch(PointerPhase::Up, 40, 375, 400, 4.05));
    const float afterBar = mixed.scrollY();
    mixed.onPointer(touch(PointerPhase::Down, 41, 180, 300, 5));
    mixed.onPointer(touch(PointerPhase::Move, 41, 208, 250, 5.03));
    check(mixed.scrollY() > afterBar && std::fabs(mixed.scrollX() - initialX) < .01f,
          "a diagonal vertical scroll stays locked to the Y axis");
    mixed.onPointer(touch(PointerPhase::Up, 41, 208, 250, 5.05));
    const float afterVertical = mixed.scrollY();
    mixed.onPointer(touch(PointerPhase::Down, 42, 180, 300, 6));
    mixed.onPointer(touch(PointerPhase::Move, 42, 80, 295, 6.04));
    check(mixed.scrollX() > initialX + 90.f &&
          std::fabs(mixed.scrollY() - afterVertical) < .01f,
          "an intentional horizontal drag can pan a magnified PDF");
    mixed.onPointer(touch(PointerPhase::Up, 42, 80, 295, 6.07));

    section("off-centre pinch anchors the touched PDF point, not the left edge");
    PageStage offCentre;
    offCentre.setFrame(0, 0, 390, 640);
    offCentre.setSource(std::make_shared<MixedPages>(), 0);
    offCentre.setZoomMode(ZoomMode::FitWidth, 0);
    offCentre.relayout(1);
    offCentre.onPointer(touch(PointerPhase::Down, 50, 90, 240, 7));
    offCentre.onPointer(touch(PointerPhase::Down, 51, 230, 240, 7.01));
    int oldPg = -1, newPg = -1;
    Point oldNorm{}, newNorm{};
    check(offCentre.hitPage({160, 240}, &oldPg, &oldNorm),
          "pinch begins over an off-centre location on the paper");
    offCentre.onPointer(touch(PointerPhase::Move, 51, 300, 240, 7.03));
    check(offCentre.hitPage({195, 240}, &newPg, &newNorm) &&
          oldPg == newPg && std::fabs(oldNorm.x - newNorm.x) < .02f &&
          std::fabs(oldNorm.y - newNorm.y) < .02f,
          "off-centre pinch retains the same page position under moving fingers");
    offCentre.onPointer(touch(PointerPhase::Up, 50, 90, 240, 7.05));
    offCentre.onPointer(touch(PointerPhase::Up, 51, 300, 240, 7.06));

    section("page boundary rebounds instead of silently hard-stopping");
    PageStage elasticPage;
    elasticPage.setFrame(0, 0, 390, 640);
    elasticPage.setSource(std::make_shared<MixedPages>(), 0);
    elasticPage.relayout(9);
    elasticPage.onPointer(touch(PointerPhase::Down, 60, 150, 140, 10));
    elasticPage.onPointer(touch(PointerPhase::Move, 60, 152, 240, 10.04));
    elasticPage.onPointer(touch(PointerPhase::Up, 60, 152, 240, 10.06));
    check(elasticPage.scrollY() == 0.f && elasticPage.animating(10.07),
          "document edge is bounded but runs a spring after release");

    section("selection never draws a band between distant text columns");
    PageStage columns;
    columns.setFrame(0, 0, 390, 640);
    columns.setSource(std::make_shared<ColumnText>(), 0);
    columns.relayout(1);
    columns.selectAll();
    std::vector<std::pair<int, Rect>> selection;
    columns.selectionQuads(selection);
    check(selection.size() == 2 && selection[0].second.right() < .3f &&
              selection[1].second.x > .6f,
          "column gap and corrupt non-finite OCR box do not fill empty page");

    section("focus scrollbar visibility also removes the hidden touch target");
    PageStage bars;
    bars.setFrame(0, 0, 390, 640);
    bars.setSource(std::make_shared<MixedPages>(), 0);
    bars.setZoom(1.f, 0);
    bars.relayout(1);
    bars.setFocusScrollbarsVisible(false);
    check(!bars.focusScrollbarsVisible(), "focus idle hides both PDF scrollbar overlays");
    const float hiddenY = bars.scrollY();
    bars.onPointer(touch(PointerPhase::Down, 81, 379, 90, 10));
    bars.onPointer(touch(PointerPhase::Move, 81, 379, 300, 10.04));
    bars.onPointer(touch(PointerPhase::Up, 81, 379, 300, 10.07));
    check(bars.scrollY() <= hiddenY + 1.f,
          "an invisible scrollbar never catches or drags a finger");
    bars.setFocusScrollbarsVisible(true);
    bars.onPointer(touch(PointerPhase::Down, 82, 379, 90, 10.2));
    bars.onPointer(touch(PointerPhase::Move, 82, 379, 300, 10.24));
    check(bars.scrollY() > hiddenY + 200.f,
          "focus controls reveal a draggable scrollbar again");
    bars.onPointer(touch(PointerPhase::Up, 82, 379, 300, 10.27));

    section("blank hold radial palette, horizontal zoom and ordinary pan");
    PageStage thumb;
    thumb.setFrame(0, 0, 390, 640);
    thumb.setSource(std::make_shared<ColumnText>(), 0);
    thumb.setZoom(1.f, 0);
    thumb.relayout(1);
    AnnotationStore thumbMarks;
    thumb.setStore(&thumbMarks);
    thumb.setEditTool(PageStage::EditTool::Ink);
    GesturePaint paint;
    int chosenTool = -1, chosenOption = -1, chosenValue = -1, picks = 0;
    thumb.onThumbChoice = [&](int tool, int opt, int value) {
        chosenTool = tool; chosenOption = opt; chosenValue = value; ++picks;
    };
    check(thumb.hitPage({190.f, 400.f}, nullptr, nullptr),
          "palette test begins on blank paper, away from text");
    thumb.onPointer(touch(PointerPhase::Down, 91, 190, 400, 11.));
    check(thumbMarks.draft() == nullptr,
          "armed pen hold waits without creating an annotation");
    thumb.onPaint(paint, {tokens::kLight, 11.83, 1.f});
    thumb.onPointer(touch(PointerPhase::Move, 91, 216, 271, 11.9));
    paint.roundedFills.clear();
    paint.roundedStrokes.clear();
    paint.labels.clear();
    paint.icons = 0;
    thumb.onPaint(paint, {tokens::kLight, 11.93, 1.f});
    const auto menuShape = [](const Rect& r) {
        return r.w >= 30.f && r.w <= 320.f && r.h >= 30.f && r.h <= 260.f;
    };
    const auto iconFrame = [](const Rect& r) {
        return r.w >= 38.f && r.w <= 48.f && r.h >= 38.f && r.h <= 48.f;
    };
    check(std::any_of(paint.roundedFills.begin(), paint.roundedFills.end(), menuShape) &&
              std::none_of(paint.roundedStrokes.begin(), paint.roundedStrokes.end(), iconFrame) &&
              paint.arcPaths >= 1 && paint.icons >= 4 && paint.stack.empty(),
          "compact arc and selection-style card have no individual icon frames");
    check(std::find(paint.labels.begin(), paint.labels.end(), "COLOUR") == paint.labels.end() &&
              std::find(paint.labels.begin(), paint.labels.end(), "SIZE") == paint.labels.end() &&
              std::find(paint.labels.begin(), paint.labels.end(), "OPACITY") == paint.labels.end() &&
              std::find(paint.labels.begin(), paint.labels.end(), "SHAPES") == paint.labels.end() &&
              std::any_of(paint.roundedFills.begin(), paint.roundedFills.end(),
                  [](const Rect& r) { return r.w == 208.f && r.h == 58.f &&
                                              r.y >= 338.f; }),
          "compact icon-only settings card sits below the tool arc without overlap");
    thumb.onPointer(touch(PointerPhase::Up, 91, 216, 271, 11.95));
    check(picks == 1 && chosenTool == 2 && chosenOption == -1 &&
              thumbMarks.all().empty(),
          "drag to enlarged Pen arc selects a persistent tool, never draws");

    thumb.onPointer(touch(PointerPhase::Down, 92, 190, 400, 12.));
    thumb.onPaint(paint, {tokens::kLight, 12.83, 1.f});
    thumb.onPointer(touch(PointerPhase::Move, 92, 216, 271, 12.9));
    thumb.onPointer(touch(PointerPhase::Move, 92, 121, 366, 12.95));
    paint.circles = 0;
    thumb.onPaint(paint, {tokens::kLight, 12.98, 1.f});
    check(paint.circles >= 24,
          "hovering Colour instantly reveals all 24 free-floating swatches");
    thumb.onPointer(touch(PointerPhase::Move, 92, 80, 300, 13.));
    thumb.onPointer(touch(PointerPhase::Up, 92, 80, 300, 13.03));
    check(picks == 2 && chosenTool == 2 && chosenOption == 0 &&
              chosenValue == 0 && thumbMarks.all().empty(),
          "second layer reaches palette swatch without crossing a document tool");
    // The same three-step sweep can reach the last thickness, opacity and
    // shape presets without hitting any ordinary toolbar or the zoom lane.
    for (int option = 1; option <= 3; ++option) {
        const auto id = static_cast<PointerId>(100 + option);
        const double time = 13.1 + static_cast<double>(option) * .4;
        thumb.onPointer(touch(PointerPhase::Down, id, 190, 400, time));
        thumb.onPaint(paint, {tokens::kLight, time + .83, 1.f});
        thumb.onPointer(touch(PointerPhase::Move, id, 216, 271, time + .9));
        const float optionX = 190.f + (static_cast<float>(option) - 1.5f) * 50.f;
        thumb.onPointer(touch(PointerPhase::Move, id, optionX, 366, time + .95));
        const int last = option == 1 ? 5 : option == 2 ? 3 : 4;
        const int count = option == 1 ? 6 : option == 2 ? 4 : 5;
        const float valueX = 190.f +
            (static_cast<float>(last) - static_cast<float>(count - 1) * .5f) * 46.f;
        thumb.onPointer(touch(PointerPhase::Move, id, valueX, 304, time + 1.));
        thumb.onPointer(touch(PointerPhase::Up, id, valueX, 304, time + 1.03));
        check(chosenTool == 2 && chosenOption == option && chosenValue == last,
              "all pen option layers reach their final preset by one thumb sweep");
    }
    check(picks == 5, "colour, thickness, opacity and shape choices each fire once");

    PageStage flight;
    flight.setFrame(0, 0, 390, 640);
    flight.setSource(std::make_shared<ColumnText>(), 0);
    flight.setZoom(1.f, 0);
    flight.relayout(1);
    flight.setEditTool(PageStage::EditTool::Ink);
    GesturePaint flightPaint;
    flight.onPointer(touch(PointerPhase::Down, 140, 190, 400, 40.));
    flight.onPaint(flightPaint, {tokens::kLight, 40.48, 1.f});
    check(flightPaint.icons == 0,
          "known-blank hold does not open before 500 ms");
    flight.onPaint(flightPaint, {tokens::kLight, 40.51, 1.f});
    flightPaint.iconCentres.clear();
    flight.onPaint(flightPaint, {tokens::kLight, 40.56, 1.f});
    const auto revealRadius = [](const GesturePaint& p) {
        float r = 0.f;
        for (const Rect& clip : p.revealClips) r = std::max(r, clip.w * .5f);
        return r;
    };
    const float early = revealRadius(flightPaint);
    const int earlyLayers = flightPaint.layers;
    flightPaint.revealClips.clear();
    flightPaint.iconCentres.clear();
    flightPaint.layers = 0;
    flight.onPaint(flightPaint, {tokens::kLight, 41.0, 1.f});
    const float settled = revealRadius(flightPaint);
    check(early > 0.f && early < settled * .85f && settled >= 209.f &&
              earlyLayers == 1 && flightPaint.layers == 1 &&
              flightPaint.iconCentres.size() == 4 && flightPaint.arcPaths >= 1,
          "native popover clip grows from anchor with just one layer for four tools");
    flight.onPointer(touch(PointerPhase::Up, 140, 190, 400, 41.01));
    flightPaint.iconCentres.clear();
    flight.onPaint(flightPaint, {tokens::kLight, 41.3, 1.f});
    check(flightPaint.iconCentres.size() == 4 && !flight.animating(41.3) &&
              flightPaint.stack.empty(),
          "unselected first tier remains pinned after lift without idle redraw");
    int pinnedTool = -1;
    flight.onThumbChoice = [&](int tool, int, int) { pinnedTool = tool; };
    flight.onPointer(touch(PointerPhase::Down, 141, 212, 288, 41.31));
    flight.onPointer(touch(PointerPhase::Up, 141, 212, 288, 41.34));
    check(pinnedTool == 2,
          "a separate tap can select Pen from the pinned first tier");
    flight.setZoom(1.3f, 41.4);
    flightPaint.iconCentres.clear();
    flight.onPaint(flightPaint, {tokens::kLight, 41.7, 1.f});
    check(flightPaint.iconCentres.empty() && flightPaint.stack.empty(),
          "using a zoom control dismisses the pinned first tier");
    flight.onPointer(touch(PointerPhase::Down, 142, 190, 400, 42.));
    flight.onPaint(flightPaint, {tokens::kLight, 42.52, 1.f});
    flight.onPointer(touch(PointerPhase::Up, 142, 190, 400, 42.54));
    flight.dismissThumbIfOutside({500.f, 20.f}, 42.6);
    flightPaint.revealClips.clear();
    flight.onPaint(flightPaint, {tokens::kLight, 42.68, 1.f});
    check(revealRadius(flightPaint) > 0.f &&
              revealRadius(flightPaint) < settled && flight.animating(42.68),
          "outside dismissal reverses the popup reveal instead of disappearing at Down");
    flightPaint.iconCentres.clear();
    flight.onPaint(flightPaint, {tokens::kLight, 42.9, 1.f});
    check(flightPaint.iconCentres.empty(),
          "outside command-bar/side-panel touch can dismiss a pinned palette");

    PageStage complete;
    complete.setFrame(0, 0, 390, 640);
    complete.setSource(std::make_shared<ColumnText>(), 0);
    complete.setZoom(1.f, 0);
    complete.relayout(1);
    complete.setEditTool(PageStage::EditTool::Ink);
    GesturePaint completePaint;
    int completeTool = -1, completeOption = -1, completeValue = -1, completed = 0;
    complete.onThumbChoice = [&](int tool, int option, int value) {
        completeTool = tool; completeOption = option; completeValue = value;
        ++completed;
    };
    complete.onPointer(touch(PointerPhase::Down, 160, 190, 400, 50.));
    complete.onPaint(completePaint, {tokens::kLight, 50.53, 1.f});
    complete.onPointer(touch(PointerPhase::Move, 160, 124, 300, 50.6));
    complete.onPointer(touch(PointerPhase::Up, 160, 124, 300, 50.65));
    completePaint.revealClips.clear();
    complete.onPaint(completePaint, {tokens::kLight, 50.72, 1.f});
    check(completed == 1 && completeTool == 0 &&
              revealRadius(completePaint) > 0.f &&
              revealRadius(completePaint) < 210.f,
          "releasing Hand chooses Pan and plays the closing reveal (no property tier)");
    completePaint.iconCentres.clear();
    complete.onPaint(completePaint, {tokens::kLight, 50.9, 1.f});
    check(completePaint.iconCentres.empty(),
          "Hand menu is fully gone after its closing animation");

    complete.onPointer(touch(PointerPhase::Down, 161, 190, 400, 51.));
    complete.onPaint(completePaint, {tokens::kLight, 51.53, 1.f});
    complete.onPointer(touch(PointerPhase::Move, 161, 212, 288, 51.6));
    complete.onPointer(touch(PointerPhase::Move, 161, 121, 366, 51.7));
    complete.onPointer(touch(PointerPhase::Move, 161, 80, 303, 51.8));
    complete.onPointer(touch(PointerPhase::Up, 161, 80, 303, 51.85));
    completePaint.revealClips.clear();
    complete.onPaint(completePaint, {tokens::kLight, 51.92, 1.f});
    check(completed == 2 && completeTool == 2 && completeOption == 0 &&
              completeValue == 0 && revealRadius(completePaint) > 0.f &&
              complete.animating(51.92),
          "one uninterrupted three-tier choice commits once then animates closed");

    // The second layer must also be navigable with successive taps, not
    // depend on keeping one finger on the glass through all three rows.
    PageStage steps;
    AnnotationStore stepMarks;
    steps.setFrame(0, 0, 390, 640);
    steps.setSource(std::make_shared<ColumnText>(), 0);
    steps.setZoom(1.f, 0);
    steps.relayout(1);
    steps.setStore(&stepMarks);
    steps.setEditTool(PageStage::EditTool::Ink);
    int stepPicks = 0, stepOption = -1, stepValue = -1;
    steps.onThumbChoice = [&](int tool, int option, int value) {
        check(tool == 2, "staged menu preserves the selected Pen");
        ++stepPicks; stepOption = option; stepValue = value;
    };
    steps.onPointer(touch(PointerPhase::Down, 130, 190, 400, 30.));
    steps.onPaint(paint, {tokens::kLight, 30.83, 1.f});
    steps.onPointer(touch(PointerPhase::Move, 130, 216, 271, 30.9));
    steps.onPointer(touch(PointerPhase::Up, 130, 216, 271, 30.95));
    paint.labels.clear();
    paint.iconCentres.clear();
    steps.onPaint(paint, {tokens::kLight, 31.2, 1.f});
    check(stepPicks == 1 && stepOption == -1 &&
              std::find(paint.labels.begin(), paint.labels.end(), "COLOUR") == paint.labels.end() &&
              paint.iconCentres.size() >= 7,
          "lifting first finger commits Pen while tool arc and icon-only settings persist");
    float maxRemaining = 0.f;
    for (const auto& c : paint.iconCentres)
        maxRemaining = std::max(maxRemaining, std::hypot(c.x - 190.f, c.y - 400.f));
    check(maxRemaining > 110.f && maxRemaining < 170.f,
          "first-tier tool glyphs stay in place alongside the separate property row");
    steps.onPointer(touch(PointerPhase::Down, 131, 121, 366, 31.24));
    steps.onPointer(touch(PointerPhase::Up, 131, 121, 366, 31.27));
    paint.circles = 0;
    steps.onPaint(paint, {tokens::kLight, 31.34, 1.f});
    check(stepPicks == 1 && paint.circles >= 24,
          "tapping Colour leaves all swatches ready for a separate value tap");
    check(!steps.animating(31.7),
          "a visible, persistent second layer causes no redraw after its reveal");
    steps.onPointer(touch(PointerPhase::Down, 132, 80, 303, 31.8));
    steps.onPointer(touch(PointerPhase::Up, 132, 80, 303, 31.83));
    check(stepPicks == 2 && stepOption == 0 && stepValue == 0 &&
              stepMarks.all().empty(),
          "three separate touches pick Pen, Colour and the first swatch without ink");
    paint.circles = 0;
    steps.onPaint(paint, {tokens::kLight, 32.0, 1.f});
    check(paint.circles >= 24 && !steps.animating(32.0),
          "third-tier colours persist without repainting after a swatch is chosen");
    steps.onPointer(touch(PointerPhase::Down, 133, 167, 366, 32.1));
    steps.onPointer(touch(PointerPhase::Up, 133, 167, 366, 32.13));
    steps.onPointer(touch(PointerPhase::Down, 134, 305, 304, 32.8));
    steps.onPointer(touch(PointerPhase::Up, 134, 305, 304, 32.83));
    check(stepPicks == 3 && stepOption == 1 && stepValue == 5,
          "without another hold the same menu switches from Colour to Size");
    steps.onPointer(touch(PointerPhase::Down, 135, 190, 400, 33.));
    paint.revealClips.clear();
    steps.onPaint(paint, {tokens::kLight, 33.08, 1.f});
    check(revealRadius(paint) > 0.f,
          "outside Down keeps the exit animation visible while normal page input begins");
    steps.onPointer(touch(PointerPhase::Move, 135, 190, 350, 33.09));
    steps.onPointer(touch(PointerPhase::Up, 135, 190, 350, 33.12));
    check(stepMarks.all().size() == 1 && stepPicks == 3,
          "outside touch closes all pinned tiers and immediately draws normally");

    const float zoom0 = thumb.zoom();
    int anchorBeforePage = -1, anchorAfterPage = -1;
    Point anchorBefore{}, anchorAfter{};
    check(thumb.hitPage({190.f, 400.f}, &anchorBeforePage, &anchorBefore),
          "radial zoom point is still on PDF paper");
    thumb.onPointer(touch(PointerPhase::Down, 93, 190, 400, 14.));
    thumb.onPaint(paint, {tokens::kLight, 14.83, 1.f});
    thumb.onPointer(touch(PointerPhase::Move, 93, 260, 401, 14.9));
    check(thumb.zoom() > zoom0 * 1.3f && picks == 5 &&
              thumb.hitPage({190.f, 400.f}, &anchorAfterPage, &anchorAfter) &&
              anchorAfterPage == anchorBeforePage &&
              std::fabs(anchorAfter.x - anchorBefore.x) < .025f &&
              std::fabs(anchorAfter.y - anchorBefore.y) < .025f,
          "post-hold horizontal right swipe smoothly zooms at touched page");
    thumb.onPointer(touch(PointerPhase::Up, 93, 260, 401, 14.93));
    const float zoom1 = thumb.zoom();
    thumb.onPointer(touch(PointerPhase::Down, 94, 190, 400, 15.));
    thumb.onPaint(paint, {tokens::kLight, 15.83, 1.f});
    thumb.onPointer(touch(PointerPhase::Move, 94, 120, 400, 15.9));
    check(thumb.zoom() < zoom1 && picks == 5,
          "post-hold left swipe zooms out without selecting a tool");
    thumb.onPointer(touch(PointerPhase::Up, 94, 120, 400, 15.93));
    thumb.onPointer(touch(PointerPhase::Down, 95, 190, 400, 16.));
    thumb.onPointer(touch(PointerPhase::Move, 95, 190, 350, 16.04));
    thumb.onPointer(touch(PointerPhase::Up, 95, 190, 350, 16.09));
    check(thumbMarks.all().size() == 1 && picks == 5,
          "movement before hold timeout replays normal pen drawing once");
    Point word{};
    bool foundWord = false;
    for (int y = 50; y < 520 && !foundWord; y += 3) {
        for (int x = 15; x < 375 && !foundWord; x += 3) {
            Point n{};
            if (thumb.hitPage({static_cast<float>(x), static_cast<float>(y)},
                              nullptr, &n) && n.x > .105f && n.x < .13f &&
                n.y > .105f && n.y < .125f) {
                word = {static_cast<float>(x), static_cast<float>(y)};
                foundWord = true;
            }
        }
    }
    check(foundWord, "known text under an armed pen has a touch position");
    if (foundWord) {
        const auto beforeMarks = thumbMarks.all().size();
        thumb.onPointer(touch(PointerPhase::Down, 96, word.x, word.y, 17.));
        thumb.onPaint(paint, {tokens::kLight, 17.55, 1.f});
        check(thumbMarks.all().size() == beforeMarks &&
                  !thumb.selectedText().empty(),
              "text long hold selects instead of drawing with Pen armed");
        thumb.onPointer(touch(PointerPhase::Up, 96, word.x, word.y, 17.6));
    }

    section("highlight preview survives touch-down; radial hold cancels only its empty draft");
    PageStage markerMenu;
    AnnotationStore markerMarks;
    markerMenu.setFrame(0, 0, 390, 640);
    markerMenu.setSource(std::make_shared<ColumnText>(), 0);
    markerMenu.setZoom(1.f, 0);
    markerMenu.relayout(1);
    markerMenu.setStore(&markerMarks);
    markerMenu.setEditTool(PageStage::EditTool::Highlight);
    markerMenu.onThumbChoice = [&](int tool, int, int) { chosenTool = tool; ++picks; };
    markerMenu.onPointer(touch(PointerPhase::Down, 97, 190, 400, 18.));
    check(markerMarks.draft() && markerMarks.draft()->empty(),
          "highlight still shows its immediate uncommitted touch-down preview");
    markerMenu.onPaint(paint, {tokens::kLight, 18.83, 1.f});
    check(!markerMarks.draft() && markerMarks.all().empty(),
          "still hold opens floating tools without committing an accidental highlight");
    markerMenu.onPointer(touch(PointerPhase::Move, 97, 268, 296, 18.9));
    markerMenu.onPointer(touch(PointerPhase::Up, 97, 268, 296, 18.95));
    check(chosenTool == 3 && markerMarks.all().empty(),
          "sweeping toward eraser chooses a tool without creating a mark");
    markerMenu.setAnnotStyle({250, 204, 21, 255}, .6f, 1.f);
    markerMenu.onPointer(touch(PointerPhase::Down, 98, 190, 400, 19.));
    markerMenu.onPointer(touch(PointerPhase::Move, 98, 240, 400, 19.04));
    const float thin = markerMarks.draft() ? markerMarks.draft()->box.h : 0.f;
    markerMenu.onPointer(touch(PointerPhase::Cancel, 98, 240, 400, 19.05));
    markerMenu.setAnnotStyle({250, 204, 21, 255}, 10.f, 1.f);
    markerMenu.onPointer(touch(PointerPhase::Down, 99, 190, 400, 19.2));
    markerMenu.onPointer(touch(PointerPhase::Move, 99, 240, 400, 19.24));
    const float thick = markerMarks.draft() ? markerMarks.draft()->box.h : 0.f;
    check(thin > 0.f && thick > thin * 2.f && markerMarks.all().empty(),
          "radial thickness settings change actual marker band and its preview");
    markerMenu.onPointer(touch(PointerPhase::Cancel, 99, 240, 400, 19.25));

    section("desktop tab strip clips overflow and scrolls to the selected PDF");
    TabBar docTabs;
    docTabs.setFrame(0.f, 0.f, 220.f, 44.f);
    int chosen = -1;
    docTabs.onSelect = [&](int i) { chosen = i; };
    for (int i = 0; i < 8; ++i)
        docTabs.addTab("document " + std::to_string(i), 1.0, true);
    check(docTabs.clipsContent && docTabs.intrinsicWidth(2.0) > docTabs.frame.w,
          "overflowing PDF tabs stay clipped inside their title-bar strip");
    docTabs.selectTab(0, 2.0);
    PointerEvent wheel{};
    wheel.phase = PointerPhase::Wheel;
    wheel.pos = {100.f, 20.f};
    wheel.wheelY = -5.f;
    wheel.timestamp = 2.0;
    check(docTabs.onPointer(wheel), "wheel pans the overflowing strip");
    PointerEvent pick{};
    pick.phase = PointerPhase::Up;
    pick.pos = {50.f, 20.f};
    pick.timestamp = 2.0;
    docTabs.onPointer(pick);
    check(chosen > 0 && chosen < 8,
          "tab hit targets follow horizontal scrolling instead of the old off-screen positions");
    docTabs.selectTab(7, 2.1);
    check(docTabs.active() == 7, "dropdown selection activates a far tab");

    section("open/save modal controls stay on a phone viewport");
    OpenDialog dialog;
    dialog.open(0);
    for (const auto& [w, h] : {std::pair{360.f, 640.f}, std::pair{640.f, 360.f}}) {
        dialog.setFrame(0, 0, w, h);
        dialog.layoutTree();
        int actions = 0;
        dialog.forEach([&](Widget& c) {
            if (c.debugName != "Button(Open)" && c.debugName != "Button(Cancel)") return;
            ++actions;
            const Point top = c.toWindow({0, 0});
            check(top.x >= 0 && top.x + c.frame.w <= w &&
                      top.y >= 0 && top.y + c.frame.h <= h,
                  c.debugName + " is wholly on screen");
        });
        check(actions == 2, "both actions were found and checked");
    }
}
