#include "pdfium/PdfiumAnnotationWriter.h"
#include "pdfium/PdfiumLibrary.h"
#include "pdfium/PdfiumAnnotationIdentity.h"
#include "pdfium/FileMappedAccess.h"
#if defined(FOLIO_SHAPED_PDF_TEXT)
#include "pdfium/PdfiumOutlinedText.h"
#endif

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <sstream>
#include <locale>
#include <type_traits>
#include <string>
#include <vector>

namespace folio::engine {
namespace {
std::mutex fontDirectoryMutex;
std::string exportFontDirectory;
struct DocCloser { void operator()(FPDF_DOCUMENT d) const { FPDF_CloseDocument(d); } };
struct PageCloser { void operator()(FPDF_PAGE p) const { FPDF_ClosePage(p); } };
struct AnnotCloser { void operator()(FPDF_ANNOTATION a) const { FPDFPage_CloseAnnot(a); } };
using Doc = std::unique_ptr<std::remove_pointer_t<FPDF_DOCUMENT>, DocCloser>;
using Page = std::unique_ptr<std::remove_pointer_t<FPDF_PAGE>, PageCloser>;
using Annot = std::unique_ptr<std::remove_pointer_t<FPDF_ANNOTATION>, AnnotCloser>;

// PDFium's wide strings are UTF-16LE, NOT wchar_t (32-bit on Android).
std::vector<FPDF_WCHAR> wide(const std::string& input) {
    std::vector<FPDF_WCHAR> result;
    for (std::size_t i = 0; i < input.size();) {
        unsigned char c = static_cast<unsigned char>(input[i++]);
        std::uint32_t cp = c;
        int extra = 0;
        if (c >= 0xF0 && c <= 0xF4) { cp = c & 7u; extra = 3; }
        else if (c >= 0xE0 && c <= 0xEF) { cp = c & 15u; extra = 2; }
        else if (c >= 0xC2 && c <= 0xDF) { cp = c & 31u; extra = 1; }
        else if (c >= 0x80) { cp = 0xFFFD; }
        for (int j = 0; j < extra; ++j) {
            if (i >= input.size() || (static_cast<unsigned char>(input[i]) & 0xC0u) != 0x80u) {
                cp = 0xFFFD; break;
            }
            cp = (cp << 6u) | (static_cast<unsigned char>(input[i++]) & 63u);
        }
        if (cp > 0xFFFFu) {
            cp -= 0x10000u;
            result.push_back(static_cast<FPDF_WCHAR>(0xD800u + (cp >> 10u)));
            result.push_back(static_cast<FPDF_WCHAR>(0xDC00u + (cp & 1023u)));
        } else result.push_back(static_cast<FPDF_WCHAR>(cp));
    }
    result.push_back(0);
    return result;
}

FPDF_ANNOTATION_SUBTYPE subtype(AnnotSpec::Kind kind) {
    using K = AnnotSpec::Kind;
    switch (kind) {
        case K::Highlight: return FPDF_ANNOT_HIGHLIGHT;
        case K::Underline: return FPDF_ANNOT_UNDERLINE;
        case K::StrikeOut: return FPDF_ANNOT_STRIKEOUT;
        case K::Ink: return FPDF_ANNOT_INK;
        case K::Note: return FPDF_ANNOT_TEXT;
        case K::Box: return FPDF_ANNOT_SQUARE;
        case K::Ellipse: return FPDF_ANNOT_CIRCLE;
        // PDFium can append font-backed text objects only to Stamp or Ink.
        // Use a visible Stamp appearance for Folio free text rather than an
        // invisible /AP-less FreeText annotation.
        case K::FreeText:
        case K::Stamp: return FPDF_ANNOT_STAMP;
    }
    return FPDF_ANNOT_UNKNOWN;
}

// DeviceToPage handles /Rotate, nonzero crop origin and reversed axes. The
// PDF's visible page is always addressed in display-normalised coordinates.
FS_POINTF point(FPDF_PAGE page, float x, float y) {
    const int w = std::max(1, static_cast<int>(std::lround(FPDF_GetPageWidthF(page) * 100)));
    const int h = std::max(1, static_cast<int>(std::lround(FPDF_GetPageHeightF(page) * 100)));
    double px = 0, py = 0;
    FPDF_DeviceToPage(page, 0, 0, w, h, 0,
                      static_cast<int>(std::lround(x * static_cast<float>(w))),
                      static_cast<int>(std::lround(y * static_cast<float>(h))), &px, &py);
    return {static_cast<float>(px), static_cast<float>(py)};
}

struct Output : FPDF_FILEWRITE {
    std::FILE* file = nullptr;
    bool failed = false;
    explicit Output(std::FILE* f) : file(f) {
        version = 1;
        WriteBlock = [](FPDF_FILEWRITE* self, const void* bytes, unsigned long count) -> int {
            auto* out = static_cast<Output*>(self);
            if (std::fwrite(bytes, 1, count, out->file) != count) {
                out->failed = true;
                return 0;
            }
            return 1;
        };
    }
};

bool add(FPDF_DOCUMENT doc, FPDF_PAGE page, const AnnotSpec& mark, std::string& error) {
    const auto type = subtype(mark.kind);
    if (!FPDFAnnot_IsSupportedSubtype(type)) { error = "PDFium cannot create this annotation type"; return false; }
    Annot a(FPDFPage_CreateAnnot(page, type));
    if (!a) { error = "could not create annotation"; return false; }
    auto fail = [&](const char* why) { error = why; return false; };

    auto tl = point(page, mark.l, mark.t), tr = point(page, mark.r, mark.t);
    auto bl = point(page, mark.l, mark.b), br = point(page, mark.r, mark.b);
    FS_RECTF rect{std::min({tl.x, tr.x, bl.x, br.x}),
                  std::max({tl.y, tr.y, bl.y, br.y}),
                  std::max({tl.x, tr.x, bl.x, br.x}),
                  std::min({tl.y, tr.y, bl.y, br.y})};
    if (!(rect.right > rect.left && rect.top > rect.bottom) ||
        !FPDFAnnot_SetRect(a.get(), &rect)) return fail("invalid annotation rectangle");
    const float rw = rect.right - rect.left, rh = rect.top - rect.bottom;
    if (!FPDFAnnot_SetFlags(a.get(), FPDF_ANNOT_FLAG_PRINT)) return fail("annotation flags failed");
    const unsigned alpha = static_cast<unsigned>(std::lround(
        std::clamp(mark.opacity, 0.f, 1.f) * 255.f));
    if (!FPDFAnnot_SetColor(a.get(), FPDFANNOT_COLORTYPE_Color,
                            mark.cr, mark.cg, mark.cb, alpha))
        return fail("annotation colour failed");
    if (!FPDFAnnot_SetStringValue(a.get(), "T", wide("Folio").data()))
        return fail("annotation title failed");
    if (!mark.contents.empty() &&
        !FPDFAnnot_SetStringValue(a.get(), "Contents", wide(mark.contents).data()))
        return fail("annotation contents failed");
    using K = AnnotSpec::Kind;
    if (mark.kind == K::Highlight || mark.kind == K::Underline || mark.kind == K::StrikeOut) {
        const FS_QUADPOINTSF quad{tl.x, tl.y, tr.x, tr.y,
                                  bl.x, bl.y, br.x, br.y};
        if (!FPDFAnnot_AppendAttachmentPoints(a.get(), &quad))
            return fail("annotation quad points failed");
    } else if (mark.kind == K::Ink) {
        if (mark.points.size() < 2) return fail("ink stroke has fewer than two points");
        std::vector<FS_POINTF> points;
        points.reserve(mark.points.size());
        for (const auto& p : mark.points) points.push_back(point(page, p[0], p[1]));
        if (FPDFAnnot_AddInkStroke(a.get(), points.data(), points.size()) < 0)
            return fail("ink stroke failed");
        if (!FPDFAnnot_SetBorder(a.get(), 0, 0, mark.width))
            return fail("ink border failed");
    } else if (mark.kind == K::Box || mark.kind == K::Ellipse) {
        if (!FPDFAnnot_SetBorder(a.get(), 0, 0, mark.width))
            return fail("annotation border failed");
    } else if (mark.kind == K::FreeText || mark.kind == K::Stamp) {
        // Stamp is the only PDFium-created subtype whose AP accepts text page
        // objects with a real font resource. This embeds a visible, editable
        // annotation without replacing or flattening the source PDF page.
        const std::string label = mark.label.empty() ? mark.contents : mark.label;
        if (label.empty()) return fail("empty text annotation");
        const float size = std::clamp(mark.fontSize > 0.f ? mark.fontSize : rh * 0.54f,
                                      1.f, std::max(1.f, rh * 0.85f));
        const auto utf16 = wide(label);
        bool nonWinAnsi = false;
        for (std::size_t i = 0; i + 1 < utf16.size(); ++i) {
            const std::uint32_t cp = utf16[i];
            if (!((cp >= 32 && cp <= 126) || (cp >= 0xA0 && cp <= 0xFF) ||
                  cp == 0x20AC || cp == 0x2013 || cp == 0x2014 ||
                  (cp >= 0x2018 && cp <= 0x201E) || cp == 0x2022 ||
                  cp == 0x2026 || cp == 0x2122 || cp == '\n' || cp == '\r'))
                nonWinAnsi = true;
        }
        if (nonWinAnsi) {
#if defined(FOLIO_SHAPED_PDF_TEXT)
            // PDFium does not shape complex scripts. HarfBuzz determines glyph
            // clusters and Skia supplies the font's actual paths; /Contents
            // retains the Unicode text. Never emit blank/tofu text objects.
            std::string fonts;
            { std::lock_guard<std::mutex> guard(fontDirectoryMutex);
              fonts = exportFontDirectory; }
            if (fonts.empty()) fonts = "assets/fonts";
            return appendShapedAnnotationText(a.get(), label, fonts,
                rect.left + rw * 0.04f, rect.bottom + (rh + size) * 0.5f,
                size, mark.cr, mark.cg, mark.cb, alpha, error);
#else
            return fail("No Unicode shaping font available for PDF text export");
#endif
        }
        // Base-14 fonts do not contain every Unicode script. A /Contents
        // value alone is not a substitute for a visible label: fail the
        // export rather than silently dropping unsupported glyphs.
        FPDF_FONT font = FPDFText_LoadStandardFont(doc, mark.fontName.c_str());
        if (!font) return fail("could not load PDF text font");
        for (std::size_t i = 0; i + 1 < utf16.size(); ++i) {
            std::uint32_t cp = utf16[i];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 2 < utf16.size()) {
                cp = 0x10000 + ((cp - 0xD800) << 10) +
                     (static_cast<std::uint32_t>(utf16[++i]) - 0xDC00);
            }
            if (cp == '\n' || cp == '\r' || cp == '\t') continue;
            // PDFium reports the .notdef fallback as having a width; that
            // does NOT mean the base-14 font contains the character. Limit
            // to WinAnsi's actual repertoire, including its punctuation.
            const bool winAnsi = (cp >= 32 && cp <= 126) ||
                (cp >= 0xA0 && cp <= 0xFF) ||
                cp == 0x20AC || cp == 0x2013 || cp == 0x2014 ||
                (cp >= 0x2018 && cp <= 0x201E) || cp == 0x2022 ||
                cp == 0x2026 || cp == 0x2122;
            if (!winAnsi) {
                FPDFFont_Close(font);
                return fail("PDF font cannot show this script; no PDF was exported");
            }
            float width = 0.f;
            if (!FPDFFont_GetGlyphWidth(font, cp, size, &width)) {
                FPDFFont_Close(font);
                return fail("PDF font lacks a text glyph; no PDF was exported");
            }
        }
        auto obj = FPDFPageObj_CreateTextObj(doc, font, size);
        FPDFFont_Close(font);
        if (!obj) return fail("could not create PDF text object");
        if (!FPDFText_SetText(obj, utf16.data()) ||
            !FPDFPageObj_SetFillColor(obj, mark.cr, mark.cg, mark.cb, alpha)) {
            FPDFPageObj_Destroy(obj);
            return fail("could not set PDF annotation text");
        }
        FPDFPageObj_Transform(obj, 1, 0, 0, 1, rect.left + rw * 0.04f,
                              rect.bottom + (rh - size) * 0.5f);
        if (!FPDFAnnot_AppendObject(a.get(), obj)) {
            FPDFPageObj_Destroy(obj);
            return fail("could not create text appearance");
        }
        return true; // AppendObject generated a font-backed appearance.
    }
    // A /Rect and /QuadPoints without /AP are valid metadata but invisible
    // in many PDF readers. PDFium creates a Form XObject with /BBox = rect;
    // its coordinates are absolute PDF user-space coordinates, not local.
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(5);
    stream << "q ";
    if (alpha < 255u) stream << "/GS gs ";  // PDFium adds /GS when /CA < 1
    stream << mark.cr / 255.f << ' ' << mark.cg / 255.f << ' '
           << mark.cb / 255.f;
    if (mark.kind == K::Highlight || mark.kind == K::Note) {
        stream << " rg " << rect.left << ' ' << rect.bottom << ' '
               << rw << ' ' << rh << " re f ";
    } else {
        stream << " RG ";
        const float line = std::max(0.4f, mark.kind == K::Underline ||
                 mark.kind == K::StrikeOut ? rh * 0.055f : mark.width);
        stream << line << " w 1 J 1 j ";
        if (mark.kind == K::Underline || mark.kind == K::StrikeOut) {
            const float y = rect.bottom + rh * (mark.kind == K::Underline ? 0.08f : 0.42f);
            stream << rect.left << ' ' << y << " m " << rect.right << ' '
                   << y << " l S ";
        } else if (mark.kind == K::Box) {
            stream << rect.left + line * 0.5f << ' ' << rect.bottom + line * 0.5f
                   << ' ' << std::max(0.1f, rw - line) << ' '
                   << std::max(0.1f, rh - line) << " re S ";
        } else if (mark.kind == K::Ellipse) {
            const float cx = (rect.left + rect.right) * 0.5f;
            const float cy = (rect.top + rect.bottom) * 0.5f;
            const float rx = std::max(0.1f, (rw - line) * 0.5f);
            const float ry = std::max(0.1f, (rh - line) * 0.5f);
            const float k = 0.55228475f;
            stream << cx + rx << ' ' << cy << " m "
                   << cx + rx << ' ' << cy + k * ry << ' ' << cx + k * rx << ' '
                   << cy + ry << ' ' << cx << ' ' << cy + ry << " c "
                   << cx - k * rx << ' ' << cy + ry << ' ' << cx - rx << ' '
                   << cy + k * ry << ' ' << cx - rx << ' ' << cy << " c "
                   << cx - rx << ' ' << cy - k * ry << ' ' << cx - k * rx << ' '
                   << cy - ry << ' ' << cx << ' ' << cy - ry << " c "
                   << cx + k * rx << ' ' << cy - ry << ' ' << cx + rx << ' '
                   << cy - k * ry << ' ' << cx + rx << ' ' << cy << " c S ";
        } else if (mark.kind == K::Ink) {
            for (std::size_t i = 0; i < mark.points.size(); ++i) {
                auto p = point(page, mark.points[i][0], mark.points[i][1]);
                stream << p.x << ' ' << p.y << (i == 0 ? " m " : " l ");
            }
            stream << "S ";
        }
    }
    stream << "Q";
    if (!FPDFAnnot_SetAP(a.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                         wide(stream.str()).data()))
        return fail("could not create annotation appearance");
    return true;
}
}  // namespace

void setPdfiumExportFontDirectory(std::string directory) {
    std::lock_guard<std::mutex> guard(fontDirectoryMutex);
    exportFontDirectory = std::move(directory);
}

bool writePdfiumAnnotations(const std::string& source, const std::string& staging,
                            const std::vector<AnnotSpec>& marks, std::string& error) {
    error.clear();
    std::error_code ec;
    const auto nativeStaging = std::filesystem::path(
        std::u8string(staging.begin(), staging.end()));
    std::filesystem::remove(nativeStaging, ec);
    // Called from PdfiumWorker only: PDFium is not thread-safe.
    ensurePdfiumInitialised();
    FileMappedAccess input(source);
    if (!input.valid()) { error = "Source PDF is unavailable; relink it to export"; return false; }
    Doc doc(FPDF_LoadCustomDocument(input.handle(), nullptr));
    if (!doc) { error = PdfiumLibrary::describeError(FPDF_GetLastError()); return false; }
    const int count = FPDF_GetPageCount(doc.get());
    // Validate ALL imported targets before mutating the export document.
    // Their index is in the untouched source page's /Annots array. Deleting
    // in descending order prevents earlier indices from shifting. Never
    // touch unsupported/unselected PDF annotations, or the original file.
    std::map<int, std::map<int, std::pair<int, std::uint64_t>, std::greater<int>>> replaced;
    for (const auto& mark : marks) {
        if (mark.sourceIndex < 0) continue;
        if (mark.page < 0 || mark.page >= count || mark.sourceIndex >= 5000 ||
            mark.sourceSubtype <= 0 || !mark.sourceFingerprint) { error = "invalid imported annotation reference"; return false; }
        const auto [it, inserted] = replaced[mark.page].emplace(mark.sourceIndex,
            std::pair{mark.sourceSubtype, mark.sourceFingerprint});
        if (!inserted) { error = "duplicate imported annotation edit"; return false; }
    }
    for (const auto& [pageNum, refs] : replaced) {
        Page pg(FPDF_LoadPage(doc.get(), pageNum));
        if (!pg) { error = "could not load imported annotation page"; return false; }
        for (const auto& [index, identity] : refs) {
            const auto [type, fingerprint] = identity;
            if (index >= FPDFPage_GetAnnotCount(pg.get())) {
                error = "imported annotation index changed; no PDF exported"; return false;
            }
            Annot original(FPDFPage_GetAnnot(pg.get(), index));
            if (!original || static_cast<int>(FPDFAnnot_GetSubtype(original.get())) != type ||
                pdfAnnotationFingerprint(original.get()) != fingerprint) {
                error = "imported annotation identity changed; no PDF exported"; return false;
            }
            original.reset();
            if (!FPDFPage_RemoveAnnot(pg.get(), index)) {
                error = "could not replace imported annotation"; return false;
            }
        }
    }
    for (const auto& mark : marks) {
        if (mark.page < 0 || mark.page >= count) {
            error = "annotation page is outside this PDF"; return false;
        }
        Page page(FPDF_LoadPage(doc.get(), mark.page));
        if (!page) { error = "could not load annotated page"; return false; }
        if (!mark.deleted && !add(doc.get(), page.get(), mark, error)) return false;
    }
#if defined(_WIN32)
    std::FILE* f = _wfopen(nativeStaging.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(staging.c_str(), "wb");
#endif
    if (!f) { error = "cannot create private PDF staging file"; return false; }
    Output output(f);
    const bool saved = FPDF_SaveAsCopy(doc.get(), &output, FPDF_NO_INCREMENTAL);
    const bool flushed = std::fflush(f) == 0;
    const bool closed = std::fclose(f) == 0;
    if (!saved || output.failed || !flushed || !closed) {
        std::filesystem::remove(nativeStaging, ec);
        error = "PDF writer or disk write failed";
        return false;
    }
    // Validate staged PDF before ever giving its path to a document provider.
    Doc verify(FPDF_LoadDocument(staging.c_str(), nullptr));
    if (!verify || FPDF_GetPageCount(verify.get()) != count) {
        std::filesystem::remove(nativeStaging, ec);
        error = "saved PDF failed to reopen";
        return false;
    }
    return true;
}
}  // namespace folio::engine
