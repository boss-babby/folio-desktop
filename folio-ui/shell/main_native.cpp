// folio-ui/shell/main_native.cpp
//
// Argument parsing for the desktop binary. Everything else is in
// NativeShell.cpp, which is platform-independent.

#include "NativeShell.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    folio::ui::NativeShellOptions opts;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--software") {
            opts.forceSoftware = true;
        } else if (a == "--size" && i + 1 < argc) {
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                opts.width = w;
                opts.height = h;
            }
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "folio [options] [file.pdf]\n"
                "  --size WxH    initial window size (default 1440x900)\n"
                "  --software    render on the CPU instead of the GPU\n");
            return 0;
        } else if (!a.empty() && a[0] != '-') {
            opts.openPath = a;
        }
    }

    return folio::ui::runNativeShell(opts);
}
