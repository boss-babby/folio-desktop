// folio-engine/pdfium/PdfiumLibrary.cpp  --  work package B2

#include "PdfiumLibrary.h"

#include <fpdfview.h>

namespace folio::engine {

PdfiumLibrary& PdfiumLibrary::instance() {
    static PdfiumLibrary lib;
    return lib;
}

PdfiumLibrary::PdfiumLibrary() {
    FPDF_LIBRARY_CONFIG config{};
    config.version          = 2;
    config.m_pUserFontPaths = nullptr;   // see the font policy note below
    config.m_pIsolate       = nullptr;   // no V8: no document JavaScript
    config.m_v8EmbedderSlot = 0;

    // FONT POLICY
    // -----------
    // m_pUserFontPaths = nullptr makes PDFium fall back to the platform's
    // font enumeration for non-embedded fonts. That is the single largest
    // source of "the same PDF looks different on Linux and macOS", because
    // the substitution for an unembedded Helvetica differs per OS.
    //
    // The golden-image gate (plan §16.1) therefore runs with a FIXED font
    // directory supplied here, so CI compares like with like. Shipping
    // builds keep the platform fallback, because a user opening a local
    // document expects their system fonts. Both behaviours are correct; the
    // bug is not noticing they are different.

    FPDF_InitLibraryWithConfig(&config);

    initThread_ = std::this_thread::get_id();
    version_    = "PDFium (bblanchon prebuilt, see tools/fetch-deps.sh for the pin)";
}

PdfiumLibrary::~PdfiumLibrary() { FPDF_DestroyLibrary(); }

const char* PdfiumLibrary::describeError(unsigned long code) noexcept {
    switch (code) {
        case FPDF_ERR_SUCCESS:  return "no error";
        case FPDF_ERR_UNKNOWN:  return "unknown error";
        case FPDF_ERR_FILE:     return "file not found or cannot be opened";
        case FPDF_ERR_FORMAT:   return "not a PDF, or the file is corrupt";
        case FPDF_ERR_PASSWORD: return "a password is required, or it was wrong";
        case FPDF_ERR_SECURITY: return "unsupported security scheme";
        case FPDF_ERR_PAGE:     return "page not found or content error";
        default:                return "unrecognised PDFium error code";
    }
}

}  // namespace folio::engine
