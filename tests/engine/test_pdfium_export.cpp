#include "pdfium/PdfiumAnnotationWriter.h"
#include "pdfium/PdfiumLibrary.h"
#include "pdfium/PdfiumWorker.h"
#include "pdfium/FileMappedAccess.h"
#include "pdfium/PdfiumAnnotationIdentity.h"
#include <thread>
#include <chrono>
#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <cstdio>
#include <fpdfview.h>
#include <array>
#include <filesystem>
#include <fstream>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif
#include <iostream>
#include <string>
#include <vector>

using folio::engine::AnnotSpec;
namespace fs = std::filesystem;
std::vector<char> bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::vector<unsigned char> render(FPDF_DOCUMENT doc) {
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    if (!page) return {};
    auto bitmap = FPDFBitmap_Create(480, 640, 1);
    FPDFBitmap_FillRect(bitmap, 0, 0, 480, 640, 0xffffffff);
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, 480, 640, 0, FPDF_ANNOT);
    auto* ptr = static_cast<unsigned char*>(FPDFBitmap_GetBuffer(bitmap));
    const int size = FPDFBitmap_GetStride(bitmap) * 640;
    std::vector<unsigned char> result(ptr, ptr + size);
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);
    return result;
}
int main() {
    folio::engine::ensurePdfiumInitialised();
    folio::engine::setPdfiumExportFontDirectory(std::string(FOLIO_TEST_FONT_DIR));
    const fs::path source = "folio-test.pdf";
    const auto original = bytes(source);
    if (original.empty()) return 1;
    const auto temp = fs::temp_directory_path() / "folio-pdfium-writer-roundtrip.pdf";
    FPDF_DOCUMENT base = FPDF_LoadDocument(source.string().c_str(), nullptr);
    if (!base) return 2;
    const auto before = render(base);
    auto bp = FPDF_LoadPage(base, 0);
    const int baseCount = FPDFPage_GetAnnotCount(bp);
    FPDF_ClosePage(bp);
    const std::array<AnnotSpec::Kind, 7> kinds = {
        AnnotSpec::Kind::Highlight, AnnotSpec::Kind::Underline,
        AnnotSpec::Kind::StrikeOut, AnnotSpec::Kind::Ink,
        AnnotSpec::Kind::Note, AnnotSpec::Kind::Box, AnnotSpec::Kind::Ellipse};
    for (auto kind : kinds) {
        AnnotSpec mark;
        mark.kind = kind;
        mark.l = 0.1f; mark.t = 0.14f; mark.r = 0.45f; mark.b = 0.24f;
        mark.page = 0;
        mark.opacity = kind == AnnotSpec::Kind::Highlight ? 0.38f : 1.f;
        mark.cr = 255; mark.cg = 40; mark.cb = 40;
        mark.width = 2;
        mark.contents = "\xE6\xB3\xA8";
        mark.points = {{0.1f, 0.16f}, {0.3f, 0.20f}, {0.44f, 0.16f}};
        std::string error;
        if (!folio::engine::writePdfiumAnnotations(source.string(), temp.string(),
                                                   {mark}, error)) {
            std::cerr << "writer failed for kind " << int(kind) << ": " << error << '\n';
            return 3;
        }
        auto saved = FPDF_LoadDocument(temp.string().c_str(), nullptr);
        if (!saved || FPDF_GetPageCount(saved) != FPDF_GetPageCount(base)) return 4;
        auto page = FPDF_LoadPage(saved, 0);
        if (!page || FPDFPage_GetAnnotCount(page) != baseCount + 1) return 5;
        auto annot = FPDFPage_GetAnnot(page, baseCount);
        if (!annot) return 6;
        std::array<FPDF_WCHAR, 32> contents{};
        if (FPDFAnnot_GetStringValue(annot, "Contents", contents.data(),
                                     contents.size() * sizeof(FPDF_WCHAR)) <= 2 ||
            contents[0] != 0x6ce8u) {
            std::cerr << "export did not preserve UTF-8 annotation comment\n";
            return 13;
        }
        std::array<FPDF_WCHAR, 2048> ap{};
        if (FPDFAnnot_GetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                            ap.data(), static_cast<unsigned long>(ap.size() * sizeof(FPDF_WCHAR))) <= 2) return 7;
        FPDFPage_CloseAnnot(annot);
        FPDF_ClosePage(page);
        if (render(saved) == before) {
            std::cerr << "annotation invisible, kind " << int(kind) << '\n';
            return 8;
        }
        FPDF_CloseDocument(saved);
    }
    {
        folio::engine::PdfiumWorker worker;
        std::string openError;
        if (!worker.openFile(temp.string(), &openError)) return 17;
        worker.requestAnnotations(0);
        bool indexed = false;
        for (int i = 0; i < 100 && !indexed; ++i) {
            for (const auto& result : worker.drain()) {
                if (result.kind != folio::engine::PdfiumWorker::Kind::Annotations)
                    continue;
                if (result.annotations.size() != static_cast<std::size_t>(baseCount + 1) ||
                    result.annotations.back().contents != "\xE6\xB3\xA8") return 18;
                indexed = true;
            }
            if (!indexed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!indexed) return 19;
    }
    std::string error;
    // Text and rubber stamps must have a real, visible font-backed AP. PDFium
    // supports object-backed text only in stamp appearances, not FreeText.
    for (auto kind : {AnnotSpec::Kind::FreeText, AnnotSpec::Kind::Stamp}) {
        AnnotSpec text;
        text.kind = kind;
        text.l = .1f; text.t = .14f; text.r = .6f; text.b = .23f;
        text.label = kind == AnnotSpec::Kind::Stamp ? "APPROVED" : "Text from Folio";
        text.contents = "comment retained";
        text.opacity = 1.f;
        if (!folio::engine::writePdfiumAnnotations(source.string(), temp.string(),
                                                   {text}, error)) {
            std::cerr << "visible text export failed: " << error << '\n';
            return 9;
        }
        auto saved = FPDF_LoadDocument(temp.string().c_str(), nullptr);
        if (!saved || render(saved) == before) return 14;
        auto pg = FPDF_LoadPage(saved, 0);
        auto an = FPDFPage_GetAnnot(pg, baseCount);
        if (!an || FPDFAnnot_GetSubtype(an) != FPDF_ANNOT_STAMP ||
            FPDFAnnot_GetObjectCount(an) == 0) return 15;
        FPDFPage_CloseAnnot(an);
        FPDF_ClosePage(pg);
        FPDF_CloseDocument(saved);
    }
    if (bytes(source) != original) return 10;
    AnnotSpec bengali;
    bengali.kind = AnnotSpec::Kind::FreeText;
    bengali.l = .1f; bengali.t = .14f;
    bengali.r = .85f; bengali.b = .23f;
    // A conjunct and Latin run exercise Bengali shaping and mixed text.
    bengali.label = "বাংলা শিক্ষা Folio";
    bengali.contents = bengali.label;
    if (!folio::engine::writePdfiumAnnotations(source.string(), temp.string(),
                                               {bengali}, error)) {
        std::cerr << "Bengali export failed: " << error << '\n'; return 16;
    }
    auto shaped = FPDF_LoadDocument(temp.string().c_str(), nullptr);
    if (!shaped || render(shaped) == before) return 20;
    auto shapedPage = FPDF_LoadPage(shaped, 0);
    auto shapedAnnot = FPDFPage_GetAnnot(shapedPage, baseCount);
    if (!shapedAnnot || FPDFAnnot_GetObjectCount(shapedAnnot) < 4) return 21;
    std::array<FPDF_WCHAR, 64> unicode{};
    if (FPDFAnnot_GetStringValue(shapedAnnot, "Contents", unicode.data(),
         unicode.size() * sizeof(FPDF_WCHAR)) <= 2 || unicode[0] != 0x09AC) return 22;
    FPDFPage_CloseAnnot(shapedAnnot); FPDF_ClosePage(shapedPage);
    FPDF_CloseDocument(shaped);
    AnnotSpec unsupported = bengali;
    unsupported.label = "\xF0\x9F\xA6\x84"; // no emoji outline in the bundled fonts
    if (folio::engine::writePdfiumAnnotations(source.string(), temp.string(),
                                               {unsupported}, error) ||
        error.empty() || fs::exists(temp)) {
        std::cerr << "unsupported script was silently exported without glyphs\n";
        return 23;
    }
    // Replace a supported third-party annotation rather than duplicating
    // the pre-rendered mark. Source bytes and unrelated objects stay intact.
    const auto imported = fs::temp_directory_path() / "folio-pdfium-imported.pdf";
    AnnotSpec foreign;
    foreign.kind = AnnotSpec::Kind::Highlight;
    foreign.page = 0;
    foreign.l = .15f; foreign.t = .18f; foreign.r = .45f; foreign.b = .24f;
    foreign.contents = "Original comment";
    if (!folio::engine::writePdfiumAnnotations(source.string(), imported.string(),
                                                {foreign}, error)) return 26;
    const auto foreignBytes = bytes(imported);
    auto edited = foreign;
    edited.sourceIndex = baseCount;
    edited.sourceSubtype = FPDF_ANNOT_HIGHLIGHT;
    { auto sourceDoc = FPDF_LoadDocument(imported.string().c_str(), nullptr);
      auto sourcePage = FPDF_LoadPage(sourceDoc, 0);
      auto sourceMark = FPDFPage_GetAnnot(sourcePage, baseCount);
      edited.sourceFingerprint = folio::engine::pdfAnnotationFingerprint(sourceMark);
      FPDFPage_CloseAnnot(sourceMark); FPDF_ClosePage(sourcePage);
      FPDF_CloseDocument(sourceDoc);
    }
    edited.contents = "Edited comment";
    edited.cr = 80;
    if (!folio::engine::writePdfiumAnnotations(imported.string(), temp.string(),
                                                {edited}, error)) {
        std::cerr << "imported edit export failed: " << error << '\n'; return 27;
    }
    auto replacement = FPDF_LoadDocument(temp.string().c_str(), nullptr);
    auto rp = FPDF_LoadPage(replacement, 0);
    if (FPDFPage_GetAnnotCount(rp) != baseCount + 1) return 28;
    auto ra = FPDFPage_GetAnnot(rp, baseCount);
    std::array<FPDF_WCHAR, 64> editedContents{};
    if (FPDFAnnot_GetStringValue(ra, "Contents", editedContents.data(),
         editedContents.size() * sizeof(FPDF_WCHAR)) <= 2 ||
        editedContents[0] != 'E') return 29;
    FPDFPage_CloseAnnot(ra); FPDF_ClosePage(rp); FPDF_CloseDocument(replacement);
    edited.deleted = true;
    if (!folio::engine::writePdfiumAnnotations(imported.string(), temp.string(),
                                                {edited}, error)) return 30;
    replacement = FPDF_LoadDocument(temp.string().c_str(), nullptr);
    rp = FPDF_LoadPage(replacement, 0);
    if (FPDFPage_GetAnnotCount(rp) != baseCount) return 31;
    FPDF_ClosePage(rp); FPDF_CloseDocument(replacement);
    edited.sourceSubtype = FPDF_ANNOT_TEXT; // wrong identity must fail closed
    if (folio::engine::writePdfiumAnnotations(imported.string(), temp.string(),
                                               {edited}, error) || error.empty()) return 32;
    if (bytes(imported) != foreignBytes) return 33;
    // The viewer suppresses only the imported object in its in-memory PDF;
    // the source file is byte-for-byte unchanged, even after rendering.
    {
        folio::engine::PdfiumWorker view;
        std::string err;
        if (!view.openFile(imported.string(), &err)) return 34;
        view.setSuppressed({{0, {{baseCount, FPDF_ANNOT_HIGHLIGHT}}}});
        view.requestAnnotations(0);
        view.requestRender(0, 1.f, 0, 612);
        bool metadata = false, image = false;
        for (int tries = 0; tries < 200 && (!metadata || !image); ++tries) {
            for (auto& result : view.drain()) {
                if (result.kind == folio::engine::PdfiumWorker::Kind::Annotations) {
                    if (result.annotations.size() != static_cast<std::size_t>(baseCount)) return 35;
                    metadata = true;
                }
                if (result.kind == folio::engine::PdfiumWorker::Kind::Render)
                    image = result.tile && !result.tile->pixels.empty();
            }
            if (!metadata || !image) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!metadata || !image || bytes(imported) != foreignBytes) return 36;
    }
    fs::remove(imported);
    // A rotated page must place the new mark in DISPLAY-normalised space.
    struct FileWriter : FPDF_FILEWRITE {
        std::FILE* file;
        explicit FileWriter(std::FILE* f) : file(f) {
            version = 1;
            WriteBlock = [](FPDF_FILEWRITE* w, const void* p, unsigned long len) -> int {
                auto* out = static_cast<FileWriter*>(w);
                return std::fwrite(p, 1, len, out->file) == len;
            };
        }
    };
    const fs::path rotated = fs::temp_directory_path() / "folio-pdfium-export-rotated.pdf";
    FPDF_DOCUMENT rotDoc = FPDF_LoadDocument(source.string().c_str(), nullptr);
    auto rotPage = FPDF_LoadPage(rotDoc, 0);
    FPDFPage_SetRotation(rotPage, 1);
    FPDF_ClosePage(rotPage);
    auto* f = std::fopen(rotated.string().c_str(), "wb");
    FileWriter sink(f);
    if (!FPDF_SaveAsCopy(rotDoc, &sink, FPDF_NO_INCREMENTAL) || std::fclose(f)) return 11;
    FPDF_CloseDocument(rotDoc);
    rotDoc = FPDF_LoadDocument(rotated.string().c_str(), nullptr);
    const auto oldPixels = render(rotDoc);
    FPDF_CloseDocument(rotDoc);
    AnnotSpec box;
    box.kind = AnnotSpec::Kind::Box;
    box.l = 0.1f; box.t = 0.14f; box.r = 0.45f; box.b = 0.24f;
    box.cr = 255; box.cg = 0; box.cb = 0;
    if (!folio::engine::writePdfiumAnnotations(rotated.string(), temp.string(), {box}, error)) {
        std::cerr << "rotated export failed: " << error << '\n'; return 12;
    }
    rotDoc = FPDF_LoadDocument(temp.string().c_str(), nullptr);
    auto newPixels = render(rotDoc);
    FPDF_CloseDocument(rotDoc);
    // Distinct mark pixels should fall within the displayed normalised box.
    bool inBox = false;
    for (int y = 85; y < 170; ++y) {
        for (int x = 40; x < 230; ++x) {
            const auto at = static_cast<std::size_t>((y * 480 + x) * 4);
            if (newPixels[at] != oldPixels[at] ||
                newPixels[at + 1] != oldPixels[at + 1] ||
                newPixels[at + 2] != oldPixels[at + 2]) inBox = true;
        }
    }
    if (!inBox) { std::cerr << "rotated mark missed displayed rectangle\n"; return 13; }
    // A stable library locator may have no PDF file at all. A fresh FD must
    // resolve it for every worker open and for Save As; no /proc fd is saved.
#if !defined(_WIN32)
    const fs::path linked = fs::temp_directory_path() / "folio-linked-locator.pdf";
    const fs::path marker = linked.string() + ".folio-link";
    { std::ofstream out(marker); out << "source-only\n"; }
    int fdOpens = 0;
    folio::engine::setLinkedPdfOpener([&](const std::string& path) {
        if (path != linked.string()) return -1;
        ++fdOpens;
        return ::open(source.c_str(), O_RDONLY);
    });
    { folio::engine::PdfiumWorker linkedWorker;
      std::string err;
      if (!linkedWorker.openFile(linked.string(), &err) || linkedWorker.pageCount() < 1) return 24;
    }
    if (!folio::engine::writePdfiumAnnotations(linked.string(), temp.string(),
                                               {box}, error) || fdOpens < 2) {
        std::cerr << "linked FD export failed: " << error << '\n'; return 25;
    }
    folio::engine::setLinkedPdfOpener({});
    fs::remove(marker);
#endif
    fs::remove(rotated);
    fs::remove(temp);
    FPDF_CloseDocument(base);
    std::cout << "7 visible annotation kinds + Bengali shaping + unsupported failure + source immutability\n";
    return 0;
}
