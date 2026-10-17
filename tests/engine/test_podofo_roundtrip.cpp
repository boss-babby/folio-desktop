// PoDoFo write -> PDFium read. The only round-trip that matters.
//
// Each case edits a copy of a real document with PoDoFo, saves, then reopens
// the result with PDFium and asserts the change is actually visible. Checking
// only that PoDoFo wrote a parseable file would pass for annotations that no
// reader draws, which is the specific failure mode this guards: an annotation
// without an appearance stream is valid, round-trips through any parser, and
// is invisible everywhere.
//
// "Visible" is measured as changed pixels against the unedited render, which
// is a renderer-agnostic way of asking the only question a user cares about.

#include "podofo/PodofoDocument.h"

#include "pdfium/PdfiumLibrary.h"
#include "pdfium/PdfiumRasterizer.h"
#include <fpdf_annot.h>
#include <fpdfview.h>

#include "folio/core/engine/DocumentSnapshot.h"
#include "../Harness.h"

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using namespace folio::core;
using namespace folio::test;
using folio::engine::AnnotSpec;
using folio::engine::PodofoDocument;
using folio::engine::PdfiumRasterizer;

namespace {

namespace fs = std::filesystem;

std::vector<std::byte> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const auto len = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> out(len);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
    return out;
}

/// Render one page of a file with PDFium. Returns an empty tile on failure.
std::unique_ptr<RasterTile> render(const fs::path& file, int page) {
    auto bytes = readFile(file);
    if (bytes.empty()) return nullptr;
    auto snap = DocumentSnapshot::Builder::fromBase(std::move(bytes));
    PdfiumRasterizer r;
    r.bindToCurrentThread();
    if (!r.setSnapshot(snap)) return nullptr;
    if (page < 0 || page >= r.pageCount()) return nullptr;
    return r.renderFullPage(PageId{page}, 1.0f, 0, ColorScheme::Light);
}

int pageCountOf(const fs::path& file) {
    auto bytes = readFile(file);
    if (bytes.empty()) return -1;
    auto snap = DocumentSnapshot::Builder::fromBase(std::move(bytes));
    PdfiumRasterizer r;
    r.bindToCurrentThread();
    if (!r.setSnapshot(snap)) return -1;
    return r.pageCount();
}

/// Fraction of pixels that differ between two same-sized renders.
double differingFraction(const RasterTile& a, const RasterTile& b) {
    if (a.width != b.width || a.height != b.height) return 1.0;
    std::size_t diff = 0, total = 0;
    for (int y = 0; y < a.height; ++y) {
        const auto* ra = reinterpret_cast<const unsigned char*>(a.pixels.data()) +
                         static_cast<std::size_t>(y) * a.stride;
        const auto* rb = reinterpret_cast<const unsigned char*>(b.pixels.data()) +
                         static_cast<std::size_t>(y) * b.stride;
        for (int x = 0; x < a.width; ++x) {
            const int d = std::abs(ra[x * 4 + 0] - rb[x * 4 + 0]) +
                          std::abs(ra[x * 4 + 1] - rb[x * 4 + 1]) +
                          std::abs(ra[x * 4 + 2] - rb[x * 4 + 2]);
            if (d > 24) ++diff;
            ++total;
        }
    }
    return total ? static_cast<double>(diff) / static_cast<double>(total) : 0.0;
}

fs::path documentsDir() {
    for (const char* c : {"../../documents", "../documents", "documents"}) {
        std::error_code ec;
        if (fs::is_directory(c, ec)) return c;
    }
    return {};
}

}  // namespace

FOLIO_TEST_MAIN("podofo round-trip") {
    folio::engine::ensurePdfiumInitialised();

    const fs::path docs = documentsDir();
    check(!docs.empty(), "documents/ directory found");
    if (docs.empty()) return;

    const fs::path source = docs / "Annual_Report_2026.pdf";
    std::error_code ec;
    check(fs::exists(source, ec), "source document exists");
    if (!fs::exists(source, ec)) return;

    const fs::path tmp = fs::temp_directory_path() / "folio-podofo-tests";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    auto freshCopy = [&](const char* name) {
        const fs::path p = tmp / name;
        std::error_code e;
        fs::copy_file(source, p, fs::copy_options::overwrite_existing, e);
        return p;
    };

    // --- annotations become visible pixels ----------------------------------
    {
        section("annotations render after a save");
        const fs::path work = freshCopy("annots.pdf");
        auto before = render(work, 0);
        check(before != nullptr, "  baseline renders");

        PodofoDocument doc;
        check(doc.open(work.string()), "  PoDoFo opens the file");
        check(doc.pageCount() == 24, "  page count agrees with PDFium");

        std::vector<AnnotSpec> annots;
        AnnotSpec hl;
        hl.kind = AnnotSpec::Kind::Highlight;
        hl.page = 0;
        hl.l = 0.12f; hl.t = 0.26f; hl.r = 0.78f; hl.b = 0.30f;
        hl.cr = 255; hl.cg = 214; hl.cb = 10;
        hl.opacity = 0.45f;
        hl.contents = "\xE0\xA6\xA8\xE0\xA7\x8B\xE0\xA6\x9F \xE2\x80\x94 highlight comment";
        annots.push_back(hl);

        AnnotSpec ink;
        ink.kind = AnnotSpec::Kind::Ink;
        ink.page = 0;
        ink.cr = 229; ink.cg = 57; ink.cb = 53;
        ink.opacity = 1.0f;
        ink.width = 3.f;
        for (int i = 0; i <= 20; ++i) {
            const float t = static_cast<float>(i) / 20.f;
            ink.points.push_back({0.15f + t * 0.6f,
                                  0.45f + 0.05f * std::sin(t * 6.28f)});
        }
        ink.l = 0.15f; ink.t = 0.38f; ink.r = 0.75f; ink.b = 0.52f;
        annots.push_back(ink);

        AnnotSpec box;
        box.kind = AnnotSpec::Kind::Box;
        box.page = 0;
        box.l = 0.20f; box.t = 0.60f; box.r = 0.60f; box.b = 0.70f;
        box.cr = 33; box.cg = 118; box.cb = 255;
        box.opacity = 1.0f;
        box.width = 2.f;
        annots.push_back(box);

        check(doc.writeAnnotations(annots), "  annotations written");
        check(doc.dirty(), "  document reports itself dirty");
        check(doc.saveIncremental(), "  incremental save succeeds");
        {
            FPDF_DOCUMENT saved = FPDF_LoadDocument(work.string().c_str(), nullptr);
            FPDF_PAGE page = saved ? FPDF_LoadPage(saved, 0) : nullptr;
            bool foundUtf8Comment = false;
            if (page) {
                for (int i = 0; i < FPDFPage_GetAnnotCount(page); ++i) {
                    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
                    if (!annot) continue;
                    std::array<FPDF_WCHAR, 128> value{};
                    FPDFAnnot_GetStringValue(annot, "Contents", value.data(),
                                              value.size() * sizeof(FPDF_WCHAR));
                    foundUtf8Comment |= value[0] == 0x09a8u;
                    FPDFPage_CloseAnnot(annot);
                }
                FPDF_ClosePage(page);
            }
            if (saved) FPDF_CloseDocument(saved);
            check(foundUtf8Comment,
                  "  exported highlight has Unicode comment readable by another PDF reader");
        }
        check(!doc.dirty(), "  save clears the dirty flag");

        auto after = render(work, 0);
        check(after != nullptr, "  edited file still renders in PDFium");
        if (before && after) {
            const double d = differingFraction(*before, *after);
            char buf[80];
            std::snprintf(buf, sizeof buf, "changed pixels: %.2f%%", d * 100.0);
            note(buf);
            // An appearance-stream-less annotation scores ~0 here.
            check(d > 0.01, "  annotations are actually visible to PDFium");
        }

        // Saving twice must not stack duplicates.
        PodofoDocument again;
        check(again.open(work.string()), "  reopens after save");
        check(again.writeAnnotations(annots), "  rewrite succeeds");
        check(again.saveIncremental(), "  second save succeeds");
        auto twice = render(work, 0);
        if (after && twice)
            check(differingFraction(*after, *twice) < 0.005,
                  "  re-saving is idempotent, not cumulative");
    }

    // --- incremental save preserves the original bytes ----------------------
    {
        section("incremental save appends");
        const fs::path work = freshCopy("incr.pdf");
        const auto originalSize = fs::file_size(work, ec);
        const auto originalHead = readFile(work);

        PodofoDocument doc;
        check(doc.open(work.string()), "  opens");
        AnnotSpec a;
        a.page = 0;
        a.l = 0.1f; a.t = 0.1f; a.r = 0.4f; a.b = 0.14f;
        check(doc.writeAnnotations({a}), "  writes");
        check(doc.saveIncremental(), "  saves");

        const auto newSize = fs::file_size(work, ec);
        check(newSize > originalSize, "  file grew");

        // The defining property of an incremental update: the original
        // revision is still there, byte for byte, at the front of the file.
        const auto updated = readFile(work);
        bool prefixIntact = updated.size() >= originalHead.size() &&
                            std::memcmp(updated.data(), originalHead.data(),
                                        originalHead.size()) == 0;
        check(prefixIntact, "  original revision preserved byte-for-byte");
        char buf[96];
        std::snprintf(buf, sizeof buf, "%zu -> %zu bytes (+%zu)",
                      static_cast<std::size_t>(originalSize),
                      static_cast<std::size_t>(newSize),
                      static_cast<std::size_t>(newSize - originalSize));
        note(buf);
    }

    // --- page operations ----------------------------------------------------
    {
        section("page operations");
        const fs::path work = freshCopy("pages.pdf");

        PodofoDocument doc;
        check(doc.open(work.string()), "  opens");
        check(doc.pageRotation(0) == 0, "  starts unrotated");
        check(doc.rotatePage(0, 90), "  rotate");
        check(doc.pageRotation(0) == 90, "  rotation recorded");
        check(doc.deletePage(5), "  delete");
        check(doc.insertBlankPage(2), "  insert");
        check(doc.saveAs(work.string()), "  full save");

        check(pageCountOf(work) == 24, "  PDFium sees 24 pages (-1 +1)");

        // A rotated page swaps its reported dimensions in the reader.
        auto bytes = readFile(work);
        auto snap = DocumentSnapshot::Builder::fromBase(std::move(bytes));
        PdfiumRasterizer r;
        r.bindToCurrentThread();
        if (r.setSnapshot(snap)) {
            const auto info = r.pageInfo(PageId{0});
            check(info.rotation == 90, "  PDFium reports the new rotation");
            // The invariant the page source depends on: PDFium's page size
            // query returns *display* dimensions, with /Rotate already
            // applied. An A4 page rotated 90 degrees must come back
            // landscape. Swapping again on top of this is what turned one
            // rotation into two in the reader.
            check(info.widthPt > info.heightPt,
                  "  rotated page reports swapped (display) dimensions");
            char buf[80];
            std::snprintf(buf, sizeof buf, "page 0: %.0fx%.0f rot %d",
                          info.widthPt, info.heightPt, info.rotation);
            note(buf);
        }

        const fs::path extracted = tmp / "extract.pdf";
        PodofoDocument doc2;
        check(doc2.open(work.string()), "  reopens");
        check(doc2.extractPages({0, 3, 7}, extracted.string()), "  extract three");
        check(pageCountOf(extracted) == 3, "  extracted file has three pages");

        // An extract must not smuggle the pages it left behind.
        //
        // PoDoFo builds the output by copying every object from the source
        // and linking only the chosen pages, so without an explicit sweep
        // the file still holds the content streams of all the others -
        // invisible to a viewer, trivially recoverable from the raw bytes.
        // These two checks are the only thing standing between "extract
        // page 3 of the contract" and shipping the other ten.
        const auto sizeOf = [](const fs::path& f) {
            return static_cast<std::uintmax_t>(fs::file_size(f));
        };
        check(sizeOf(extracted) < sizeOf(work) * 3 / 4,
              "  extract is materially smaller than the source");

        std::ifstream raw(extracted, std::ios::binary);
        const std::string rawBytes((std::istreambuf_iterator<char>(raw)),
                                   std::istreambuf_iterator<char>());
        const auto countOf = [&rawBytes](const std::string& needle) {
            std::size_t n = 0, at = 0;
            while ((at = rawBytes.find(needle, at)) != std::string::npos) { ++n; ++at; }
            return n;
        };
        check(countOf("/Type/Catalog") + countOf("/Type /Catalog") == 1,
              "  exactly one catalogue survives the sweep");
    }

    // --- free text ----------------------------------------------------------
    {
        section("free text");
        const fs::path work = freshCopy("freetext.pdf");
        PodofoDocument doc;
        check(doc.open(work.string()), "  opens");

        AnnotSpec a;
        a.kind = AnnotSpec::Kind::FreeText;
        a.page = 0;
        a.l = 0.20f; a.t = 0.20f; a.r = 0.70f; a.b = 0.24f;
        a.cr = 30; a.cg = 32; a.cb = 44;
        a.opacity = 1.f;
        a.contents = "Reviewed by Legal";
        a.fontSize = 14.f;
        check(doc.writeAnnotations({a}), "  writes");
        check(doc.saveIncremental(), "  saves");

        std::ifstream raw(work, std::ios::binary);
        const std::string b((std::istreambuf_iterator<char>(raw)),
                            std::istreambuf_iterator<char>());
        check(b.find("/FreeText") != std::string::npos, "  /FreeText subtype present");
        {
            FPDF_DOCUMENT saved = FPDF_LoadDocument(work.string().c_str(), nullptr);
            FPDF_PAGE page = saved ? FPDF_LoadPage(saved, 0) : nullptr;
            bool readable = false;
            if (page) {
                for (int i = 0; i < FPDFPage_GetAnnotCount(page); ++i) {
                    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
                    if (!annot) continue;
                    std::array<FPDF_WCHAR, 128> value{};
                    FPDFAnnot_GetStringValue(annot, "Contents", value.data(),
                                              value.size() * sizeof(FPDF_WCHAR));
                    readable |= value[0] == 'R' && value[1] == 'e';
                    FPDFPage_CloseAnnot(annot);
                }
                FPDF_ClosePage(page);
            }
            if (saved) FPDF_CloseDocument(saved);
            check(readable, "  FreeText contents readable by another PDF reader");
        }
        // Without an appearance stream the annotation is valid, parses
        // everywhere, and is invisible everywhere - the exact trap that
        // cost a round earlier in this project.
        check(b.find("/AP") != std::string::npos, "  has an appearance stream");
        check(b.find("Helvetica") != std::string::npos, "  font resource embedded");
        check(pageCountOf(work) > 0, "  still opens in PDFium");

        // The picker offers three families; they have to reach the file,
        // not just the screen.
        const fs::path serifDoc = freshCopy("freetext-serif.pdf");
        PodofoDocument d3;
        check(d3.open(serifDoc.string()), "  serif: opens");
        AnnotSpec t2 = a;
        t2.fontName = "Times-Roman";
        t2.contents = "Serif sample";
        check(d3.writeAnnotations({t2}), "  serif: writes");
        check(d3.saveIncremental(), "  serif: saves");
        std::ifstream raw2(serifDoc, std::ios::binary);
        const std::string b2((std::istreambuf_iterator<char>(raw2)),
                             std::istreambuf_iterator<char>());
        check(b2.find("Times") != std::string::npos,
              "  serif: Times font recorded");
        check(pageCountOf(serifDoc) > 0, "  serif: still opens in PDFium");
    }

    // --- metadata -----------------------------------------------------------
    {
        section("metadata");
        const fs::path work = freshCopy("meta.pdf");
        PodofoDocument doc;
        check(doc.open(work.string()), "  opens");
        PodofoDocument::Metadata m;
        m.title = "Round-trip title";
        m.author = "Folio";
        check(doc.setMetadata(m), "  sets");
        check(doc.saveIncremental(), "  saves");

        PodofoDocument back;
        check(back.open(work.string()), "  reopens");
        check(back.metadata().title == "Round-trip title", "  title survived");
        check(back.metadata().author == "Folio", "  author survived");
    }

    // --- text markup: underline and strikeout -------------------------------
    //
    // These two arrived with the selection popup. They are the kinds most
    // likely to regress silently: a text markup annotation with the wrong
    // /QuadPoints order, or none at all, still parses, still has the right
    // subtype, and is simply invisible. So this measures pixels AND checks
    // the subtype byte actually reached the file.
    {
        section("underline and strikeout");
        const fs::path work = freshCopy("markup.pdf");
        auto before = render(work, 0);
        check(before != nullptr, "  baseline renders");

        PodofoDocument doc;
        check(doc.open(work.string()), "  opens");

        std::vector<AnnotSpec> annots;
        AnnotSpec ul;
        ul.kind = AnnotSpec::Kind::Underline;
        ul.page = 0;
        ul.l = 0.12f; ul.t = 0.26f; ul.r = 0.78f; ul.b = 0.295f;
        ul.cr = 229; ul.cg = 57; ul.cb = 53;
        ul.opacity = 1.0f;
        annots.push_back(ul);

        AnnotSpec st = ul;
        st.kind = AnnotSpec::Kind::StrikeOut;
        st.t = 0.34f; st.b = 0.375f;
        annots.push_back(st);

        check(doc.writeAnnotations(annots), "  written");
        check(doc.saveIncremental(), "  saved");

        auto after = render(work, 0);
        check(after != nullptr, "  still renders");
        if (before && after) {
            const double d = differingFraction(*before, *after);
            char buf[80];
            std::snprintf(buf, sizeof buf, "changed pixels: %.3f%%", d * 100.0);
            note(buf);
            // Two hairlines across two thirds of the page: a much smaller
            // footprint than a highlight, so the threshold is lower - but
            // an /AP-less annotation would still score a flat zero.
            check(d > 0.0008, "  both rules are visible to PDFium");
        }

        // And the subtypes really are text markup, not squares.
        std::ifstream f(work, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
        check(bytes.find("/Underline") != std::string::npos,
              "  /Underline subtype present");
        check(bytes.find("/StrikeOut") != std::string::npos,
              "  /StrikeOut subtype present");
        const std::size_t quads =
            [&] {
                std::size_t n = 0, pos = 0;
                while ((pos = bytes.find("/QuadPoints", pos)) != std::string::npos) {
                    ++n;
                    pos += 11;
                }
                return n;
            }();
        check(quads >= 2, "  both carry /QuadPoints");
    }

    fs::remove_all(tmp, ec);
}
