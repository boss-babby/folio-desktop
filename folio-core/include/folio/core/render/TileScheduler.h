// folio/core/render/TileScheduler.h  --  work package C4
//
// Priority queue with cancellation, sitting between the viewport and the
// PDFium worker threads.
//
// ---------------------------------------------------------------------------
// WHY THIS IS NOT JUST A THREAD POOL
// ---------------------------------------------------------------------------
// A fling across a 400-page document submits thousands of tile requests in
// under a second, and by the time the first one is rendered the user is
// somewhere else entirely. A FIFO pool renders every single one of them, in
// the wrong order, and the viewport stays blank for seconds. The scheduler's
// job is to make almost all of that work never happen:
//
//   1. SIX STRICT LANES. Lower Priority value always wins. A visible tile
//      jumps the entire backlog; background text extraction only ever runs
//      when nothing else is pending. No ageing, no fair-share - starving the
//      background lane during a fling is the correct behaviour.
//
//   2. DEDUPE AND PROMOTE. The same TileKey submitted twice does not queue
//      twice. If the second submission has a better priority, the existing
//      entry is promoted rather than duplicated.
//
//   3. GENERATION FENCING. Every request carries the snapshot generation it
//      was made against. An edit bumps the generation and everything older is
//      dropped in one O(n) sweep - no per-request bookkeeping at the call site.
//
//   4. CANCELLATION THAT REACHES RUNNING WORK. Queued jobs are simply erased.
//      Jobs already inside FPDF_RenderPageBitmap_Continue cannot be, so each
//      job carries a shared cancel flag that the IFSDK_PAUSE callback polls
//      every ~4 ms slice (see folio-engine/pdfium/PdfiumPause.h). Cancelling
//      is therefore bounded by one slice, not by one page render.
//
// ---------------------------------------------------------------------------
// THREADING
// ---------------------------------------------------------------------------
// One mutex, one condition variable. Producers are the UI thread and the
// prefetcher; consumers are the per-document PDFium threads (PDFium's public
// header is explicit that its API is not thread-safe, so there is exactly one
// consumer per open document, and the pool exists to overlap DIFFERENT
// documents plus the thumbnail renderer).
//
// The queue itself is deliberately thread-count agnostic and owns no threads.
// That keeps it fully testable synchronously - the entire policy below is
// exercised without starting a single thread - and leaves thread ownership to
// RenderService (WP C5), where it belongs.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "folio/core/engine/IPdfRasterizer.h"
#include "folio/core/render/TileKey.h"

namespace folio::core {

/// Polled by the rasterizer's pause handler. shared_ptr because the job
/// outlives the queue entry once it has been popped.
using CancelFlag = std::shared_ptr<std::atomic<bool>>;

[[nodiscard]] inline CancelFlag makeCancelFlag() {
    return std::make_shared<std::atomic<bool>>(false);
}

struct TileJob {
    TileRequest  request{};
    TileCallback onDone{};
    CancelFlag   cancelled{};
    std::uint64_t sequence = 0;   // submission order, for FIFO within a lane

    [[nodiscard]] bool isCancelled() const noexcept {
        return cancelled && cancelled->load(std::memory_order_relaxed);
    }
};

inline constexpr std::size_t kPriorityLanes = 6;
static_assert(static_cast<std::size_t>(Priority::BackgroundText) + 1 == kPriorityLanes,
              "lane count must track the Priority enum");

/// Derive the cache key a request will produce. Shared by the scheduler's
/// dedupe and by the cache lookup that precedes submission, so the two can
/// never disagree about what identifies a tile.
[[nodiscard]] TileKey keyForRequest(const TileRequest& r) noexcept;

class TileScheduler {
public:
    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t deduped = 0;      // identical key already queued
        std::uint64_t promoted = 0;     // re-submitted at a better priority
        std::uint64_t dispatched = 0;
        std::uint64_t cancelled = 0;    // removed from the queue unexecuted
        std::uint64_t staleDropped = 0; // failed the generation fence
        std::size_t   queued = 0;
        std::size_t   inFlight = 0;
    };

    TileScheduler() = default;
    ~TileScheduler();

    TileScheduler(const TileScheduler&) = delete;
    TileScheduler& operator=(const TileScheduler&) = delete;

    // --- producer side -----------------------------------------------------

    /// Queue a tile. Returns the cancel flag for this key, whether the call
    /// enqueued, deduped or promoted - so the caller can always cancel what it
    /// asked for without caring which happened.
    ///
    /// Requests older than the current fence are dropped and return nullptr.
    CancelFlag submit(TileRequest request, TileCallback onDone);

    /// Raise the generation fence. Everything queued below it is dropped and
    /// everything in flight below it is flagged. Call on every document edit.
    /// Returns the number of queued jobs discarded.
    std::size_t setGenerationFence(Generation gen);
    [[nodiscard]] Generation generationFence() const;

    /// Cancel queued and in-flight work for one page older than `olderThan`.
    std::size_t cancelPage(PageId page, Generation olderThan);

    /// Cancel every queued job at or below (numerically >=) `worstKept`.
    /// The viewport calls this with PrefetchTile when a fling starts: drop the
    /// speculative backlog, keep the visible lanes.
    std::size_t cancelAtOrBelow(Priority worstKept);

    std::size_t cancelAll();

    // --- consumer side -----------------------------------------------------

    /// Highest-priority job, FIFO within its lane. False if nothing is ready.
    [[nodiscard]] bool tryPop(TileJob& out);

    /// Block until a job is available or shutdown() is called.
    [[nodiscard]] bool waitPop(TileJob& out);

    /// Workers must call this when a popped job finishes, succeeded or not.
    /// It is what keeps the in-flight set - and therefore cancellation of
    /// running work - accurate.
    void complete(const TileJob& job);

    void shutdown();
    [[nodiscard]] bool isShutdown() const;

    // --- introspection -----------------------------------------------------

    [[nodiscard]] Stats stats() const;
    [[nodiscard]] std::size_t queueDepth() const;
    [[nodiscard]] std::size_t queueDepth(Priority lane) const;
    [[nodiscard]] bool isQueued(const TileKey& key) const;

    /// Is anything queued that is strictly more urgent than `p`?
    ///
    /// This is the function PDFium's IFSDK_PAUSE callback polls, thousands of
    /// times per page render, from the render thread. It must stay cheap: a
    /// lock acquisition and at most five empty() checks. Do not make it
    /// allocate, and do not be tempted to give it a nicer signature that
    /// returns the job - taking a reference to a queue entry from inside a
    /// pause callback is a lifetime bug waiting to happen.
    [[nodiscard]] bool hasWorkAbove(Priority p) const;

private:
    struct Slot {
        Priority      lane = Priority::VisibleTile;
        std::uint64_t sequence = 0;
        bool          alive = true;
    };

    /// Caller holds the lock. Tombstones the key's queue entry.
    bool killLocked(const TileKey& key);

    mutable std::mutex              mutex_;
    std::condition_variable         cv_;
    std::deque<TileJob>             lanes_[kPriorityLanes];
    std::unordered_map<TileKey, Slot> index_;
    std::unordered_map<TileKey, CancelFlag> inFlight_;
    Generation                      fence_ = 0;
    std::uint64_t                   nextSequence_ = 1;
    bool                            shutdown_ = false;
    mutable Stats                   counters_{};
};

}  // namespace folio::core
