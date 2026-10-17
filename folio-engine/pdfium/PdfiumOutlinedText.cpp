#include "pdfium/PdfiumOutlinedText.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPath.h"
#include "include/ports/SkFontMgr_empty.h"
#include <fpdf_edit.h>
#include <hb.h>
#include <hb-ot.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

namespace folio::engine {
namespace {
struct HbBlob { void operator()(hb_blob_t* x) const { hb_blob_destroy(x); } };
struct HbFace { void operator()(hb_face_t* x) const { hb_face_destroy(x); } };
struct HbFont { void operator()(hb_font_t* x) const { hb_font_destroy(x); } };
struct HbBuffer { void operator()(hb_buffer_t* x) const { hb_buffer_destroy(x); } };
using Blob = std::unique_ptr<hb_blob_t, HbBlob>;
using Face = std::unique_ptr<hb_face_t, HbFace>;
using Font = std::unique_ptr<hb_font_t, HbFont>;
using Buffer = std::unique_ptr<hb_buffer_t, HbBuffer>;

bool covers(hb_font_t* font, const std::string& utf8) {
    Buffer buffer(hb_buffer_create());
    hb_buffer_add_utf8(buffer.get(), utf8.c_str(), static_cast<int>(utf8.size()), 0,
                       static_cast<int>(utf8.size()));
    unsigned count = 0;
    const auto* cps = hb_buffer_get_glyph_infos(buffer.get(), &count);
    // Before shaping, hb_buffer glyph info contains Unicode codepoints.
    for (unsigned i = 0; i < count; ++i) {
        const auto c = cps[i].codepoint;
        if (c < 32 || c == 0x7f || c == 0x200c || c == 0x200d) continue;
        hb_codepoint_t glyph = 0;
        if (!hb_font_get_nominal_glyph(font, c, &glyph) || !glyph) return false;
    }
    return true;
}

struct ShapedFont {
    sk_sp<SkTypeface> typeface;
    Font font;
};

ShapedFont chooseFont(const std::string& dir, const std::string& text) {
    auto manager = SkFontMgr_New_Custom_Empty();
    std::vector<std::filesystem::path> candidates;
    std::error_code ec;
    const auto root = std::filesystem::path(std::u8string(dir.begin(), dir.end()));
    for (const char* name : {"NotoSansBengali-Variable.ttf", "DMSans-Variable.ttf"})
        candidates.push_back(root / name);
    // User-installed fonts in the asset font folder can extend export to
    // other scripts without claiming that every Unicode script is bundled.
    if (std::filesystem::is_directory(root, ec))
        for (const auto& item : std::filesystem::directory_iterator(root, ec)) {
            if (candidates.size() > 128) break;
            if (item.is_regular_file(ec) &&
                (item.path().extension() == ".ttf" || item.path().extension() == ".otf"))
                candidates.push_back(item.path());
        }
    for (const auto& path : candidates) {
        auto data = SkData::MakeFromFileName(path.string().c_str());
        if (!data || data->size() == 0 || data->size() > 32 * 1024 * 1024) continue;
        auto typeface = manager->makeFromData(data);
        if (!typeface) continue;
        Blob blob(hb_blob_create(reinterpret_cast<const char*>(data->data()),
                                 static_cast<unsigned>(data->size()),
                                 HB_MEMORY_MODE_READONLY, nullptr, nullptr));
        Face face(hb_face_create(blob.get(), 0));
        Font font(hb_font_create(face.get()));
        hb_ot_font_set_funcs(font.get());
        if (covers(font.get(), text)) return {std::move(typeface), std::move(font)};
    }
    return {};
}

bool pathObject(FPDF_ANNOTATION annotation, const SkPath& path,
                float originX, float baseline, unsigned r, unsigned g, unsigned b,
                unsigned alpha) {
    SkPath::Iter iter(path, false);
    SkPoint p[4];
    FPDF_PAGEOBJECT obj = nullptr;
    auto xy = [&](const SkPoint& pt) { return SkPoint::Make(originX + pt.x(), baseline - pt.y()); };
    auto move = [&](const SkPoint& pt) {
        auto v = xy(pt);
        if (!obj) obj = FPDFPageObj_CreateNewPath(v.x(), v.y());
        else FPDFPath_MoveTo(obj, v.x(), v.y());
    };
    auto cubic = [&](const SkPoint& a, const SkPoint& c, const SkPoint& d) {
        const auto x = xy(a), y = xy(c), z = xy(d);
        return FPDFPath_BezierTo(obj, x.x(), x.y(), y.x(), y.y(), z.x(), z.y()) != 0;
    };
    for (;;) {
        const auto verb = iter.next(p);
        if (verb == SkPath::kDone_Verb) break;
        if (verb == SkPath::kMove_Verb) move(p[0]);
        else if (verb == SkPath::kLine_Verb) {
            if (!obj) return false;
            const auto v = xy(p[1]);
            if (!FPDFPath_LineTo(obj, v.x(), v.y())) break;
        } else if (verb == SkPath::kQuad_Verb) {
            if (!obj || !cubic(p[0] + (p[1] - p[0]) * (2.f/3.f),
                               p[2] + (p[1] - p[2]) * (2.f/3.f), p[2])) break;
        } else if (verb == SkPath::kConic_Verb) {
            if (!obj) break;
            SkPoint quads[5];
            const int n = SkPath::ConvertConicToQuads(p[0], p[1], p[2],
                                                       iter.conicWeight(), quads, 1);
            bool ok = true;
            for (int i = 0; i < n; ++i) {
                const auto& a = quads[2*i], &control = quads[2*i+1], &end = quads[2*i+2];
                ok &= cubic(a + (control-a)*(2.f/3.f),
                            end + (control-end)*(2.f/3.f), end);
            }
            if (!ok) break;
        } else if (verb == SkPath::kCubic_Verb) {
            if (!obj || !cubic(p[1], p[2], p[3])) break;
        } else if (verb == SkPath::kClose_Verb && obj) FPDFPath_Close(obj);
    }
    if (!obj) return true; // whitespace glyph
    if (!FPDFPath_SetDrawMode(obj, FPDF_FILLMODE_WINDING, 0) ||
        !FPDFPageObj_SetFillColor(obj, r, g, b, alpha) ||
        !FPDFAnnot_AppendObject(annotation, obj)) {
        FPDFPageObj_Destroy(obj);
        return false;
    }
    return true;
}
} // namespace

bool appendShapedAnnotationText(FPDF_ANNOTATION annotation, const std::string& label,
                                const std::string& fontDirectory, float left, float baseline,
                                float size, unsigned r, unsigned g, unsigned b,
                                unsigned alpha, std::string& error) {
    if (label.size() > 4096) { error = "text annotation is too long"; return false; }
    auto chosen = chooseFont(fontDirectory, label);
    if (!chosen.typeface) {
        error = "No font covers this script; no PDF was exported";
        return false;
    }
    const int scale = std::max(64, static_cast<int>(std::lround(size * 64.f)));
    hb_font_set_scale(chosen.font.get(), scale, scale);
    Buffer buffer(hb_buffer_create());
    hb_buffer_add_utf8(buffer.get(), label.c_str(), static_cast<int>(label.size()),
                       0, static_cast<int>(label.size()));
    hb_buffer_guess_segment_properties(buffer.get());
    hb_shape(chosen.font.get(), buffer.get(), nullptr, 0);
    unsigned count = 0;
    const auto* glyphs = hb_buffer_get_glyph_infos(buffer.get(), &count);
    const auto* positions = hb_buffer_get_glyph_positions(buffer.get(), &count);
    SkFont skfont(chosen.typeface, size);
    skfont.setEdging(SkFont::Edging::kAlias);
    float x = left, y = baseline;
    for (unsigned i = 0; i < count; ++i) {
        if (!glyphs[i].codepoint || glyphs[i].codepoint > std::numeric_limits<SkGlyphID>::max()) {
            error = "Font shaping produced a missing glyph; no PDF was exported";
            return false;
        }
        SkPath path;
        if (skfont.getPath(static_cast<SkGlyphID>(glyphs[i].codepoint), &path) &&
            !pathObject(annotation, path, x + static_cast<float>(positions[i].x_offset) / 64.f,
                        y + static_cast<float>(positions[i].y_offset) / 64.f, r, g, b, alpha)) {
            error = "Could not write shaped PDF text appearance";
            return false;
        }
        x += static_cast<float>(positions[i].x_advance) / 64.f;
        y += static_cast<float>(positions[i].y_advance) / 64.f;
    }
    return true;
}
} // namespace folio::engine
