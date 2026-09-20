#pragma once
// Separate control/telemetry from bulk/video, so a small command does not wait
// behind a queue of video frames.
//
// WHAT THIS CAN AND CANNOT FIX. The receive path cannot deliver a frame before
// a whole DMA packet has arrived: at PKT_BYTES 32768 and 7.66 Mbit/s that is
// 34 ms, and it applies to a 20-byte command exactly as to a 1200-byte video
// chunk. Scheduling does NOT change that floor. What it changes is which frames
// enter the next uncommitted block -- so a command queued behind ten video
// frames goes out in this block rather than the next one. Worth having, and not
// a substitute for reducing PKT_BYTES.
//
// A BLOCK ALREADY HANDED TO HARDWARE IS NEVER REORDERED. Priority applies only
// while frames are still queued in software; once a DMA block is committed it
// is transmitted as it stands.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace sdr {

enum class Class : uint8_t { CONTROL = 0, BULK = 1 };

// Classify by what is visible in a transparent L2 bridge.
//
// An 802.1Q priority tag is the only explicit statement of intent a frame
// carries, so it wins when present. Otherwise size is the honest proxy: control
// and telemetry are small, video is not. ARP and IPv6 ND are treated as control
// regardless of size, because losing them to a video burst breaks reachability
// rather than merely delaying a picture.
inline Class classify(const uint8_t* f, std::size_t n, std::size_t bulk_min = 512) {
    if (n < 14) return Class::CONTROL;                 // runts: cheap, send early
    const uint16_t et = uint16_t(f[12]) << 8 | f[13];
    if (et == 0x8100 && n >= 16) {                     // 802.1Q: trust the PCP
        const uint8_t pcp = uint8_t(f[14] >> 5);
        return pcp >= 4 ? Class::CONTROL : Class::BULK;
    }
    if (et == 0x0806) return Class::CONTROL;           // ARP
    if (et == 0x86DD && n >= 21 && f[20] == 58)        // IPv6 ICMPv6 (ND)
        return Class::CONTROL;
    return n < bulk_min ? Class::CONTROL : Class::BULK;
}

// Two bounded queues with starvation protection.
//
// Strict priority starves bulk traffic outright: a steady telemetry stream at
// even a few hundred frames a second would stop video entirely. So every
// `bulk_every` dequeues take one bulk frame even when control is waiting. The
// ratio is the knob between latency and fairness, and it is bounded at both
// ends rather than left to chance.
class TrafficQueues {
public:
    struct Frame { std::vector<uint8_t> bytes; };

    TrafficQueues(std::size_t control_limit = 256,
                  std::size_t bulk_limit    = 256,
                  unsigned    bulk_every    = 8)
        : climit_(control_limit), blimit_(bulk_limit), bulk_every_(bulk_every) {}

    // BOUND THE QUEUE BY TIME, NOT BY PACKET COUNT.
    //
    // A count limit permits unbounded latency: 256 frames of 1270 B at
    // 6.96 Mbit/s is 373 ms of queue, which is 37x a 10 ms budget and is
    // exactly the bufferbloat this must avoid. The honest limit is how long the
    // queue takes to drain at the link rate, so the depth follows the rate
    // instead of being guessed.
    //
    // Control gets a SHORTER budget than bulk. A control queue that is allowed
    // to grow has already failed at its job -- latency is the whole reason the
    // class exists -- whereas video would rather be buffered than dropped.
    void setRateBudget(uint64_t link_bits_per_sec,
                       unsigned control_budget_ms = 10,
                       unsigned bulk_budget_ms    = 100) {
        rate_bps_ = link_bits_per_sec;
        cbytes_max_ = rate_bps_ * control_budget_ms / 8000;
        bbytes_max_ = rate_bps_ * bulk_budget_ms    / 8000;
    }

    // Occupancy in bytes, and what that means in milliseconds at the link rate.
    // Depth in packets is not an operational number; drain time is.
    std::size_t controlBytes() const { return cbytes_; }
    std::size_t bulkBytes()    const { return bbytes_; }
    unsigned    drainMs() const {
        if (rate_bps_ == 0) return 0;
        return unsigned((uint64_t(cbytes_ + bbytes_) * 8000) / rate_bps_);
    }

    // Returns false when the frame was dropped because its queue is full.
    // A bounded queue that drops is honest; an unbounded one converts a
    // transient overload into unbounded memory growth on a board with no swap,
    // and into latency that grows without limit for everything behind it.
    bool push(const uint8_t* f, std::size_t n, std::size_t bulk_min = 512) {
        const bool is_ctrl = (classify(f, n, bulk_min) == Class::CONTROL);
        auto& q = is_ctrl ? ctrl_ : bulk_;
        const std::size_t lim    = is_ctrl ? climit_ : blimit_;
        const std::size_t bmax   = is_ctrl ? cbytes_max_ : bbytes_max_;
        std::size_t&      bytes  = is_ctrl ? cbytes_ : bbytes_;
        // Tail-drop on whichever bound binds first: the packet count caps
        // memory, the byte budget caps latency. Dropping the arriving frame
        // rather than an queued one keeps delivery in order for what is already
        // committed.
        if (q.size() >= lim || (bmax > 0 && bytes + n > bmax)) {
            ++dropped_[is_ctrl ? 0 : 1];
            return false;
        }
        bytes += n;
        q.push_back(Frame{std::vector<uint8_t>(f, f + n)});
        return true;
    }

    bool empty() const { return ctrl_.empty() && bulk_.empty(); }

    // Next frame to transmit, honouring priority with a bulk guarantee.
    bool pop(std::vector<uint8_t>& out) {
        std::deque<Frame>* pick = nullptr;
        const bool bulk_turn = bulk_every_ > 0 && (since_bulk_ >= bulk_every_);
        if (bulk_turn && !bulk_.empty())      pick = &bulk_;
        else if (!ctrl_.empty())              pick = &ctrl_;
        else if (!bulk_.empty())              pick = &bulk_;
        if (!pick) return false;
        out = std::move(pick->front().bytes);
        (pick == &ctrl_ ? cbytes_ : bbytes_) -= out.size();
        pick->pop_front();
        if (pick == &bulk_) since_bulk_ = 0; else ++since_bulk_;
        return true;
    }

    std::size_t controlDepth() const { return ctrl_.size(); }
    std::size_t bulkDepth()    const { return bulk_.size(); }
    uint64_t    controlDropped() const { return dropped_[0]; }
    uint64_t    bulkDropped()    const { return dropped_[1]; }

private:
    std::deque<Frame> ctrl_, bulk_;
    std::size_t climit_, blimit_;
    unsigned    bulk_every_;
    unsigned    since_bulk_ = 0;
    uint64_t    dropped_[2] = {0, 0};
    std::size_t cbytes_ = 0, bbytes_ = 0;
    std::size_t cbytes_max_ = 0, bbytes_max_ = 0;   // 0 = no byte budget set
    uint64_t    rate_bps_ = 0;
};

} // namespace sdr
