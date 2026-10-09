// folio-platform/src/PlatformNone.cpp
//
// The backend for machines with no display server: CI containers, Docker
// builds, this sandbox.
//
// It exists so that `folio-platform` and everything above it compiles and
// links on any host. A build that silently omits the platform layer cannot
// tell you whether the platform layer still compiles, and the whole point of
// turning the UI build on in CI is to find that out on every commit.

#include "folio/platform/Platform.h"

namespace folio::platform {
namespace {

class NullClipboard final : public IClipboard {
public:
    bool setText(std::string_view) override { return false; }
    [[nodiscard]] std::string text() override { return {}; }
};

class NullFileDialog final : public IFileDialog {
public:
    [[nodiscard]] bool available() const override { return false; }
    [[nodiscard]] std::optional<std::string> openFile(
        const FileDialogRequest&) override {
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string> saveFile(
        const FileDialogRequest&) override {
        return std::nullopt;
    }
};

}  // namespace

std::unique_ptr<IWindow> createWindow(const WindowDesc&, std::string* error) {
    if (error) {
        *error =
            "this build has no windowing backend (no X11/EGL headers were "
            "found at configure time)";
    }
    return nullptr;
}

IClipboard& clipboard() {
    static NullClipboard c;
    return c;
}

IFileDialog& fileDialog() {
    static NullFileDialog d;
    return d;
}

const char* backendName() noexcept { return "none"; }

}  // namespace folio::platform
