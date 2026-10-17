#pragma once
#include <fpdf_annot.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace folio::engine {
// Stable across process restarts for the same source PDF. This is a guard
// against overwriting an unrelated annotation when /Annots was reordered or
// a desktop PDF changed under an existing Folio sidecar. Android relink adds
// a stronger whole-file SHA-256 match before the source is used at all.
inline std::uint64_t pdfAnnotationFingerprint(FPDF_ANNOTATION annot) {
    if (!annot) return 0;
    std::uint64_t hash = 14695981039346656037ull;
    const auto mix = [&](const void* bytes, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(bytes);
        for (std::size_t i = 0; i < size; ++i) { hash ^= p[i]; hash *= 1099511628211ull; }
    };
    const int subtype = static_cast<int>(FPDFAnnot_GetSubtype(annot));
    mix(&subtype, sizeof(subtype));
    FS_RECTF rect{};
    if (!FPDFAnnot_GetRect(annot, &rect)) return 0;
    mix(&rect.left, sizeof(rect.left)); mix(&rect.top, sizeof(rect.top));
    mix(&rect.right, sizeof(rect.right)); mix(&rect.bottom, sizeof(rect.bottom));
    const unsigned long size = FPDFAnnot_GetStringValue(annot, "Contents", nullptr, 0);
    if (size > 8192 || size % 2) return 0;
    if (size > 2) {
        std::vector<FPDF_WCHAR> text(size / 2);
        if (FPDFAnnot_GetStringValue(annot, "Contents", text.data(), size) != size) return 0;
        mix(text.data(), size);
    }
    return hash ? hash : 1;
}
} // namespace folio::engine
