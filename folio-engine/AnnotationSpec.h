#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace folio::engine {

/// An annotation to write, in the UI's coordinate space: 0..1 page-normalised
/// with a top-left origin. Converting to PDF user space - bottom-left origin,
/// points, offset by the MediaBox, possibly rotated - is this class's job and
/// nobody else's.
struct AnnotSpec {
    enum class Kind : std::uint8_t {
        Highlight, Underline, StrikeOut, Ink, Note, Box, Ellipse, FreeText, Stamp
    };

    Kind  kind = Kind::Highlight;
    int sourceIndex = -1; // original PDF /Annots index, -1 for new Folio mark
    int sourceSubtype = 0; // subtype guard against replacing unrelated marks
    std::uint64_t sourceFingerprint = 0;
    bool deleted = false; // imported tombstone: remove only, do not recreate
    /// Glyph height in PDF points, for FreeText only. Zero means "derive
    /// it from the box", which is what the UI does when it sizes text to
    /// the annotation rectangle.
    float fontSize = 0.f;
    /// One of the base-14 families. Named rather than enumerated because
    /// that is what goes into the PDF, and because the set the viewer is
    /// required to provide is fixed: Helvetica, Times-Roman, Courier.
    std::string fontName = "Helvetica";
    int   page = 0;
    float l = 0.f, t = 0.f, r = 0.f, b = 0.f;
    std::vector<std::array<float, 2>> points;  // Ink only
    std::uint8_t cr = 255, cg = 214, cb = 10;
    float opacity = 0.4f;
    float width = 2.f;                         // stroke width in points
    std::string contents;
    std::string label; // visible text for FreeText/Stamp; separate from comment
    std::string author;
};

}  // namespace folio::engine
