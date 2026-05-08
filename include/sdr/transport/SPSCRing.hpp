#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>

namespace sdr {

// Lock-free Single-Producer Single-Consumer ring buffer.
// SLOTS must be a power of 2.
template<size_t SLOTS, size_t ITEM_SIZE>
class SPSCRing {
    static_assert((SLOTS & (SLOTS - 1)) == 0, "SLOTS must be a power of 2");
    static constexpr size_t MASK = SLOTS - 1;

    struct alignas(64) Slot {
        uint8_t data[ITEM_SIZE];
        int     len{0};
    };

public:
    SPSCRing() = default;

    bool push(const uint8_t* d, int len) {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= SLOTS) return false;  // full
        auto& s = slots_[h & MASK];
        std::memcpy(s.data, d, static_cast<size_t>(len));
        s.len = len;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Peek at next slot without consuming. Returns nullptr if empty.
    Slot* peek() {
        size_t t = tail_.load(std::memory_order_relaxed);
        size_t h = head_.load(std::memory_order_acquire);
        if (h == t) return nullptr;
        return &slots_[t & MASK];
    }

    void consume() {
        tail_.fetch_add(1, std::memory_order_release);
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire)
            == tail_.load(std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    std::array<Slot, SLOTS>         slots_{};
};

} // namespace sdr
