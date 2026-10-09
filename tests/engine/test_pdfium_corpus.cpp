// Corpus smoke test: every PDF in documents/ must open, report a sane page
// count, and render a first page that is not blank.
//
// The three Folio sample files are synthetic - written by tools/make-sample-pdf.py -
// so PDFium accepting them proves less than it looks like. The rest of the
// corpus is real material pulled off the web, chosen for the features that
// actually break renderers:
//
//   Attention_Is_All_You_Need.pdf   arXiv LaTeX: Type1 subsets, vector figures
//   lshort.pdf                      LaTeX book: outline, links, 150+ pages
//   Japanese_Government_Notice.pdf  CID/CJK fonts, vertical metrics
//
// "Not blank" is the interesting assertion. A renderer that silently fails to
// find a font still returns a successful, perfectly white bitmap, so page
// count alone would pass while the user sees nothing.

#include "pdfium/PdfiumLibrary.h"
#include "pdfium/PdfiumRasterizer.h"

#include "folio/core/engine/DocumentSnapshot.h"
#include "../Harness.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace folio::core;
using namespace folio::test;
using folio::engine::PdfiumRasterizer;

namespace {

std::vector<std::byte> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const auto len = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> out(len);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
    return out;
}

/// Fraction of pixels that differ from the top-left pixel. A blank page -
/// whatever its background colour - scores 0.
double inkCoverage(const RasterTile& tile) {
    if (tile.width <= 0 || tile.height <= 0) return 0.0;
    const auto* base = reinterpret_cast<const unsigned char*>(tile.pixels.data());
    unsigned char bg[4];
    std::memcpy(bg, base, 4);
    std::size_t differing = 0, total = 0;
    for (int y = 0; y < tile.height; ++y) {
        const unsigned char* row = base + static_cast<std::size_t>(y) * tile.stride;
        for (int x = 0; x < tile.width; ++x) {
            const unsigned char* px = row + static_cast<std::size_t>(x) * 4;
            const int d = std::abs(px[0] - bg[0]) + std::abs(px[1] - bg[1]) +
                          std::abs(px[2] - bg[2]);
            if (d > 24) ++differing;
            ++total;
        }
    }
    return total ? static_cast<double>(differing) / static_cast<double>(total) : 0.0;
}

}  // namespace

FOLIO_TEST_MAIN("pdfium corpus") {
    namespace fs = std::filesystem;
    folio::engine::ensurePdfiumInitialised();

    // The test runs from tests/data/; the corpus lives beside the sources.
    fs::path dir;
    for (const char* c : {"../../documents", "../documents", "documents"}) {
        std::error_code ec;
        if (fs::is_directory(c, ec)) { dir = c; break; }
    }
    check(!dir.empty(), "documents/ directory found");
    if (dir.empty()) return;

    std::vector<fs::path> corpus;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".pdf") corpus.push_back(e.path());
    std::sort(corpus.begin(), corpus.end());

    check(corpus.size() >= 3, "corpus has at least three documents");

    for (const auto& path : corpus) {
        section(path.filename().string());

        auto bytes = readFile(path);
        check(!bytes.empty(), "  file reads");
        if (bytes.empty()) continue;

        auto snapshot = DocumentSnapshot::Builder::fromBase(std::move(bytes));
        PdfiumRasterizer rast;
        rast.bindToCurrentThread();

        const bool loaded = rast.setSnapshot(snapshot);
        check(loaded, "  PDFium accepts the file");
        if (!loaded) continue;

        const int pages = rast.pageCount();
        check(pages > 0, "  page count is positive");
        note("pages: " + std::to_string(pages));

        const auto info = rast.pageInfo(PageId{0});
        check(info.widthPt > 1.0 && info.heightPt > 1.0, "  page 0 has real dimensions");

        auto tile = rast.renderFullPage(PageId{0}, 1.0f, 0, ColorScheme::Light);
        check(tile != nullptr, "  page 0 renders");
        if (!tile) continue;

        const double ink = inkCoverage(*tile);
        check(ink > 0.002, "  page 0 is not blank");
        char buf[96];
        std::snprintf(buf, sizeof buf, "ink coverage: %.3f%%  (%dx%d)", ink * 100.0,
                      tile->width, tile->height);
        note(buf);

        // Last page too: single-page assumptions in a page tree walk tend to
        // show up at the end, not the beginning.
        if (pages > 1) {
            auto last = rast.renderFullPage(PageId{pages - 1}, 1.0f, 0,
                                            ColorScheme::Light);
            check(last != nullptr, "  last page renders");
            if (last) check(inkCoverage(*last) > 0.0005, "  last page is not blank");
        }
    }

    void folio_test_text_layer();
    folio_test_text_layer();
    void folio_test_outline();
    folio_test_outline();
}

// --- text layer -------------------------------------------------------------
//
// Appended as a second body so the corpus loop above stays readable. This
// exercises PdfiumWorker end to end - the thread, the job queue, and the
// glyph normalisation - rather than calling FPDFText directly, because the
// threading is the part most likely to break.

#include "pdfium/PdfiumWorker.h"

#include <chrono>
#include <thread>

namespace {

/// Spin the worker until it hands back a result for `page`, or give up.
std::shared_ptr<const folio::engine::PageText> waitForText(
    folio::engine::PdfiumWorker& w, int page) {
    w.requestText(page);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& r : w.drain())
            if (r.kind == folio::engine::PdfiumWorker::Kind::Text && r.page == page)
                return r.text;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return nullptr;
}

}  // namespace

void folio_test_text_layer() {
    namespace fs = std::filesystem;
    section("text layer (via PdfiumWorker)");

    fs::path dir;
    for (const char* c : {"../../documents", "../documents", "documents"}) {
        std::error_code ec;
        if (fs::is_directory(c, ec)) { dir = c; break; }
    }
    if (dir.empty()) { check(false, "documents/ found"); return; }

    struct Case { const char* file; int minGlyphs; };
    const Case cases[] = {
        {"Annual_Report_2026.pdf", 200},
        {"Attention_Is_All_You_Need.pdf", 1000},
        {"Japanese_Government_Notice.pdf", 20},
    };

    for (const auto& c : cases) {
        const fs::path p = dir / c.file;
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;

        folio::engine::PdfiumWorker w;
        std::string err;
        check(w.open(readFile(p), &err), std::string("  open ") + c.file);

        auto text = waitForText(w, 0);
        check(text != nullptr, std::string("  text arrives for ") + c.file);
        if (!text) continue;

        check(static_cast<int>(text->glyphs.size()) >= c.minGlyphs,
              std::string("  enough glyphs in ") + c.file);
        check(text->folded.size() == text->glyphs.size(),
              "  folded string parallels the glyph list");
        note(std::string(c.file) + ": " + std::to_string(text->glyphs.size()) +
             " glyphs");

        // Every box must land inside the page, top-left origin. A sign error
        // in the y-flip puts the whole page off by one page height and makes
        // selection silently unusable - cheap to assert, painful to debug.
        int outside = 0, inverted = 0;
        for (const auto& g : text->glyphs) {
            if (g.l < -0.05f || g.r > 1.05f || g.t < -0.05f || g.b > 1.05f)
                ++outside;
            if (g.r < g.l || g.b < g.t) ++inverted;
        }
        check(outside == 0, "  all glyph boxes lie on the page");
        check(inverted == 0, "  no inverted glyph boxes");

        // Reading order: the first glyph should be at or near the top.
        check(text->glyphs.front().t < 0.5f, "  first glyph is in the top half");
    }
}

// ---------------------------------------------------------------------------
// /Outlines
//
// The outline panel showed "Chapter 1 / Section 1.1" rows generated from the
// page count until round 9. It looked like data and was not, so these checks
// are deliberately about provenance: real titles from the file, real
// destination pages, and an honestly empty list when the file has none.
// ---------------------------------------------------------------------------

void folio_test_outline() {
    namespace fs = std::filesystem;
    section("outline (/Outlines tree)");

    fs::path dir;
    for (const char* c : {"../../documents", "../documents", "documents"}) {
        std::error_code ec;
        if (fs::is_directory(c, ec)) { dir = c; break; }
    }
    if (dir.empty()) { check(false, "documents/ found"); return; }

    struct Expect {
        const char* file;
        bool hasOutline;
        const char* firstTitle;   // nullptr when none expected
        int firstPage;            // 1-based, -1 when none expected
        int minEntries;
    };
    const Expect cases[] = {
        {"Attention_Is_All_You_Need.pdf", true, "Introduction", 2, 20},
        {"lshort.pdf", true, "Thank you!", 3, 100},
        {"Contract_v3.pdf", false, nullptr, -1, 0},
        {"Annual_Report_2026.pdf", false, nullptr, -1, 0},
    };

    for (const auto& c : cases) {
        const fs::path path = dir / c.file;
        std::error_code ec;
        if (!fs::exists(path, ec)) continue;

        folio::engine::PdfiumWorker w;
        std::string err;
        if (!w.open(readFile(path), &err)) {
            check(false, std::string("open ") + c.file + ": " + err);
            continue;
        }
        const auto toc = w.outline();
        check(toc.empty() != c.hasOutline,
              std::string(c.file) + (c.hasOutline ? ": has an outline"
                                                  : ": has no outline"));
        if (!c.hasOutline) continue;

        check(static_cast<int>(toc.size()) >= c.minEntries,
              "  at least " + std::to_string(c.minEntries) + " entries (" +
                  std::to_string(toc.size()) + ")");
        check(toc.front().title == c.firstTitle,
              std::string("  first title is \"") + c.firstTitle + "\"");
        check(toc.front().page + 1 == c.firstPage,
              "  first destination is page " + std::to_string(c.firstPage));
        check(toc.front().depth == 0, "  first entry is top-level");

        // Provenance: a page-count-derived fake has a rigid shape. Real
        // bookmark titles vary in length and nest more than one level.
        int maxDepth = 0, blank = 0, offPage = 0;
        for (const auto& e : toc) {
            maxDepth = std::max(maxDepth, e.depth);
            if (e.title.empty()) ++blank;
            if (e.page < -1 || e.page >= w.pageCount()) ++offPage;
        }
        check(maxDepth >= 2, "  nests at least three levels deep");
        check(blank == 0, "  no blank titles");
        check(offPage == 0, "  every destination is a real page");
        note(std::string(c.file) + ": " + std::to_string(toc.size()) +
             " entries, max depth " + std::to_string(maxDepth));
    }
}
