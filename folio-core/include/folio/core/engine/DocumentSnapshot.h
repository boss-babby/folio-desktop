// folio/core/engine/DocumentSnapshot.h
//
// An immutable, ref-counted view of a PDF's bytes at one point in time.
//
// This is the contract between the two engines (see architecture plan §5):
//   PoDoFo owns truth  ->  produces snapshots
//   PDFium owns pixels ->  consumes snapshots, never writes
//
// Built as a ROPE: [original file] + [incremental update 1] + [update 2] ...
// PoDoFo's SaveUpdate() emits only the delta, so a save costs the delta, never
// a full re-serialisation. PDFium reads through FPDF_FILEACCESS with no copy.
//
// Thread safety: a DocumentSnapshot is immutable after build() and may be read
// concurrently from any thread. Builder is single-threaded.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace folio::core {

using PageId = std::uint32_t;
using Generation = std::uint64_t;
using PageIdSet = std::unordered_set<PageId>;

/// One contiguous run of bytes in the rope.
struct Segment {
    std::shared_ptr<const std::vector<std::byte>> data;
    std::size_t offset;  // absolute offset of this segment within the document
};

class DocumentSnapshot {
public:
    class Builder;

    /// Monotonic version. PDFium drops tile requests tagged with an older gen.
    [[nodiscard]] Generation generation() const noexcept { return generation_; }

    /// Total document length in bytes.
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    /// Rope-aware read. Returns bytes actually read (short read == past EOF).
    /// This is what FPDF_FILEACCESS::m_GetBlock calls. Must be allocation-free.
    [[nodiscard]] std::size_t read(std::size_t pos, std::span<std::byte> out) const noexcept;

    /// Pages whose object graph changed between `from` and this snapshot.
    /// Drives cache invalidation: unchanged pages keep their tiles across a save.
    [[nodiscard]] const PageIdSet& dirtyPagesSince(Generation from) const noexcept;

    /// True if no page content changed - a no-op save. Used by the round-trip
    /// fidelity gate (work package B8).
    [[nodiscard]] bool isCleanSince(Generation from) const noexcept {
        return dirtyPagesSince(from).empty();
    }

    DocumentSnapshot(const DocumentSnapshot&) = delete;
    DocumentSnapshot& operator=(const DocumentSnapshot&) = delete;

private:
    friend class Builder;
    DocumentSnapshot() = default;

    /// Index of the segment containing `pos`, or segments_.size() if past EOF.
    [[nodiscard]] std::size_t segmentFor(std::size_t pos) const noexcept;

    std::vector<Segment> segments_;
    std::size_t size_ = 0;
    Generation generation_ = 0;

    // Dirty-page sets keyed by the generation they are relative to. Bounded:
    // entries older than the oldest live FPDF_DOCUMENT are pruned on append.
    std::vector<std::pair<Generation, PageIdSet>> dirtyLog_;
    static const PageIdSet kEmptyPageSet;
};

using SnapshotPtr = std::shared_ptr<const DocumentSnapshot>;

/// Builds generation 0 from a base buffer, or generation N+1 by appending an
/// incremental update produced by PoDoFo's SaveUpdate().
class DocumentSnapshot::Builder {
public:
    /// Generation 0 from the originally opened file.
    static SnapshotPtr fromBase(std::vector<std::byte> bytes);

    /// Generation N+1 = previous rope + delta. Shares the previous segments,
    /// so appending is O(segments), not O(bytes).
    static SnapshotPtr append(const SnapshotPtr& previous,
                              std::vector<std::byte> delta,
                              PageIdSet dirtyPages);
};

}  // namespace folio::core
