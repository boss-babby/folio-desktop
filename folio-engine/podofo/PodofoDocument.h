// folio-engine/podofo/PodofoDocument.h
//
// The writing half of the engine. PDFium renders; PoDoFo edits.
//
// WHY TWO LIBRARIES
// -----------------
// PDFium is a renderer with a document model bolted on: it will rasterise
// anything, but its editing API is narrow and its annotation support is
// aimed at form filling. PoDoFo is the opposite - it has no renderer at all,
// and a complete object model of the file. Folio uses each for the half it
// is good at, which is why the page a user looks at and the page a user
// saves travel through different code.
//
// The seam is the file on disk. PoDoFo never sees a raster; PDFium never
// writes. After a save the document is simply reopened.
//
// APPEARANCE STREAMS ARE NOT OPTIONAL
// -----------------------------------
// A PDF annotation carries both semantics (/Subtype /Highlight, a colour, a
// rectangle) and an appearance stream (/AP) that says what to actually draw.
// The spec lets a viewer synthesise the appearance, and almost none do -
// PDFium included. An annotation written without /AP is valid, round-trips
// through any parser, and is invisible in every reader including this one.
// So every annotation here is drawn into a Form XObject as well as
// described. That is most of the code below.
//
// VERSION
// -------
// Written against PoDoFo 0.9.8, the version Debian ships and the one
// tools/bootstrap-deps.sh fetches. The plan was drafted against 1.x, whose
// API differs substantially (podofo::PdfDocument, SaveUpdate, std::string_view
// throughout). The concepts are identical; the spellings are not.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace PoDoFo {
class PdfMemDocument;
}

namespace folio::engine {

/// An annotation to write, in the UI's coordinate space: 0..1 page-normalised
/// with a top-left origin. Converting to PDF user space - bottom-left origin,
/// points, offset by the MediaBox, possibly rotated - is this class's job and
/// nobody else's.
struct AnnotSpec {
    enum class Kind : std::uint8_t {
        Highlight, Underline, StrikeOut, Ink, Note, Box, Ellipse, FreeText
    };

    Kind  kind = Kind::Highlight;
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
    std::string author;
};

class PodofoDocument {
public:
    PodofoDocument();
    ~PodofoDocument();

    PodofoDocument(const PodofoDocument&) = delete;
    PodofoDocument& operator=(const PodofoDocument&) = delete;

    bool open(const std::string& path);
    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

    [[nodiscard]] int pageCount() const;
    /// Page size in points, after /Rotate - matching what the reader shows.
    [[nodiscard]] std::array<double, 2> pageSize(int index) const;
    [[nodiscard]] int pageRotation(int index) const;

    // --- editing ------------------------------------------------------------

    /// Add `/Rotate delta` to a page, normalised to 0/90/180/270.
    bool rotatePage(int index, int deltaDegrees);
    bool deletePage(int index);
    /// Insert a blank page the same size as its neighbour.
    bool insertBlankPage(int atIndex);
    /// Write `pages` (0-based, any order) to a new document at `outPath`.
    bool extractPages(const std::vector<int>& pages, const std::string& outPath);

    /// Replace Folio's annotations on every page mentioned, leaving
    /// annotations this app did not create untouched.
    bool writeAnnotations(const std::vector<AnnotSpec>& annots);

    // --- saving -------------------------------------------------------------

    /// Append a changed-objects section to the existing file, leaving the
    /// original bytes intact. Cheap, and preserves any signature covering
    /// the earlier revision.
    bool saveIncremental();
    /// Write a fresh, fully rewritten file.
    bool saveAs(const std::string& outPath);

    struct Metadata {
        std::string title, author, subject, creator, producer;
    };
    [[nodiscard]] Metadata metadata() const;
    bool setMetadata(const Metadata& m);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string path_;
    std::string error_;
    bool        dirty_ = false;
};

}  // namespace folio::engine
