#pragma once
#include <cstdint>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>

namespace sdr {

// Sliding-window selective-repeat ARQ, independent of radio/transport so it
// can be driven and unit-tested directly. Callers pass already-encoded
// on-wire bytes in and get them back out for (re)transmission.
class ArqWindow {
public:
    using Clock = std::chrono::steady_clock;

    struct Config {
        int      window_size {16};
        int      timeout_ms  {80};
        int      max_retries {5};
    };

    explicit ArqWindow(const Config& cfg) : cfg_(cfg) {}

    
    // Registers a newly-sent frame for tracking. Returns false if the
    // window is full (caller should back off / retry the same packet later).
    bool trySend(uint32_t seq, std::vector<uint8_t> encoded_bytes);

    // Marks `seq` as delivered; removes it from the outstanding window.
    // No-op if `seq` isn't currently outstanding (duplicate/late ACK).
    void onAck(uint32_t seq);

    struct PendingFrame {
        uint32_t             seq;
        std::vector<uint8_t> encoded_bytes;
    };

    // Returns frames whose retransmit timeout has elapsed (as of `now`),
    // bumping their retry count and rearming their timer with backoff.
    // Frames that exceed max_retries are dropped (counted, not returned).
    std::vector<PendingFrame> pollTimeouts(Clock::time_point now = Clock::now());

    bool full()  const;
    int  size()  const;

    uint64_t acked()       const { return acked_.load(); }
    uint64_t retransmits() const { return retransmits_.load(); }
    uint64_t dropped()     const { return dropped_.load(); }

private:
    struct Entry {
        std::vector<uint8_t> encoded_bytes;
        Clock::time_point    deadline;
        int                  retries {0};
        int                  timeout_ms;
    };

    Config cfg_;
    mutable std::mutex        mu_;
    std::map<uint32_t, Entry> outstanding_;

    std::atomic<uint64_t> acked_{0};
    std::atomic<uint64_t> retransmits_{0};
    std::atomic<uint64_t> dropped_{0};
};

} // namespace sdr
