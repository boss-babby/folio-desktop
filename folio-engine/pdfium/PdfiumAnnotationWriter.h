#pragma once
#include "AnnotationSpec.h"
#include <string>
#include <vector>

namespace folio::engine {
void setPdfiumExportFontDirectory(std::string directory);
// Call ONLY on the PDFium worker thread. Never modify the source document:
// a fresh instance is loaded and a complete copy is written to a private
// staging file before the Android document provider is asked to publish it.
// On any error the staging file is removed, and `error` names the failure.
bool writePdfiumAnnotations(const std::string& source, const std::string& staging,
                            const std::vector<AnnotSpec>& marks,
                            std::string& error);
}  // namespace folio::engine
