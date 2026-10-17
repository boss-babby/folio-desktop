// folio-render/skia/SkiaGpuTarget.cpp
//
// Ganesh GL binding. See the header for why this is a wrapper rather than a
// subclass.
//
// A note on the Skia version: in m144 the Ganesh entry points live in
// namespaces (`GrDirectContexts::MakeGL`, `SkSurfaces::WrapBackendRenderTarget`,
// `GrBackendRenderTargets::MakeGL`) rather than as static members, and the
// headers moved under `include/gpu/ganesh/`. Code written against the old
// spelling compiles against neither. The includes below are the m144 layout.

#include "SkiaGpuTarget.h"

#include "SkiaPaintTarget.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"

#include <cstdio>

namespace folio::render {

namespace {

/// Ganesh asks for entry points by name; the platform layer knows how to
/// resolve them. The `void*` context is the IGlContext.
GrGLFuncPtr resolve(void* ctx, const char* name) {
    auto* gl = static_cast<platform::IGlContext*>(ctx);
    return reinterpret_cast<GrGLFuncPtr>(gl->procAddress(name));
}

}  // namespace

struct SkiaGpuTarget::Impl {
    platform::IGlContext* gl = nullptr;
    sk_sp<GrDirectContext> context;
    sk_sp<SkSurface> surface;       // persistent GPU scene; survives swaps
    sk_sp<SkSurface> windowSurface; // the discardable default framebuffer
    int w = 0, h = 0;
    int stencil = 8;
    int samples = 1;
    std::unique_ptr<skia::SkiaPaintTarget> target;
};

SkiaGpuTarget::SkiaGpuTarget() : impl_(std::make_unique<Impl>()) {}
SkiaGpuTarget::~SkiaGpuTarget() = default;

bool SkiaGpuTarget::initialise(platform::IGlContext& gl, std::string* error) {
    impl_->gl = &gl;
    gl.makeCurrent();

    // MakeAssembledInterface rather than MakeNativeInterface: the native
    // path dlopen()s libGL itself and picks up whatever the loader finds,
    // which on a multi-GPU or GLVND system is not necessarily the context
    // we just made current. Going through the platform's own resolver keeps
    // the interface and the context in agreement.
    sk_sp<const GrGLInterface> iface =
        GrGLMakeAssembledInterface(&gl, resolve);
    if (!iface) {
        if (error) *error = "could not assemble a GrGLInterface (missing GL entry points)";
        return false;
    }

    impl_->context = GrDirectContexts::MakeGL(iface);
    if (!impl_->context) {
        if (error) *error = "GrDirectContexts::MakeGL failed (GL too old for Ganesh?)";
        return false;
    }
    // Ganesh's GPU resources are a FOURTH pool alongside PDFium bitmaps,
    // SkImage copies and the window surface. Set an explicit mobile limit.
#if defined(__ANDROID__)
    impl_->context->setResourceCacheLimit(32u * 1024u * 1024u);
#else
    impl_->context->setResourceCacheLimit(64u * 1024u * 1024u);
#endif

    impl_->stencil = gl.stencilBits();
    impl_->samples = gl.sampleCount();
    return true;
}

bool SkiaGpuTarget::resize(int widthPx, int heightPx, float) {
    if (!impl_->context) return false;
    if (widthPx <= 0 || heightPx <= 0) return false;
    if (impl_->surface && widthPx == impl_->w && heightPx == impl_->h) return true;

    impl_->w = widthPx;
    impl_->h = heightPx;

    // The default framebuffer is undefined after SwapBuffers on Windows.
    // Keep a GPU scene surface across frames so a clipped UI repaint cannot
    // expose uninitialised pixels or rerender the PDF canvas. Presenting the
    // already-composited texture is cheaper than replaying the widget tree.
    GrGLFramebufferInfo fbInfo{};
    fbInfo.fFBOID = impl_->gl ? impl_->gl->defaultFramebuffer() : 0;
    fbInfo.fFormat = 0x8058;  // GL_RGBA8

    GrBackendRenderTarget backend = GrBackendRenderTargets::MakeGL(
        widthPx, heightPx, impl_->samples, impl_->stencil, fbInfo);

    SkSurfaceProps props(0, kRGB_H_SkPixelGeometry);
    impl_->target.reset();
    impl_->surface.reset();
    impl_->windowSurface = SkSurfaces::WrapBackendRenderTarget(
        impl_->context.get(), backend, kBottomLeft_GrSurfaceOrigin,
        kRGBA_8888_SkColorType, SkColorSpace::MakeSRGB(), &props);
    const auto info = SkImageInfo::Make(widthPx, heightPx, kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    if (impl_->windowSurface)
        impl_->surface = SkSurfaces::RenderTarget(
            impl_->context.get(), skgpu::Budgeted::kYes, info, 0,
            kBottomLeft_GrSurfaceOrigin, &props);
    if (!impl_->surface) {
        impl_->windowSurface.reset();
        return false;
    }
    impl_->target = std::make_unique<skia::SkiaPaintTarget>(impl_->surface->getCanvas());
    return true;
}

IPaintTarget* SkiaGpuTarget::begin() {
    if (!impl_->surface || !impl_->target) return nullptr;
    if (impl_->gl) impl_->gl->makeCurrent();
    return impl_->target.get();
}

void SkiaGpuTarget::end() {
    if (!impl_->context || !impl_->surface || !impl_->windowSurface) return;
    // Only the damaged widgets were repainted into `surface`; the PDF and
    // other unchanged pixels remain there. Blit the complete GPU snapshot
    // to the swap buffer, whose prior contents cannot be trusted after swap.
    auto snapshot = impl_->surface->makeImageSnapshot();
    if (!snapshot) return;
    impl_->windowSurface->getCanvas()->drawImage(snapshot, 0.f, 0.f);
    impl_->context->flushAndSubmit(impl_->windowSurface.get(), GrSyncCpu::kNo);
}

void SkiaGpuTarget::purgeResources() {
    // The retained scene itself is a full-window texture. Free it too,
    // otherwise freeGpuResources cannot reclaim pixels still referenced by
    // our SkSurface. resize() recreates it on the next visible frame.
    impl_->target.reset();
    impl_->surface.reset();
    impl_->windowSurface.reset();
#if !defined(__ANDROID__)
    if (impl_->context && impl_->gl) {
        impl_->gl->makeCurrent();
        impl_->context->freeGpuResources();
    }
#endif
}

bool SkiaGpuTarget::valid() const noexcept {
    return impl_ && impl_->surface != nullptr;
}

std::size_t SkiaGpuTarget::gpuBytesUsed() const {
    if (!impl_->context) return 0;
    int count = 0;
    size_t bytes = 0;
    impl_->context->getResourceCacheUsage(&count, &bytes);
    return bytes;
}

}  // namespace folio::render
