// folio-render/skia/SkiaGpuTarget.h
//
// A Ganesh-backed persistent scene surface, presented into the window's
// default framebuffer after partial or full painting.
//
// This is the only file that knows both Skia's GPU API and the fact that a
// GL context exists. folio-platform hands out `makeCurrent` / `swapBuffers`
// / `procAddress` and nothing more; everything Skia-shaped happens here.
//
// WHY THIS IS NOT A SUBCLASS OF SkiaPaintTarget
// ---------------------------------------------
// SkiaPaintTarget owns its raster surface and its page/image LRUs, and all
// of that logic is identical on the GPU. So this class *contains* one and
// re-points it at a new SkCanvas each frame: the caches, the sampling
// ladder, the text system and the draw calls are shared verbatim between
// the software and GPU paths. Only the surface changes.

#pragma once

#include "folio/platform/Platform.h"
#include "folio/render/IPaintTarget.h"

#include <memory>
#include <string>

class GrDirectContext;
class SkSurface;

namespace folio::render {

namespace skia { class SkiaPaintTarget; }

class SkiaGpuTarget {
public:
    SkiaGpuTarget();
    ~SkiaGpuTarget();

    SkiaGpuTarget(const SkiaGpuTarget&) = delete;
    SkiaGpuTarget& operator=(const SkiaGpuTarget&) = delete;

    /// Build a Ganesh context on the already-current GL context. Returns
    /// false with a reason when the driver cannot support it - the caller
    /// should then fall back to the software target rather than exit, since
    /// a software Folio is still a working Folio.
    bool initialise(platform::IGlContext& gl, std::string* error);

    /// Recreate the persistent GPU scene and window target on resize.
    /// Unchanged dimensions retain the previous scene for partial repaint.
    bool resize(int widthPx, int heightPx, float dpr);

    /// The target to paint into this frame, or nullptr if unusable.
    [[nodiscard]] IPaintTarget* begin();

    /// Flush Skia's queue and submit. Does **not** swap - the shell owns
    /// presentation, because on some platforms a swap has to be paired with
    /// a display-link callback.
    void end();

    /// Release GPU resources held for frames that are no longer visible.
    /// Called when the window is hidden or the app goes to the background.
    void purgeResources();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t gpuBytesUsed() const;
    /// Scale the cropped scene on the GPU BEFORE reading back the small
    /// snapshot. No full-resolution viewport copy or per-frame readback.
    [[nodiscard]] bool readScenePreview(int x, int y, int w, int h,
                                        int previewW, int previewH,
                                        void* bgra, std::size_t stride);

private:
    // Pimpl, so no Skia GPU header leaks into anything that includes this.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace folio::render
