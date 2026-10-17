// folio-engine/pdfium/PdfiumLibrary.h  --  work package B2
//
// Process-wide PDFium initialisation.
//
// FPDF_InitLibraryWithConfig touches global state (the font cache, the codec
// registry, optionally a V8 isolate) and FPDF_DestroyLibrary tears it down.
// Calling either twice, or calling any FPDF_ function outside the pair, is
// undefined behaviour that usually manifests as a crash in a codec rather
// than anywhere near the actual mistake.
//
// So: one Meyers singleton, constructed on first use, destroyed at exit.
//
// THE V8 / XFA DECISION IS MADE HERE
// ----------------------------------
// config.version = 3 and a null m_pIsolate means "no JavaScript". Folio ships
// that way on every platform by default (FOLIO_PDFIUM_V8=OFF). It removes
// ~20 MB of binary, and removes document-embedded JavaScript as an attack
// surface in an application whose whole job is opening untrusted files from
// the internet. Turning it on is a product decision with a security review
// attached, not a build flag someone flips to fix a form.

#pragma once

#include <mutex>
#include <string>
#include <thread>

namespace folio::engine {

class PdfiumLibrary {
public:
    static PdfiumLibrary& instance();

    PdfiumLibrary(const PdfiumLibrary&) = delete;
    PdfiumLibrary& operator=(const PdfiumLibrary&) = delete;

    /// Human-readable build identity, for the About box and for bug reports.
    /// Knowing the exact PDFium revision that produced a wrong rendering is
    /// the difference between a reproducible bug and a shrug.
    [[nodiscard]] const std::string& version() const noexcept { return version_; }

    /// Translate FPDF_GetLastError() into something a user can act on.
    [[nodiscard]] static const char* describeError(unsigned long code) noexcept;

private:
    PdfiumLibrary();
    ~PdfiumLibrary();

    std::string     version_;
    std::thread::id initThread_;
};

/// PDFium's font, codec and page-object caches are process-wide. Even when
/// separate tabs own different documents, their API calls must not overlap.
/// Keep the gate outside a document/worker so standalone rasterizers and
/// background tab workers share the same protection. Recursive because
/// pageInfo/render operations call acquirePage and other guarded helpers.
[[nodiscard]] std::recursive_mutex& pdfiumApiMutex();

/// Call before any FPDF_ use. Cheap after the first time.
inline void ensurePdfiumInitialised() { (void)PdfiumLibrary::instance(); }

}  // namespace folio::engine
