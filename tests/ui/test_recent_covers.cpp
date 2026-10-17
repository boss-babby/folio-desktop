#include "screens/RecentCovers.h"
#include "screens/FolioApp.h"
#include "components/Containers.h"
#include "../Harness.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <thread>

using namespace folio::ui;
using namespace folio::test;
namespace fs = std::filesystem;

namespace {
std::shared_ptr<const RecentCover> waitCover(RecentCovers& covers,
                                              const std::string& path) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < until) {
        if (covers.hasResults()) {
            covers.drain();
            if (!covers.busy()) return covers.get(path);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return {};
}

class CoverSpy : public folio::render::IPaintTarget {
public:
    using namespace_dummy = int;
    folio::render::Rect image{}, clip{};
    int images = 0;
    void save() override {}
    void restore() override {}
    void translate(float, float) override {}
    void scale(float, float) override {}
    void rotateDegrees(float) override {}
    void clipRect(const folio::render::Rect&, bool) override {}
    void clipRoundedRect(const folio::render::Rect& r,
                         const folio::render::Corners&, bool) override { clip = r; }
    void saveLayerAlpha(float) override {}
    void clear(const folio::render::Color&) override {}
    void fillRect(const folio::render::Rect&, const folio::render::Color&) override {}
    void fillRoundedRect(const folio::render::Rect&,
                         const folio::render::Corners&,
                         const folio::render::Color&) override {}
    void strokeRoundedRect(const folio::render::Rect&,
                           const folio::render::Corners&,
                           const folio::render::Color&, float) override {}
    void drawShadow(const folio::render::Rect&,
                    const folio::render::Corners&,
                    const folio::render::Shadow&) override {}
    void fillCircle(const folio::render::Point&, float,
                    const folio::render::Color&) override {}
    void drawLine(const folio::render::Point&, const folio::render::Point&,
                  const folio::render::Color&, float) override {}
    void fillRectVerticalGradient(const folio::render::Rect&,
                                  const folio::render::Color&,
                                  const folio::render::Color&) override {}
    void setBlendMode(folio::render::BlendMode) override {}
    void strokePath(std::span<const folio::render::PathVerb>,
                    const folio::render::Color&, float,
                    folio::render::LineCap, folio::render::LineJoin) override {}
    void fillPath(std::span<const folio::render::PathVerb>,
                  const folio::render::Color&) override {}
    void drawText(std::string_view, const folio::render::Point&,
                  const folio::render::TextStyle&,
                  folio::render::TextBaseline) override {}
    void drawTextInBox(std::string_view, const folio::render::Rect&,
                       const folio::render::TextStyle&,
                       folio::render::TextAlign) override {}
    float measureText(std::string_view, const folio::render::TextStyle&) override {
        return 0.f;
    }
    folio::render::FontMetrics fontMetrics(
        const folio::render::TextStyle&) override { return {}; }
    bool drawCached(std::uint64_t, const folio::render::Rect&, int, int,
                    const std::function<void(IPaintTarget&)>&) override { return false; }
    void drawImageBGRA(const void*, int, int, std::size_t,
                       const folio::render::Rect& dst, std::uint64_t,
                       ImageFit, ImageTint) override { image = dst; ++images; }
    void invalidateRasterCache() override {}
    RasterCacheStats rasterCacheStats() const override { return {}; }
    int width() const override { return 400; }
    int height() const override { return 800; }
    std::string_view backendName() const override { return "cover spy"; }
};
} // namespace

FOLIO_TEST_MAIN("real recent covers, top crop and bounded disk cache") {
    section("top crop geometry preserves first-page aspect ratio");
    const folio::render::Rect crop{10.f, 10.f, 180.f, 100.f};
    const auto portrait = recentCoverDestination(crop, 200, 300);
    check(portrait.x == crop.x && portrait.y == crop.y &&
              portrait.w == crop.w && portrait.h > crop.h,
          "portrait first page fills the card and crops below, not at centre");
    const auto landscape = recentCoverDestination(crop, 320, 100);
    check(landscape.y == crop.y && landscape.h == crop.h &&
              landscape.w > crop.w && landscape.x < crop.x,
          "wide first page crops evenly on the sides without distortion");

    const fs::path tmp = fs::temp_directory_path() / "folio-cover-cache-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const fs::path original = "folio-test.pdf";
    const fs::path source = tmp / "a real first page.pdf";
    fs::copy_file(original, source, fs::copy_options::overwrite_existing, ec);
    check(!ec, "fixture copied into a real recent PDF path");
    if (ec) return;
    const std::string file = source.string();
    const std::string state = (tmp / "recent.txt").string();
    std::atomic<int> wakes{0};
    std::shared_ptr<const RecentCover> first;
    {
        RecentCovers loader;
        loader.setStateFile(state);
        loader.setWake([&] { ++wakes; });
        loader.request(file);
        first = waitCover(loader, file);
        check(first && first->width > 0 && first->height > 0 &&
                  first->width <= 256 && first->height <= 384 &&
                  first->pixels.size() ==
                     static_cast<std::size_t>(first->width * first->height * 4),
              "real PDFium first page renders off-thread at bounded dimensions");
        check(wakes == 1, "one completion wakes the sleeping UI exactly once");
        loader.keepMemoryFor({});
        check(!loader.get(file), "three-second idle pass releases offscreen cover RAM");
        loader.request(file);
        auto reloaded = waitCover(loader, file);
        check(reloaded && reloaded->pixels == first->pixels,
              "scrolling back retrieves the persistent cover without a fake page");
        loader.releaseMemory();
        check(!loader.get(file) && !loader.hasResults(),
              "minimise drops all home-cover pixels but preserves disk cache");
        if (first) {
            RecentCard card("Document", "PDF", 0);
            card.setFrame(0, 0, 206, 196);
            card.cover = first;
            CoverSpy spy;
            card.onPaint(spy, {tokens::kLight, 1.0, 1.f});
            check(spy.images == 1 && spy.clip.h > 0.f &&
                      std::abs(spy.image.y - spy.clip.y) < .01f &&
                      spy.image.h + .01f >= spy.clip.h &&
                      spy.image.w + .01f >= spy.clip.w,
                  "home card paints its real cover with a top-aligned clip");
        }
    }
    if (!first) return;
    fs::path cacheFile;
    for (const auto& entry : fs::directory_iterator(tmp / "covers"))
        if (entry.path().extension() == ".bgra") cacheFile = entry.path();
    check(!cacheFile.empty() && fs::file_size(cacheFile) < 400u * 1024u,
          "private persistent cover uses less than 400 KiB per PDF");
    const auto oldWrite = fs::last_write_time(cacheFile);
    {
        AppState saved;
        saved.loadRecents(state);
        saved.rememberDocument(file, 3, 1);
        FolioApp library(390.f, 800.f);
        library.loadRecents(state);
        CoverSpy painted;
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::seconds(5);
        double t = 1.0;
        while (painted.images == 0 && std::chrono::steady_clock::now() < until) {
            library.paint(painted, t);
            t += .05;
            if (painted.images == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(painted.images > 0,
              "actual home screen shows a persisted first-page cover, not fake lines");
    }
    {
        RecentCovers warmed;
        warmed.setStateFile(state);
        warmed.request(file);
        auto again = waitCover(warmed, file);
        check(again && again->pixels == first->pixels &&
                  again->imageKey == first->imageKey &&
                  fs::last_write_time(cacheFile) == oldWrite,
              "reopening uses the unchanged on-disk preview, not a new render");
    }
    { // A file edit invalidates the cover even when the path stays the same.
        std::ofstream append(source, std::ios::binary | std::ios::app);
        append << "\n% revision\n";
    }
    fs::last_write_time(source, fs::last_write_time(source) +
                                 std::chrono::seconds(3), ec);
    {
        RecentCovers changed;
        changed.setStateFile(state);
        changed.request(file);
        auto updated = waitCover(changed, file);
        check(updated && updated->imageKey != first->imageKey,
              "size/mtime change makes a new real-PDF cover identity");
    }
    fs::remove_all(tmp, ec);
}
