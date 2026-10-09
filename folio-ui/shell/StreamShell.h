// folio-ui/shell/StreamShell.h
//
// A platform shell that has no window.
//
// It renders FolioApp into an offscreen Skia surface and serves the frames
// over HTTP as MJPEG, with an input channel coming back the other way. That
// makes the UI visible and interactive on a machine with no display server -
// a CI container, a remote build box, or this sandbox.
//
// This is not a toy. It is the same shape as the SDL3 shell: acquire a
// surface, translate platform input into folio::ui events, pump the app,
// present. The fact that both can exist behind the same FolioApp with no
// #ifdef inside any component is the practical proof that the platform seam
// described in the architecture plan actually holds.
//
// WHAT IT IS NOT
// --------------
// Not the shipping desktop path (that is Sdl3Shell), and the encode cost it
// pays per frame does not exist in a native build. Render time and encode
// time are measured separately for exactly that reason.

#pragma once

#include "screens/FolioApp.h"
#include "shell/FrameEncoder.h"
#include "shell/HttpServer.h"

#include "include/core/SkSurface.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace folio::shell {

struct ShellStats {
    double renderMs = 0.0;   // app paint into the Skia surface
    double encodeMs = 0.0;   // JPEG encode - streaming overhead only
    double frameMs = 0.0;    // wall time between presented frames
    std::uint64_t frames = 0;
    std::size_t lastBytes = 0;
    int clients = 0;
};

class StreamShell {
public:
    StreamShell(int width, int height, float dpr = 1.f);
    ~StreamShell();

    bool start(std::uint16_t port);
    /// Blocks, driving the render loop, until stop() is called.
    void run();
    void stop();

    [[nodiscard]] ui::FolioApp& app() noexcept { return app_; }
    [[nodiscard]] ShellStats stats() const;

private:
    void renderOnce(double now);
    void publish(std::vector<std::uint8_t> jpeg);
    void installRoutes();
    /// Parse one input record from the client and queue it.
    void queueInput(const std::string& record);
    void drainInput(double now);

    int width_, height_;
    float dpr_;
    sk_sp<SkSurface> surface_;
    ui::FolioApp app_;
    HttpServer http_;

    // Latest encoded frame, published to any number of MJPEG readers.
    mutable std::mutex frameMu_;
    std::condition_variable frameCv_;
    std::vector<std::uint8_t> frame_;
    std::uint64_t frameSeq_ = 0;

    // Input queue, filled by HTTP worker threads, drained by the render loop.
    std::mutex inputMu_;
    std::deque<std::string> inputQueue_;

    mutable std::mutex statsMu_;
    ShellStats stats_;
    std::atomic<int> clients_{0};
    std::atomic<bool> running_{false};

    // Pointer state is reconstructed here because the browser sends
    // positions, not the button bitmask semantics our router expects.
    ui::Point lastPointer_{};
    ui::Point lastClickPos_{};
    double lastClickTime_ = -1.0;
    std::uint8_t buttons_ = 0;
};

}  // namespace folio::shell
