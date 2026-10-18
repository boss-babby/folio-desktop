#include "screens/ReaderScreen.h"
#include "pdfium/PdfiumLibrary.h"
#include <fpdfview.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
std::vector<char> read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "folio-reader-saveas-test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path source = dir / "input.pdf";
    const fs::path output = dir / "copy.pdf";
    fs::copy_file("folio-test.pdf", source, fs::copy_options::overwrite_existing, ec);
    fs::remove(output, ec);
    const auto original = read(source);
    if (original.empty()) {
        std::cerr << "Could not stage folio-test.pdf in " << dir << "\n";
        return 1;
    }

    {
    folio::ui::AppState state;
    folio::ui::ReaderScreen reader(state);
    if (!reader.openFile(source.string(), 0.0)) {
        std::cerr << "Reader could not open staged input.pdf\n";
        return 2;
    }
    std::string suggested;
    reader.onPickSavePath = [&](const std::string& name,
                                std::function<void(std::string)> done) {
        suggested = name;
        done(output.string());
    };
    if (!reader.saveDocument() || suggested != "input-annotated.pdf") {
        std::cerr << "Save As did not request the expected filename; got '"
                  << suggested << "'\n";
        return 3;
    }
    bool done = false;
    for (int i = 0; i < 1200; ++i) { // allow a busy CI runner up to 12 s
        // pollExport() also returns true when unrelated page metadata, notes
        // or thumbnail results change the UI. It is NOT an export-complete
        // signal; on a fast Windows runner that change can arrive first.
        reader.pollExport();
        // Save As publishes only by renaming a fully written, verified PDF
        // into this destination, so its existence is the completion signal.
        if (fs::exists(output)) { done = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done || read(source) != original) {
        std::cerr << "Save As did not publish a PDF within 12 s, or modified its input\n";
        return 4;
    }
    folio::engine::ensurePdfiumInitialised();
    auto pdf = FPDF_LoadDocument(output.string().c_str(), nullptr);
    if (!pdf || FPDF_GetPageCount(pdf) < 1) return 5;
    FPDF_CloseDocument(pdf);

    // Save As must not overwrite an existing destination or its open input.
    const auto exported = read(output);
    reader.saveDocument();
    if (read(output) != exported) return 6;
    reader.onPickSavePath = [&](const std::string&,
                                std::function<void(std::string)> next) {
        next(source.string());
    };
    reader.saveDocument();
    if (read(source) != original) return 7;
    } // close PDFium's file handles before removing files on Windows
    fs::remove_all(dir, ec);
    std::cout << "Reader-level PDF Save As: source unchanged, copy valid, overwrites refused\n";
    return 0;
}
