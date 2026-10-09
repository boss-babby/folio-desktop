// folio-engine/pdfium/PdfiumPause.h  --  work package B4
//
// The cooperative scheduler. This is the trick that makes a single PDFium
// thread sufficient (plan §7.2).
//
// PDFium's public header: "None of the PDFium APIs are thread-safe. They
// expect to be called from a single thread." So one thread owns every FPDF_*
// call for a document. The obvious worry is that a slow page blocks everything
// - a map-heavy A0 sheet can take two seconds.
//
// FPDF_RenderPageBitmap_Start + IFSDK_PAUSE + FPDF_RenderPage_Continue turn
// that two seconds into ~500 resumable fragments. A visible tile queued
// mid-render preempts at the next slice boundary. Result: thumbnails and the
// main viewport never starve each other, with no locking anywhere.
//
// Acceptance (WP B4): "a 2 s page render yields >=100 times; a high-priority
// tile preempts within 8 ms".

#pragma once

#include <fpdf_progressive.h>

#include <chrono>
#include <functional>

namespace folio::engine {

/// Budget for one slice. 4 ms keeps preemption latency inside a 60 fps frame
/// while staying large enough that per-slice overhead stays negligible.
inline constexpr std::chrono::microseconds kRenderSlice{4000};

class PdfiumPause : public IFSDK_PAUSE {
public:
    /// `hasUrgentWork` is polled by PDFium between internal render steps. It
    /// must be cheap and must not allocate - it runs thousands of times per
    /// page. In practice it is one relaxed atomic load on the scheduler's
    /// high-priority queue depth.
    explicit PdfiumPause(std::function<bool()> hasUrgentWork)
        : urgent_(std::move(hasUrgentWork)) {
        // IFSDK_PAUSE is a C struct with function-pointer members; we inherit
        // from it so `this` can be passed straight to PDFium and recovered in
        // the static callback. version must be 1 or PDFium rejects it.
        version        = 1;
        NeedToPauseNow = &PdfiumPause::needToPauseNow;
        user           = this;
    }

    /// Call at the start of each slice.
    void beginSlice() noexcept {
        sliceStart_ = std::chrono::steady_clock::now();
        ++slices_;
    }

    [[nodiscard]] std::uint64_t sliceCount() const noexcept { return slices_; }

    /// Longest observed gap between a pause request and the actual yield.
    [[nodiscard]] std::chrono::microseconds worstLatency() const noexcept {
        return worst_;
    }

private:
    static FPDF_BOOL needToPauseNow(IFSDK_PAUSE* p) {
        auto* self = static_cast<PdfiumPause*>(p);

        // Cheap check first: has our time slice expired?
        const auto now = std::chrono::steady_clock::now();
        const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(
            now - self->sliceStart_);
        if (spent >= kRenderSlice) {
            if (spent > self->worst_) self->worst_ = spent;
            return 1;
        }

        // Then the slightly more expensive one: did something urgent arrive?
        if (self->urgent_ && self->urgent_()) {
            if (spent > self->worst_) self->worst_ = spent;
            return 1;
        }
        return 0;
    }

    std::function<bool()> urgent_;
    std::chrono::steady_clock::time_point sliceStart_{};
    std::chrono::microseconds worst_{0};
    std::uint64_t slices_ = 0;
};

}  // namespace folio::engine
