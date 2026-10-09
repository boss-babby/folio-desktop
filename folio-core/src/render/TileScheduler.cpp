// folio/core/render/TileScheduler.cpp  --  work package C4

#include "folio/core/render/TileScheduler.h"

#include <algorithm>

namespace folio::core {

TileKey keyForRequest(const TileRequest& r) noexcept {
    TileKey k;
    k.pageId = r.page;
    // Straight from the request. See the comment on TileRequest::tileX for
    // what happened the last time these were derived from tileDevice.
    k.tileX = r.tileX;
    k.tileY = r.tileY;
    k.scaleQ = quantiseScale(r.scale);
    k.rotation = static_cast<std::uint8_t>(((r.rotation / 90) % 4 + 4) % 4);
    k.scheme = r.scheme;
    return k;
}

namespace {

[[nodiscard]] std::size_t laneIndex(Priority p) noexcept {
    const auto i = static_cast<std::size_t>(p);
    return (i < kPriorityLanes) ? i : kPriorityLanes - 1;
}

}  // namespace

TileScheduler::~TileScheduler() { shutdown(); }

// ---------------------------------------------------------------------------

CancelFlag TileScheduler::submit(TileRequest request, TileCallback onDone) {
    std::unique_lock lock(mutex_);
    if (shutdown_) {
        return nullptr;
    }

    ++counters_.submitted;

    if (request.snapshotGen < fence_) {
        // Submitted against a document state that no longer exists. Dropping
        // at the door rather than at dispatch matters: during a burst of edits
        // this is where most of the work disappears.
        ++counters_.staleDropped;
        return nullptr;
    }

    const TileKey key = keyForRequest(request);

    if (const auto it = index_.find(key); it != index_.end() && it->second.alive) {
        const std::size_t oldLane = laneIndex(it->second.lane);
        const std::size_t newLane = laneIndex(request.priority);

        if (newLane >= oldLane) {
            ++counters_.deduped;          // already queued at least as urgently
            // Return the existing flag so the caller can still cancel it.
            for (const auto& j : lanes_[oldLane]) {
                if (j.sequence == it->second.sequence) {
                    return j.cancelled;
                }
            }
            return nullptr;
        }

        // Promotion: the tile just became visible while sitting in the
        // prefetch lane. Move it, preserving its original cancel flag and
        // callback so nobody holds a stale handle.
        ++counters_.promoted;
        auto& from = lanes_[oldLane];
        const auto pos = std::find_if(from.begin(), from.end(),
            [&](const TileJob& j) { return j.sequence == it->second.sequence; });
        if (pos != from.end()) {
            TileJob moved = std::move(*pos);
            from.erase(pos);
            moved.request.priority = request.priority;
            // Fresh sequence so it sorts after existing jobs in the new lane:
            // an already-visible tile that has been waiting should not be
            // overtaken, and a promoted one should not jump it.
            moved.sequence = nextSequence_++;
            it->second.lane = request.priority;
            it->second.sequence = moved.sequence;
            auto flag = moved.cancelled;
            lanes_[newLane].push_back(std::move(moved));
            lock.unlock();
            cv_.notify_one();
            return flag;
        }
        // Index said alive but the lane had no entry: a tombstone race. Fall
        // through and enqueue fresh.
    }

    TileJob job;
    job.request = std::move(request);
    job.onDone = std::move(onDone);
    job.cancelled = makeCancelFlag();
    job.sequence = nextSequence_++;

    const std::size_t lane = laneIndex(job.request.priority);
    index_[key] = Slot{job.request.priority, job.sequence, true};
    auto flag = job.cancelled;
    lanes_[lane].push_back(std::move(job));

    lock.unlock();
    cv_.notify_one();
    return flag;
}

// ---------------------------------------------------------------------------

bool TileScheduler::killLocked(const TileKey& key) {
    const auto it = index_.find(key);
    if (it == index_.end() || !it->second.alive) {
        return false;
    }
    auto& lane = lanes_[laneIndex(it->second.lane)];
    const auto pos = std::find_if(lane.begin(), lane.end(),
        [&](const TileJob& j) { return j.sequence == it->second.sequence; });
    if (pos != lane.end()) {
        if (pos->cancelled) {
            pos->cancelled->store(true, std::memory_order_relaxed);
        }
        lane.erase(pos);
    }
    index_.erase(it);
    ++counters_.cancelled;
    return true;
}

std::size_t TileScheduler::setGenerationFence(Generation gen) {
    std::unique_lock lock(mutex_);
    if (gen <= fence_) {
        return 0;
    }
    fence_ = gen;

    std::size_t dropped = 0;
    for (auto& lane : lanes_) {
        for (auto it = lane.begin(); it != lane.end();) {
            if (it->request.snapshotGen < gen) {
                if (it->cancelled) {
                    it->cancelled->store(true, std::memory_order_relaxed);
                }
                index_.erase(keyForRequest(it->request));
                it = lane.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
    }
    counters_.cancelled += dropped;

    // In-flight work cannot be erased, only flagged. The pause callback picks
    // this up within one render slice.
    for (auto& [k, flag] : inFlight_) {
        if (flag) {
            flag->store(true, std::memory_order_relaxed);
        }
    }
    return dropped;
}

Generation TileScheduler::generationFence() const {
    const std::lock_guard lock(mutex_);
    return fence_;
}

std::size_t TileScheduler::cancelPage(PageId page, Generation olderThan) {
    std::unique_lock lock(mutex_);
    std::size_t dropped = 0;

    for (auto& lane : lanes_) {
        for (auto it = lane.begin(); it != lane.end();) {
            if (it->request.page == page && it->request.snapshotGen < olderThan) {
                if (it->cancelled) {
                    it->cancelled->store(true, std::memory_order_relaxed);
                }
                index_.erase(keyForRequest(it->request));
                it = lane.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
    }
    counters_.cancelled += dropped;

    for (auto& [k, flag] : inFlight_) {
        if (k.pageId == page && flag) {
            flag->store(true, std::memory_order_relaxed);
        }
    }
    return dropped;
}

std::size_t TileScheduler::cancelAtOrBelow(Priority worstKept) {
    std::unique_lock lock(mutex_);
    std::size_t dropped = 0;
    for (std::size_t i = laneIndex(worstKept); i < kPriorityLanes; ++i) {
        for (auto& j : lanes_[i]) {
            if (j.cancelled) {
                j.cancelled->store(true, std::memory_order_relaxed);
            }
            index_.erase(keyForRequest(j.request));
            ++dropped;
        }
        lanes_[i].clear();
    }
    counters_.cancelled += dropped;
    return dropped;
}

std::size_t TileScheduler::cancelAll() {
    std::unique_lock lock(mutex_);
    std::size_t dropped = 0;
    for (auto& lane : lanes_) {
        for (auto& j : lane) {
            if (j.cancelled) {
                j.cancelled->store(true, std::memory_order_relaxed);
            }
            ++dropped;
        }
        lane.clear();
    }
    index_.clear();
    counters_.cancelled += dropped;
    for (auto& [k, flag] : inFlight_) {
        if (flag) {
            flag->store(true, std::memory_order_relaxed);
        }
    }
    return dropped;
}

// ---------------------------------------------------------------------------

bool TileScheduler::tryPop(TileJob& out) {
    const std::lock_guard lock(mutex_);
    for (auto& lane : lanes_) {
        while (!lane.empty()) {
            TileJob job = std::move(lane.front());
            lane.pop_front();

            const TileKey key = keyForRequest(job.request);
            index_.erase(key);

            if (job.isCancelled() || job.request.snapshotGen < fence_) {
                ++counters_.staleDropped;
                continue;            // silently skip, keep draining this lane
            }

            inFlight_[key] = job.cancelled;
            ++counters_.dispatched;
            out = std::move(job);
            return true;
        }
    }
    return false;
}

bool TileScheduler::waitPop(TileJob& out) {
    std::unique_lock lock(mutex_);
    for (;;) {
        for (auto& lane : lanes_) {
            while (!lane.empty()) {
                TileJob job = std::move(lane.front());
                lane.pop_front();

                const TileKey key = keyForRequest(job.request);
                index_.erase(key);

                if (job.isCancelled() || job.request.snapshotGen < fence_) {
                    ++counters_.staleDropped;
                    continue;
                }

                inFlight_[key] = job.cancelled;
                ++counters_.dispatched;
                out = std::move(job);
                return true;
            }
        }
        if (shutdown_) {
            return false;
        }
        cv_.wait(lock);
    }
}

void TileScheduler::complete(const TileJob& job) {
    const std::lock_guard lock(mutex_);
    inFlight_.erase(keyForRequest(job.request));
}

void TileScheduler::shutdown() {
    {
        const std::lock_guard lock(mutex_);
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        for (auto& lane : lanes_) {
            for (auto& j : lane) {
                if (j.cancelled) {
                    j.cancelled->store(true, std::memory_order_relaxed);
                }
            }
            lane.clear();
        }
        index_.clear();
        for (auto& [k, flag] : inFlight_) {
            if (flag) {
                flag->store(true, std::memory_order_relaxed);
            }
        }
    }
    cv_.notify_all();
}

bool TileScheduler::isShutdown() const {
    const std::lock_guard lock(mutex_);
    return shutdown_;
}

// ---------------------------------------------------------------------------

TileScheduler::Stats TileScheduler::stats() const {
    const std::lock_guard lock(mutex_);
    Stats s = counters_;
    std::size_t q = 0;
    for (const auto& lane : lanes_) {
        q += lane.size();
    }
    s.queued = q;
    s.inFlight = inFlight_.size();
    return s;
}

std::size_t TileScheduler::queueDepth() const {
    const std::lock_guard lock(mutex_);
    std::size_t q = 0;
    for (const auto& lane : lanes_) {
        q += lane.size();
    }
    return q;
}

std::size_t TileScheduler::queueDepth(Priority lane) const {
    const std::lock_guard lock(mutex_);
    return lanes_[laneIndex(lane)].size();
}

bool TileScheduler::hasWorkAbove(Priority p) const {
    const std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < laneIndex(p); ++i) {
        if (!lanes_[i].empty()) {
            return true;
        }
    }
    return false;
}

bool TileScheduler::isQueued(const TileKey& key) const {
    const std::lock_guard lock(mutex_);
    const auto it = index_.find(key);
    return it != index_.end() && it->second.alive;
}

}  // namespace folio::core
