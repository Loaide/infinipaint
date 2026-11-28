#pragma once
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <optional>
#include <semaphore>
#include <type_traits>
#include <utility>

namespace engine::tablet {
// One producer and one consumer. The producer owns each empty slot until
// publishing write_; the consumer owns each occupied slot until releasing read_.
// Full means backpressure: do not overwrite ordered document deltas.
template<class T, size_t Capacity>
class SpscRing {
    static_assert(Capacity>0 && std::is_nothrow_move_constructible_v<T>);
    std::array<std::optional<T>,Capacity> slots_;
    alignas(64) std::atomic<size_t> read_{0};
    alignas(64) std::atomic<size_t> write_{0};
public:
    bool canPush() const noexcept {
        return write_.load(std::memory_order_relaxed)-read_.load(std::memory_order_acquire)<Capacity;
    }
    bool push(T&& item) noexcept {
        const auto w=write_.load(std::memory_order_relaxed);
        if(w-read_.load(std::memory_order_acquire)>=Capacity) return false;
        slots_[w%Capacity].emplace(std::move(item));
        write_.store(w+1,std::memory_order_release);return true;
    }
    bool hasData() const noexcept {
        return read_.load(std::memory_order_relaxed)!=write_.load(std::memory_order_acquire);
    }
    std::optional<T> pop() noexcept {
        const auto r=read_.load(std::memory_order_relaxed);
        if(r==write_.load(std::memory_order_acquire)) return {};
        auto& slot=slots_[r%Capacity];
        std::optional<T> out(std::move(*slot));slot.reset();
        read_.store(r+1,std::memory_order_release);return out;
    }
};

// Single preconstructed slot for an ordered, potentially expensive container.
// Callbacks only exchange ownership; no allocation/deletion under handoff.
// Producer cannot touch the slot again until the consumer releases it.
template<class T>
class SpscSlot {
    T value_{};
    std::atomic<bool> full_{false};
public:
    bool canPush() const noexcept {return !full_.load(std::memory_order_acquire);}
    bool hasData() const noexcept {return full_.load(std::memory_order_acquire);}
    template<class F> bool push(F&& transfer) noexcept {
        if(!canPush()) return false;
        transfer(value_);full_.store(true,std::memory_order_release);return true;
    }
    template<class F> bool pop(F&& transfer) noexcept {
        if(!hasData()) return false;
        transfer(value_);full_.store(false,std::memory_order_release);return true;
    }
};

// Three preallocated slots: one writer, one reader, one exchange slot.
// Latest value wins. Neither side touches bytes owned by the other. A reader
// can keep the front reference until its next latch; no reclamation on reader.
template<class T>
class LatestTriple {
    std::array<T,3> values_{};
    static constexpr unsigned dirty=4;
    std::atomic<unsigned> middle_{1};
    unsigned front_=0,back_=2;
public:
    T& write() noexcept {return values_[back_];}
    void publish() noexcept {
        back_=middle_.exchange(back_|dirty,std::memory_order_acq_rel)&3;
    }
    const T& latch() noexcept {
        if(middle_.load(std::memory_order_acquire)&dirty)
            front_=middle_.exchange(front_,std::memory_order_acq_rel)&3;
        return values_[front_];
    }
};

// Multiple producers, single worker; coalesces arbitrary notifications to
// one pending wake. Acquire clears the flag before reading any work mailbox.
// A notification during work schedules one follow-up, never an unbounded FIFO.
class WakeGate {
    std::binary_semaphore semaphore_{0};
    std::atomic<bool> pending_{false};
public:
    void signal() noexcept {
        if(!pending_.exchange(true,std::memory_order_acq_rel)) semaphore_.release();
    }
    void wait() noexcept {
        semaphore_.acquire();pending_.exchange(false,std::memory_order_acq_rel);
    }
};
}
