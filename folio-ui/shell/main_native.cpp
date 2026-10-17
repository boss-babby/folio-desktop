// Native desktop entry point. Windows is linked as a GUI-subsystem binary:
// its Unicode entry point does not create an accompanying CMD window.
#include "NativeShell.h"

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <shellapi.h>
#endif

namespace {
int run(std::vector<std::string> args) {
    folio::ui::NativeShellOptions opts;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--software") {
            opts.forceSoftware = true;
        } else if (a == "--size" && i + 1 < args.size()) {
            int w = 0, h = 0;
            if (std::sscanf(args[++i].c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                opts.width = w;
                opts.height = h;
            }
        } else if (a == "--help" || a == "-h") {
#if defined(_WIN32)
            // CI smoke tests invoke --help; never block an unattended runner
            // behind an informational message box.
            if (GetEnvironmentVariableW(L"CI", nullptr, 0) == 0)
                MessageBoxW(nullptr, L"folio [options] [file.pdf]\n"
                                      L"--size WxH: initial window size\n"
                                      L"--software: disable GPU rendering",
                            L"Folio options", MB_OK | MB_ICONINFORMATION);
#else
            std::printf("folio [options] [file.pdf]\n"
                        "  --size WxH    initial window size (default 1440x900)\n"
                        "  --software    render on the CPU instead of the GPU\n");
#endif
            return 0;
        } else if (!a.empty() && a[0] != '-') {
            opts.openPath = a;
        }
    }
    return folio::ui::runNativeShell(opts);
}
}  // namespace

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                          argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0) { LocalFree(argv); return 1; }
        std::string s(static_cast<std::size_t>(n), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                 argv[i], -1, s.data(), n, nullptr, nullptr)) {
            LocalFree(argv); return 1;
        }
        s.pop_back(); // remove the terminating NUL, not a UTF-8 character
        args.push_back(std::move(s));
    }
    LocalFree(argv);
    return run(std::move(args));
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    return run(std::move(args));
}
#endif
