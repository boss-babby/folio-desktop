// folio/core/util/Observable.h  --  work package A4
//
// The notification primitive the whole core is built on. Replaces Qt signals
// so that folio-core stays toolkit-free (plan §4.1).
//
// Acceptance condition: "no allocation on notify".
// Subscribers live in one vector; emit() iterates it without touching the
// allocator. Subscribing allocates (amortised); emitting never does.
//
// Re-entrancy: emit() tolerates subscribers that disconnect themselves or
// others mid-emit. Slots added during an emit are not called until the next.

#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace folio::core {

using SubscriptionId = std::uint64_t;
inline constexpr SubscriptionId kNoSubscription = 0;

template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;

    Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;
    Signal(Signal&&) = default;
    Signal& operator=(Signal&&) = default;

    [[nodiscard]] SubscriptionId connect(Slot slot) {
        const SubscriptionId id = ++lastId_;
        slots_.push_back(Entry{id, std::move(slot), true});
        return id;
    }

    void disconnect(SubscriptionId id) noexcept {
        if (id == kNoSubscription) return;
        for (auto& e : slots_) {
            if (e.id == id) {
                e.alive = false;
                dead_ = true;
                return;
            }
        }
    }

    /// Allocation-free. Safe against self-disconnect during iteration.
    void emit(Args... args) const {
        const std::size_t n = slots_.size();  // snapshot: ignore slots added now
        ++depth_;
        for (std::size_t i = 0; i < n && i < slots_.size(); ++i) {
            if (slots_[i].alive) slots_[i].slot(args...);
        }
        --depth_;
        if (depth_ == 0 && dead_) compact();
    }

    [[nodiscard]] std::size_t subscriberCount() const noexcept {
        std::size_t n = 0;
        for (const auto& e : slots_) n += e.alive ? 1 : 0;
        return n;
    }

    void clear() noexcept { slots_.clear(); dead_ = false; }

private:
    struct Entry {
        SubscriptionId id;
        Slot slot;
        bool alive;
    };

    void compact() const {
        std::erase_if(slots_, [](const Entry& e) { return !e.alive; });
        dead_ = false;
    }

    mutable std::vector<Entry> slots_;
    mutable bool dead_ = false;
    mutable int depth_ = 0;
    SubscriptionId lastId_ = kNoSubscription;
};

/// A value that announces its own changes. The backbone of the binding layer:
/// the Qt adapter subscribes once and forwards to a Q_PROPERTY NOTIFY.
template <typename T>
class Observable {
public:
    Observable() = default;
    explicit Observable(T initial) : value_(std::move(initial)) {}

    [[nodiscard]] const T& get() const noexcept { return value_; }
    [[nodiscard]] const T& operator*() const noexcept { return value_; }
    [[nodiscard]] const T* operator->() const noexcept { return &value_; }

    /// No-op when the value is unchanged - prevents notification storms from
    /// idempotent writes (e.g. a scroll handler re-setting the same page).
    void set(T next) {
        if (value_ == next) return;
        value_ = std::move(next);
        changed.emit(value_);
    }

    /// For types where == is expensive or absent.
    void setForced(T next) {
        value_ = std::move(next);
        changed.emit(value_);
    }

    /// Mutate in place, then notify unconditionally.
    template <typename Fn>
    void mutate(Fn&& fn) {
        std::forward<Fn>(fn)(value_);
        changed.emit(value_);
    }

    Signal<const T&> changed;

private:
    T value_{};
};

/// RAII subscription token. Disconnects on destruction, so a view that dies
/// cannot leave a dangling slot behind.
template <typename... Args>
class ScopedConnection {
public:
    ScopedConnection() = default;
    ScopedConnection(Signal<Args...>& sig, SubscriptionId id) noexcept
        : sig_(&sig), id_(id) {}

    ScopedConnection(ScopedConnection&& o) noexcept
        : sig_(o.sig_), id_(o.id_) { o.sig_ = nullptr; o.id_ = kNoSubscription; }

    ScopedConnection& operator=(ScopedConnection&& o) noexcept {
        if (this != &o) {
            reset();
            sig_ = o.sig_; id_ = o.id_;
            o.sig_ = nullptr; o.id_ = kNoSubscription;
        }
        return *this;
    }

    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;
    ~ScopedConnection() { reset(); }

    void reset() noexcept {
        if (sig_ && id_ != kNoSubscription) sig_->disconnect(id_);
        sig_ = nullptr; id_ = kNoSubscription;
    }

private:
    Signal<Args...>* sig_ = nullptr;
    SubscriptionId id_ = kNoSubscription;
};

}  // namespace folio::core
