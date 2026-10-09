// folio-ui/shell/main_stream.cpp
//
// Entry point for the streaming shell.
//
//   folio-stream [--port N] [--size WxH] [--dpr F] [--dark]

#include "shell/StreamShell.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

folio::shell::StreamShell* g_shell = nullptr;

void onSignal(int) {
    if (g_shell) g_shell->stop();
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8080;
    int w = 1440, h = 900;
    float dpr = 1.f;
    bool dark = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--port") {
            if (const char* v = next()) port = std::atoi(v);
        } else if (a == "--size") {
            if (const char* v = next()) {
                if (const char* x = std::strchr(v, 'x')) {
                    w = std::atoi(v);
                    h = std::atoi(x + 1);
                }
            }
        } else if (a == "--dpr") {
            if (const char* v = next()) dpr = static_cast<float>(std::atof(v));
        } else if (a == "--dark") {
            dark = true;
        } else if (a == "--help" || a == "-h") {
            std::printf("usage: folio-stream [--port N] [--size WxH] [--dpr F] [--dark]\n");
            return 0;
        }
    }

    folio::shell::StreamShell shell(w, h, dpr);
    g_shell = &shell;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    if (dark) {
        shell.app().state().theme.setSchemeImmediate(folio::ui::ColorScheme::Dark);
    }

    if (!shell.start(static_cast<std::uint16_t>(port))) {
        std::fprintf(stderr, "[folio] could not bind port %d\n", port);
        return 1;
    }
    std::printf("[folio] streaming shell on http://0.0.0.0:%d  (%dx%d @%.1fx)\n", port, w,
                h, static_cast<double>(dpr));
    std::fflush(stdout);
    shell.run();
    std::printf("[folio] stopped\n");
    return 0;
}
