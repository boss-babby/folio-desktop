#include "folio/core/engine/DocumentSnapshot.h"

#include <algorithm>
#include <cstring>

namespace folio::core {

const PageIdSet DocumentSnapshot::kEmptyPageSet{};

std::size_t DocumentSnapshot::segmentFor(std::size_t pos) const noexcept {
    if (pos >= size_) return segments_.size();
    // Binary search: segments are sorted by offset and contiguous.
    auto it = std::upper_bound(
        segments_.begin(), segments_.end(), pos,
        [](std::size_t p, const Segment& s) { return p < s.offset; });
    return static_cast<std::size_t>(std::distance(segments_.begin(), it)) - 1;
}

std::size_t DocumentSnapshot::read(std::size_t pos,
                                   std::span<std::byte> out) const noexcept {
    if (pos >= size_ || out.empty()) return 0;

    const std::size_t want = std::min(out.size(), size_ - pos);
    std::size_t done = 0;
    std::size_t seg = segmentFor(pos);

    while (done < want && seg < segments_.size()) {
        const Segment& s = segments_[seg];
        const std::size_t segLen = s.data->size();
        const std::size_t within = (pos + done) - s.offset;
        if (within >= segLen) { ++seg; continue; }

        const std::size_t chunk = std::min(want - done, segLen - within);
        std::memcpy(out.data() + done, s.data->data() + within, chunk);
        done += chunk;
        ++seg;
    }
    return done;
}

const PageIdSet& DocumentSnapshot::dirtyPagesSince(
    Generation from) const noexcept {
    if (from >= generation_) return kEmptyPageSet;
    // Cheap path: the common case is from == generation_ - 1.
    for (const auto& [gen, set] : dirtyLog_) {
        if (gen == from) return set;
    }
    return kEmptyPageSet;
}

SnapshotPtr DocumentSnapshot::Builder::fromBase(std::vector<std::byte> bytes) {
    auto snap = std::shared_ptr<DocumentSnapshot>(new DocumentSnapshot());
    const std::size_t n = bytes.size();
    snap->segments_.push_back(
        Segment{std::make_shared<const std::vector<std::byte>>(std::move(bytes)), 0});
    snap->size_ = n;
    snap->generation_ = 0;
    return snap;
}

SnapshotPtr DocumentSnapshot::Builder::append(const SnapshotPtr& previous,
                                              std::vector<std::byte> delta,
                                              PageIdSet dirtyPages) {
    auto snap = std::shared_ptr<DocumentSnapshot>(new DocumentSnapshot());
    snap->segments_ = previous->segments_;  // shares buffers, O(segments)
    const std::size_t deltaLen = delta.size();
    snap->segments_.push_back(
        Segment{std::make_shared<const std::vector<std::byte>>(std::move(delta)),
                previous->size_});
    snap->size_ = previous->size_ + deltaLen;
    snap->generation_ = previous->generation_ + 1;

    // Accumulate: a tile cached at gen G must be invalidated if the page is
    // dirty in ANY step between G and now, not just the last one.
    snap->dirtyLog_ = previous->dirtyLog_;
    for (auto& [gen, set] : snap->dirtyLog_) {
        (void)gen;
        set.insert(dirtyPages.begin(), dirtyPages.end());
    }
    snap->dirtyLog_.emplace_back(previous->generation_, std::move(dirtyPages));
    return snap;
}

}  // namespace folio::core
