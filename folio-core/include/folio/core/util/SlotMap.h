// folio/core/util/SlotMap.h  --  work package A4
//
// Generational-index container. Handles stay valid across insert/erase, and a
// handle to an erased element is detectably stale rather than silently
// pointing at whatever took its place.
//
// Used for pages, annotations, form fields and in-flight tile requests - every
// place where a stale reference would otherwise cause the wrong object to be
// mutated. That failure mode is exactly the class of bug that makes an editor
// corrupt documents, so it is worth the 8 bytes per handle.

#pragma once

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace folio::core {

template <typename T>
struct Handle {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;  // 0 == null handle

    [[nodiscard]] bool valid() const noexcept { return generation != 0; }
    friend bool operator==(const Handle&, const Handle&) = default;
};

template <typename T>
class SlotMap {
public:
    using handle_type = Handle<T>;

    [[nodiscard]] handle_type insert(T value) {
        if (freeHead_ != kNoFree) {
            const std::uint32_t idx = freeHead_;
            freeHead_ = slots_[idx].nextFree;
            slots_[idx].value = std::move(value);
            slots_[idx].occupied = true;
            ++size_;
            return handle_type{idx, slots_[idx].generation};
        }
        slots_.push_back(Slot{std::move(value), 1, kNoFree, true});
        ++size_;
        return handle_type{static_cast<std::uint32_t>(slots_.size() - 1), 1};
    }

    bool erase(handle_type h) noexcept {
        if (!contains(h)) return false;
        Slot& s = slots_[h.index];
        s.occupied = false;
        s.value = T{};
        // Bump generation so existing handles become stale. Wrap skips 0,
        // which is reserved for the null handle.
        s.generation = (s.generation == std::numeric_limits<std::uint32_t>::max())
                           ? 1u
                           : s.generation + 1u;
        s.nextFree = freeHead_;
        freeHead_ = h.index;
        --size_;
        return true;
    }

    [[nodiscard]] bool contains(handle_type h) const noexcept {
        return h.valid() && h.index < slots_.size() &&
               slots_[h.index].occupied &&
               slots_[h.index].generation == h.generation;
    }

    [[nodiscard]] T* get(handle_type h) noexcept {
        return contains(h) ? &slots_[h.index].value : nullptr;
    }
    [[nodiscard]] const T* get(handle_type h) const noexcept {
        return contains(h) ? &slots_[h.index].value : nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    void clear() noexcept { slots_.clear(); freeHead_ = kNoFree; size_ = 0; }

    template <typename Fn>
    void forEach(Fn&& fn) {
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].occupied)
                fn(handle_type{i, slots_[i].generation}, slots_[i].value);
        }
    }

private:
    static constexpr std::uint32_t kNoFree =
        std::numeric_limits<std::uint32_t>::max();

    struct Slot {
        T value{};
        std::uint32_t generation = 1;
        std::uint32_t nextFree = kNoFree;
        bool occupied = false;
    };

    std::vector<Slot> slots_;
    std::uint32_t freeHead_ = kNoFree;
    std::size_t size_ = 0;
};

}  // namespace folio::core
