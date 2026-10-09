// folio-ui/shell/NativeShell.h
//
// The desktop shell entry point. One implementation for Windows, macOS and
// Linux; the differences all live behind folio-platform.

#pragma once

#include <string>

namespace folio::ui {

struct NativeShellOptions {
    int width = 1440;
    int height = 900;
    /// Skip Ganesh and render on the CPU. Useful for bisecting a GPU bug and
    /// for machines whose drivers cannot give us a GL 3.3 context.
    bool forceSoftware = false;
    /// Open this document at startup, if non-empty.
    std::string openPath;
    /// Where fonts live. Empty means "work it out from the executable",
    /// which is right on every desktop. Android has no executable to be
    /// beside and no readable assets directory, so its entry point unpacks
    /// the fonts and passes the destination here.
    std::string assetDir;
};

/// Runs until the window closes. Returns a process exit code.
int runNativeShell(const NativeShellOptions& opts);

}  // namespace folio::ui
