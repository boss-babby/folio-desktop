// Work package C4 acceptance tests.
//
//   "Done when: a fling that submits 5,000 tiles and then reverses executes
//    the visible ones first and cancels the rest within one render slice."
//
// Most of this runs with zero threads on purpose - the ordering, dedupe,
// promotion and fencing rules are pure state machine, and a deterministic
// test that cannot flake is worth far more than one that exercises the
// scheduler "realistically". The last two tests do start threads, to prove
// the lock discipline and the blocking pop actually work.

#include "folio/core/render/TileScheduler.h"
#include "../Harness.h"

#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

using namespace folio::core;
using namespace folio::test;

namespace {

TileRequest req(PageId page, std::int32_t tx, std::int32_t ty,
                Priority prio = Priority::VisibleTile, Generation gen = 1) {
    TileRequest r;
    r.page = page;
    r.tileX = static_cast<std::uint16_t>(tx);
    r.tileY = static_cast<std::uint16_t>(ty);
    r.tileDevice = {tx * kTileSize, ty * kTileSize, kTileSize, kTileSize};
    r.scale = 1.0f;
    r.priority = prio;
    r.snapshotGen = gen;
    return r;
}

TileCallback noop() { return [](TileResult) {}; }

// --- ordering --------------------------------------------------------------

void test_strict_priority_order() {
    section("C4  six lanes, strict priority, FIFO within a lane");
    TileScheduler s;

    // Submit worst-first so FIFO alone would produce the wrong answer.
    s.submit(req(1, 0, 0, Priority::BackgroundText), noop());
    s.submit(req(2, 0, 0, Priority::OffscreenThumb), noop());
    s.submit(req(3, 0, 0, Priority::VisibleThumb), noop());
    s.submit(req(4, 0, 0, Priority::PrefetchTile), noop());
    s.submit(req(5, 0, 0, Priority::VisibleText), noop());
    s.submit(req(6, 0, 0, Priority::VisibleTile), noop());

    std::vector<PageId> order;
    TileJob job;
    while (s.tryPop(job)) {
        order.push_back(job.request.page);
        s.complete(job);
    }
    check(order.size() == 6, "all six dispatched");
    check(order == std::vector<PageId>({6, 5, 4, 3, 2, 1}),
          "dispatched in strict priority order, not submission order");
}

void test_fifo_within_a_lane() {
    section("C4  centre-out order from TileGrid is preserved inside a lane");
    TileScheduler s;
    for (std::int32_t i = 0; i < 8; ++i) {
        s.submit(req(1, i, 0, Priority::VisibleTile), noop());
    }
    std::vector<std::int32_t> order;
    TileJob job;
    while (s.tryPop(job)) {
        order.push_back(job.request.tileDevice.x / kTileSize);
        s.complete(job);
    }
    check(order == std::vector<std::int32_t>({0, 1, 2, 3, 4, 5, 6, 7}),
          "submission order preserved within the lane");
}

// --- dedupe and promotion --------------------------------------------------

void test_duplicate_submission_is_dropped() {
    section("C4  the same tile submitted twice queues once");
    TileScheduler s;
    s.submit(req(1, 0, 0, Priority::VisibleTile), noop());
    s.submit(req(1, 0, 0, Priority::VisibleTile), noop());
    s.submit(req(1, 0, 0, Priority::PrefetchTile), noop());   // worse, ignored

    check(s.queueDepth() == 1, "one job queued");
    check(s.stats().deduped == 2, "two dedupes counted");
}

void test_promotion_moves_lanes_without_duplicating() {
    section("C4  a prefetched tile that becomes visible is promoted, not re-queued");
    TileScheduler s;
    s.submit(req(1, 0, 0, Priority::PrefetchTile), noop());
    s.submit(req(2, 0, 0, Priority::PrefetchTile), noop());
    check(s.queueDepth(Priority::PrefetchTile) == 2, "both in the prefetch lane");

    auto flag = s.submit(req(2, 0, 0, Priority::VisibleTile), noop());
    check(flag != nullptr, "promotion returns the cancel flag");
    check(s.queueDepth() == 2, "still two jobs total - no duplicate");
    check(s.queueDepth(Priority::VisibleTile) == 1, "one promoted into the visible lane");
    check(s.queueDepth(Priority::PrefetchTile) == 1, "one left behind");
    check(s.stats().promoted == 1, "promotion counted");

    TileJob job;
    check(s.tryPop(job) && job.request.page == 2, "the promoted tile comes out first");
    s.complete(job);
}

void test_promotion_preserves_the_cancel_flag() {
    section("C4  cancelling via a pre-promotion handle still works");
    TileScheduler s;
    auto early = s.submit(req(1, 0, 0, Priority::PrefetchTile), noop());
    s.submit(req(1, 0, 0, Priority::VisibleTile), noop());     // promotes

    early->store(true);                                        // caller cancels
    TileJob job;
    check(!s.tryPop(job), "the promoted job was skipped because the old flag fired");
}

// --- cancellation ----------------------------------------------------------

void test_cancel_prefetch_backlog_keeps_visible_work() {
    section("C4  fling start: drop the speculative backlog, keep what is on screen");
    TileScheduler s;
    for (std::int32_t i = 0; i < 4; ++i) {
        s.submit(req(10, i, 0, Priority::VisibleTile), noop());
    }
    for (PageId p = 11; p < 60; ++p) {
        s.submit(req(p, 0, 0, Priority::PrefetchTile), noop());
        s.submit(req(p, 1, 0, Priority::OffscreenThumb), noop());
    }
    check(s.queueDepth() == 4 + 49 * 2, "backlog built up");

    const std::size_t dropped = s.cancelAtOrBelow(Priority::PrefetchTile);
    check(dropped == 49 * 2, "every prefetch and thumbnail job dropped");
    check(s.queueDepth() == 4, "the four visible tiles survived");
    check(s.queueDepth(Priority::VisibleTile) == 4, "and are still in the top lane");
}

void test_cancel_page_flags_inflight_work() {
    section("C4  cancellation reaches work already inside PDFium");
    TileScheduler s;
    s.submit(req(3, 0, 0, Priority::VisibleTile, /*gen=*/1), noop());

    TileJob running;
    check(s.tryPop(running), "popped - now notionally inside FPDF_Render...Continue");
    check(s.stats().inFlight == 1, "tracked as in flight");
    check(!running.isCancelled(), "not cancelled yet");

    s.cancelPage(3, /*olderThan=*/2);
    check(running.isCancelled(),
          "the running job's flag fired - IFSDK_PAUSE will see it next slice");

    s.complete(running);
    check(s.stats().inFlight == 0, "in-flight set drains on complete()");
}

void test_cancelled_jobs_are_never_dispatched() {
    section("C4  a job cancelled while queued is skipped, not executed");
    TileScheduler s;
    auto a = s.submit(req(1, 0, 0, Priority::VisibleTile), noop());
    s.submit(req(2, 0, 0, Priority::VisibleTile), noop());
    auto c = s.submit(req(3, 0, 0, Priority::VisibleTile), noop());

    a->store(true);
    c->store(true);

    std::vector<PageId> ran;
    TileJob job;
    while (s.tryPop(job)) {
        ran.push_back(job.request.page);
        s.complete(job);
    }
    check(ran == std::vector<PageId>({2}), "only the uncancelled job ran");
}

// --- generation fencing ----------------------------------------------------

void test_generation_fence_drops_stale_work() {
    section("C4  an edit invalidates every request made against the old document");
    TileScheduler s;
    for (PageId p = 0; p < 20; ++p) {
        s.submit(req(p, 0, 0, Priority::VisibleTile, /*gen=*/1), noop());
    }
    check(s.queueDepth() == 20, "20 queued at gen 1");

    const std::size_t dropped = s.setGenerationFence(2);
    check(dropped == 20, "all 20 dropped by the fence");
    check(s.queueDepth() == 0, "queue empty");

    auto late = s.submit(req(0, 0, 0, Priority::VisibleTile, /*gen=*/1), noop());
    check(late == nullptr, "a late gen-1 submission is refused at the door");
    check(s.stats().staleDropped >= 1, "counted as stale");

    s.submit(req(0, 0, 0, Priority::VisibleTile, /*gen=*/2), noop());
    check(s.queueDepth() == 1, "gen-2 work is accepted");
}

void test_fence_flags_inflight() {
    section("C4  the fence also reaches running work");
    TileScheduler s;
    s.submit(req(1, 0, 0, Priority::VisibleTile, 1), noop());
    TileJob running;
    check(s.tryPop(running), "popped");
    s.setGenerationFence(5);
    check(running.isCancelled(), "running job flagged by the fence");
    s.complete(running);
}

// --- the acceptance scenario -----------------------------------------------

void test_five_thousand_tile_fling_then_reverse() {
    section("C4  5,000-tile fling, then reverse: visible first, backlog gone");
    TileScheduler s;

    // The fling: 1,000 pages x 5 tiles, speculative.
    for (PageId p = 0; p < 1000; ++p) {
        for (std::int32_t t = 0; t < 5; ++t) {
            s.submit(req(p, t, 0, Priority::PrefetchTile), noop());
        }
    }
    check(s.queueDepth() == 5000, "5,000 speculative jobs queued");

    // The user stops at page 742. Viewport submits its visible tiles and
    // kills the backlog - the order the real viewport uses, visible first so
    // there is never a window with nothing queued.
    for (std::int32_t t = 0; t < 4; ++t) {
        s.submit(req(742, t, 0, Priority::VisibleTile), noop());
    }
    const std::size_t dropped = s.cancelAtOrBelow(Priority::PrefetchTile);

    check(dropped >= 4995, "the speculative backlog was discarded");
    check(s.queueDepth() == 4, "exactly the four visible tiles remain");

    std::set<PageId> pages;
    TileJob job;
    int n = 0;
    while (s.tryPop(job)) {
        pages.insert(job.request.page);
        s.complete(job);
        ++n;
    }
    check(n == 4, "four jobs executed in total");
    check(pages.size() == 1 && *pages.begin() == 742, "all of them on the visible page");

    const auto st = s.stats();
    note(std::to_string(st.submitted) + " submitted, " +
         std::to_string(st.dispatched) + " dispatched, " +
         std::to_string(st.cancelled) + " cancelled -> " +
         std::to_string(100 * st.dispatched / st.submitted) + "% of work actually done");
}

// --- threads ---------------------------------------------------------------

void test_concurrent_producers_and_consumers() {
    section("C4  4 producers, 3 consumers, 12,000 jobs, no loss and no duplication");
    TileScheduler s;
    constexpr int kProducers = 4;
    constexpr int kConsumers = 3;
    constexpr int kPerProducer = 3000;

    std::atomic<int> executed{0};
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            TileJob job;
            while (s.waitPop(job)) {
                executed.fetch_add(1, std::memory_order_relaxed);
                s.complete(job);
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                // Distinct keys per producer so nothing is deduped away.
                s.submit(req(static_cast<PageId>(p * kPerProducer + i), 0, 0,
                             static_cast<Priority>(i % kPriorityLanes)), noop());
            }
        });
    }
    for (auto& t : producers) t.join();

    // Drain, then shut down to release the consumers.
    for (int spins = 0; spins < 2000 && s.queueDepth() > 0; ++spins) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    s.shutdown();
    for (auto& t : consumers) t.join();

    const auto st = s.stats();
    check(st.submitted == kProducers * kPerProducer, "every submission counted once");
    check(executed.load() == static_cast<int>(st.dispatched),
          "dispatched count matches work actually executed");
    check(st.dispatched + st.cancelled + st.staleDropped + st.deduped ==
              st.submitted,
          "every job is accounted for: dispatched + cancelled + stale + deduped");
    note(std::to_string(st.submitted) + " submitted, " +
         std::to_string(st.dispatched) + " executed across 3 threads");
}

void test_waitpop_unblocks_on_shutdown() {
    section("C4  waitPop returns on shutdown rather than hanging");
    TileScheduler s;
    std::atomic<bool> returned{false};
    std::thread t([&] {
        TileJob job;
        const bool got = s.waitPop(job);
        check(!got, "waitPop returned false");
        returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    check(!returned.load(), "still blocked while the queue is empty");
    s.shutdown();
    t.join();
    check(returned.load(), "unblocked and exited cleanly");
}

}  // namespace

FOLIO_TEST_MAIN("C4 TileScheduler") {
    test_strict_priority_order();
    test_fifo_within_a_lane();
    test_duplicate_submission_is_dropped();
    test_promotion_moves_lanes_without_duplicating();
    test_promotion_preserves_the_cancel_flag();
    test_cancel_prefetch_backlog_keeps_visible_work();
    test_cancel_page_flags_inflight_work();
    test_cancelled_jobs_are_never_dispatched();
    test_generation_fence_drops_stale_work();
    test_fence_flags_inflight();
    test_five_thousand_tile_fling_then_reverse();
    test_concurrent_producers_and_consumers();
    test_waitpop_unblocks_on_shutdown();
}
