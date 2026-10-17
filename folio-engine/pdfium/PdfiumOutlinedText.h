#pragma once
#include <fpdf_annot.h>
#include <string>
namespace folio::engine {
// Shape Unicode with HarfBuzz and paint the resulting font outlines into a
// Stamp appearance. /Contents remains UTF-16 text for readers and comments.
// On missing glyphs fail rather than creating a .notdef/tofu appearance.
bool appendShapedAnnotationText(FPDF_ANNOTATION annotation, const std::string& label,
                                const std::string& fontDirectory, float left, float baseline,
                                float fontSize, unsigned r, unsigned g, unsigned b,
                                unsigned alpha, std::string& error);
}
