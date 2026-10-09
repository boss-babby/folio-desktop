#include "SkiaTextSystem.h"

#include "include/core/SkData.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkPaint.h"
#include "include/core/SkStream.h"
#include "include/core/SkTextBlob.h"
#include "include/ports/SkFontMgr_empty.h"

#if defined(FOLIO_SKIA_HAS_FONTCONFIG)
#include "include/ports/SkFontMgr_fontconfig.h"
#endif

#include <cstdio>
#include <vector>

namespace folio::render::skia {
namespace {

constexpr const char* kFontFile = "DMSans-Variable.ttf";

/// The OpenType variation tag 'wght'.
constexpr SkFourByteTag kWghtTag = SkSetFourByteTag('w', 'g', 'h', 't');

SkColor toSk(const Color& c) {
    return SkColorSetARGB(c.a, c.r, c.g, c.b);
}

/// Number of bytes in the UTF-8 sequence starting at `b`.
int utf8SeqLen(unsigned char b) {
    if (b < 0x80) return 1;
    if ((b >> 5) == 0x6) return 2;
    if ((b >> 4) == 0xE) return 3;
    if ((b >> 3) == 0x1E) return 4;
    return 1;  // malformed; advance one byte so we always terminate
}

}  // namespace

SkiaTextSystem& SkiaTextSystem::instance() {
    static SkiaTextSystem s;
    return s;
}

bool SkiaTextSystem::initialise(const std::string& fontDir) {
    if (initialised_) return bundled_;
    initialised_ = true;

    const std::string path = fontDir + "/" + kFontFile;
    fontData_ = SkData::MakeFromFileName(path.c_str());

    if (fontData_ && fontData_->size() > 0) {
        mgr_ = SkFontMgr_New_Custom_Empty();
        // Probe with a default typeface to confirm the data actually parses.
        auto probe = mgr_->makeFromData(fontData_);
        if (probe) {
            bundled_ = true;
            SkString name;
            probe->getFamilyName(&name);
            familyName_ = name.c_str();
            return true;
        }
        std::fprintf(stderr, "[folio] %s exists but did not parse as a font\n",
                     path.c_str());
    } else {
        std::fprintf(stderr, "[folio] bundled font not found at %s\n", path.c_str());
    }

    // Fallback: system fonts. Layout will NOT match other platforms.
#if defined(FOLIO_SKIA_HAS_FONTCONFIG)
    mgr_ = SkFontMgr_New_FontConfig(nullptr);
#else
    mgr_ = SkFontMgr_New_Custom_Empty();
#endif
    std::fprintf(stderr,
                 "[folio] WARNING: falling back to system fonts; chrome metrics "
                 "will differ between platforms. Run tools/fetch-deps.sh.\n");
    familyName_ = "(system fallback)";
    bundled_ = false;
    return false;
}

/// Serif and mono are not bundled: shipping three variable families would
/// roughly triple the asset payload for something only document content
/// uses. They are loaded from the host instead, from the usual locations on
/// each platform, and quietly fall back to the interface sans when the host
/// has none - a missing serif should downgrade the annotation, not break it.
sk_sp<SkTypeface> SkiaTextSystem::loadFamily(FontFamily f) {
    const auto key = static_cast<std::uint8_t>(f);
    if (auto it = families_.find(key); it != families_.end()) return it->second;

    static const char* kSerif[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Times New Roman.ttf",
        "C:/Windows/Fonts/times.ttf",
    };
    static const char* kMono[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "C:/Windows/Fonts/consola.ttf",
    };
    const char* const* list = (f == FontFamily::Serif) ? kSerif : kMono;
    const std::size_t n = 4;

    sk_sp<SkTypeface> tf;
    for (std::size_t i = 0; i < n && !tf; ++i) {
        auto data = SkData::MakeFromFileName(list[i]);
        if (data && data->size() > 0 && mgr_)
            tf = mgr_->makeFromData(data);
    }
    families_[key] = tf;   // cache the miss too; the disk will not change
    return tf;
}

sk_sp<SkTypeface> SkiaTextSystem::typefaceFor(FontWeight w, FontFamily fam) {
    const auto key = static_cast<std::uint32_t>(w) |
                     (static_cast<std::uint32_t>(fam) << 16);
    if (auto it = typefaces_.find(key); it != typefaces_.end()) return it->second;

    if (fam != FontFamily::Sans) {
        if (sk_sp<SkTypeface> tf = loadFamily(fam)) {
            typefaces_[key] = tf;
            return tf;
        }
        // No such family on this host - fall through to the sans below.
    }

    sk_sp<SkTypeface> tf;
    if (bundled_ && fontData_) {
        // DM Sans ships as a variable font. Pin the weight axis rather than
        // relying on synthetic emboldening, which is visibly worse at 13 px.
        SkFontArguments::VariationPosition::Coordinate coord{
            kWghtTag, static_cast<float>(key)};
        SkFontArguments::VariationPosition pos{&coord, 1};
        SkFontArguments args;
        args.setVariationDesignPosition(pos);
        tf = mgr_->makeFromStream(
            std::make_unique<SkMemoryStream>(fontData_), args);
    }
    if (!tf && mgr_) {
        SkFontStyle style(static_cast<int>(key), SkFontStyle::kNormal_Width,
                          SkFontStyle::kUpright_Slant);
        tf = mgr_->legacyMakeTypeface(nullptr, style);
    }
    typefaces_[key] = tf;
    return tf;
}

const SkFont& SkiaTextSystem::font(float sizePx, FontWeight weight,
                                   FontFamily family) {
    const Key k{static_cast<std::uint32_t>(sizePx * 16.f + 0.5f),
                static_cast<std::uint16_t>(weight),
                static_cast<std::uint8_t>(family)};
    if (auto it = fonts_.find(k); it != fonts_.end()) return it->second;

    SkFont f(typefaceFor(weight, family), sizePx);
    // Subpixel positioning + no hinting is what every modern UI toolkit uses
    // for small text; hinting snaps stems to the pixel grid and makes
    // animated translation of a label judder.
    f.setSubpixel(true);
    f.setHinting(SkFontHinting::kNone);
    f.setEdging(SkFont::Edging::kAntiAlias);
    return fonts_.emplace(k, f).first->second;
}

float SkiaTextSystem::measure(std::string_view utf8, const TextStyle& st) {
    if (utf8.empty()) return 0.f;
    const SkFont& f = font(st.size, st.weight, st.family);
    float w = f.measureText(utf8.data(), utf8.size(), SkTextEncoding::kUTF8);
    if (st.letterSpacingEm != 0.f) {
        const int n = f.countText(utf8.data(), utf8.size(), SkTextEncoding::kUTF8);
        w += st.letterSpacingEm * st.size * static_cast<float>(n);
    }
    return w;
}

FontMetrics SkiaTextSystem::metrics(const TextStyle& st) {
    SkFontMetrics m{};
    font(st.size, st.weight, st.family).getMetrics(&m);
    return FontMetrics{m.fAscent, m.fDescent, m.fLeading};
}

void SkiaTextSystem::draw(SkCanvas* canvas, std::string_view utf8, float x,
                          float baselineY, const TextStyle& st) {
    if (utf8.empty() || !canvas) return;
    const SkFont& f = font(st.size, st.weight, st.family);

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(toSk(st.color));

    if (st.letterSpacingEm == 0.f) {
        canvas->drawSimpleText(utf8.data(), utf8.size(), SkTextEncoding::kUTF8, x,
                               baselineY, f, paint);
        return;
    }

    // Tracking: lay the glyphs out ourselves. This is safe for the Latin
    // chrome strings that use it (only the uppercase section headers), but it
    // is NOT a general shaping path - it would break cursive joining. The
    // interface documents that; anything complex goes through drawSimpleText.
    const int n = f.countText(utf8.data(), utf8.size(), SkTextEncoding::kUTF8);
    if (n <= 0) return;
    std::vector<SkGlyphID> glyphs(static_cast<std::size_t>(n));
    f.textToGlyphs(utf8.data(), utf8.size(), SkTextEncoding::kUTF8, glyphs);
    std::vector<float> widths(static_cast<std::size_t>(n));
    f.getWidths(glyphs, widths);

    const float track = st.letterSpacingEm * st.size;
    std::vector<SkPoint> pos(static_cast<std::size_t>(n));
    float pen = x;
    for (int i = 0; i < n; ++i) {
        pos[static_cast<std::size_t>(i)] = SkPoint{pen, baselineY};
        pen += widths[static_cast<std::size_t>(i)] + track;
    }
    auto blob = SkTextBlob::MakeFromPosText(
        glyphs.data(), glyphs.size() * sizeof(SkGlyphID),
        SkSpan<const SkPoint>(pos.data(), pos.size()), f, SkTextEncoding::kGlyphID);
    if (blob) canvas->drawTextBlob(blob, 0, 0, paint);
}

std::string SkiaTextSystem::elide(std::string_view utf8, const TextStyle& st,
                                  float maxWidth) {
    if (utf8.empty()) return {};
    if (measure(utf8, st) <= maxWidth) return std::string(utf8);

    static constexpr std::string_view kEllipsis = "\xE2\x80\xA6";  // U+2026
    const float ellipsisW = measure(kEllipsis, st);
    if (ellipsisW > maxWidth) return {};

    // Linear scan on code point boundaries. Strings in chrome are short
    // (< 60 chars); a binary search would save microseconds and cost clarity.
    std::size_t best = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const std::size_t step =
            static_cast<std::size_t>(utf8SeqLen(static_cast<unsigned char>(utf8[i])));
        const std::size_t next = std::min(i + step, utf8.size());
        if (measure(utf8.substr(0, next), st) + ellipsisW > maxWidth) break;
        best = next;
        i = next;
    }
    return std::string(utf8.substr(0, best)) + std::string(kEllipsis);
}

}  // namespace folio::render::skia
