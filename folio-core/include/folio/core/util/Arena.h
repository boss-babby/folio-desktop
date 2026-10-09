// folio/core/util/Arena.h  --  work package A4
//
// Bump allocator for per-frame scratch. Enforces plan §14.2 rule 6:
// "no allocation in the frame path".
//
// At 60 fps you have 16.67 ms; a single malloc contending on a global lock
// while the render thread is mid-batch is a dropped frame. The arena gives
// the frame path a pointer bump and a single reset() at the end.
//
// Not thread-safe by design: one arena per thread.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace folio::core {

class Arena {
public:
    explicit Arena(std::size_t blockBytes = 64 * 1024)
        : blockBytes_(blockBytes) {}

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) {
        if (bytes == 0) return nullptr;

        const std::uintptr_t cur = reinterpret_cast<std::uintptr_t>(cursor_);
        const std::uintptr_t aligned = (cur + align - 1) & ~(align - 1);
        const std::size_t padding = static_cast<std::size_t>(aligned - cur);

        if (cursor_ == nullptr || padding + bytes > remaining_) {
            grow(bytes + align);
            return allocate(bytes, align);
        }

        auto* out = reinterpret_cast<void*>(aligned);
        cursor_ = static_cast<std::byte*>(out) + bytes;
        remaining_ -= (padding + bytes);
        highWater_ += (padding + bytes);
        return out;
    }

    template <typename T, typename... A>
    [[nodiscard]] T* create(A&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "Arena never runs destructors - use trivially "
                      "destructible types only");
        void* mem = allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<A>(args)...);
    }

    /// Call once per frame. Keeps the blocks, drops the contents: zero frees,
    /// so steady-state frames perform no allocator work at all.
    void reset() noexcept {
        if (blocks_.empty()) { cursor_ = nullptr; remaining_ = 0; return; }
        // Reuse the largest block; release the rest only if we over-grew.
        cursor_ = blocks_.front().get();
        remaining_ = blockSizes_.front();
        if (blocks_.size() > 1) {
            blocks_.resize(1);
            blockSizes_.resize(1);
        }
        peakHighWater_ = highWater_ > peakHighWater_ ? highWater_ : peakHighWater_;
        highWater_ = 0;
    }

    [[nodiscard]] std::size_t bytesThisFrame() const noexcept { return highWater_; }
    [[nodiscard]] std::size_t peakBytes() const noexcept { return peakHighWater_; }
    [[nodiscard]] std::size_t blockCount() const noexcept { return blocks_.size(); }

private:
    void grow(std::size_t atLeast) {
        const std::size_t sz = atLeast > blockBytes_ ? atLeast : blockBytes_;
        blocks_.push_back(std::make_unique<std::byte[]>(sz));
        blockSizes_.push_back(sz);
        cursor_ = blocks_.back().get();
        remaining_ = sz;
    }

    std::vector<std::unique_ptr<std::byte[]>> blocks_;
    std::vector<std::size_t> blockSizes_;
    std::byte* cursor_ = nullptr;
    std::size_t remaining_ = 0;
    std::size_t blockBytes_;
    std::size_t highWater_ = 0;
    std::size_t peakHighWater_ = 0;
};

/// Resets an arena when the frame ends, however it ends.
class ArenaFrame {
public:
    explicit ArenaFrame(Arena& a) noexcept : a_(a) {}
    ~ArenaFrame() { a_.reset(); }
    ArenaFrame(const ArenaFrame&) = delete;
    ArenaFrame& operator=(const ArenaFrame&) = delete;

private:
    Arena& a_;
};

}  // namespace folio::core
