#pragma once
// Stop an appliance pair from replicating frames when RF and Ethernet provide
// two paths between the same L2 segment.
//
// THE FAILURE THIS PREVENTS. Two appliances on one switch: a frame is captured
// by A, sent over RF, injected onto the switch by B, captured by A again, and
// sent over RF again. Nothing decays -- every lap is regenerated at full power
// by the modem -- so the segment saturates. That segment is the CUSTOMER'S
// network, which is why this ranks above any throughput or latency work: it is
// the only failure mode here that damages something outside the radio.
//
// THE RULE. A frame arriving on Ethernet is NOT forwarded to the radio if its
// source address was last seen arriving FROM the radio. A station reachable
// over the air cannot legitimately also originate on the local wire; if its
// address appears there, the frame is our own echo coming back.
//
// Trace, with the rule active on both units:
//   1. host H broadcasts; both A.eth0 and B.eth0 receive it
//   2. neither knows H as radio-side yet, so both forward it to RF
//   3. each receives the other's transmission, learns H as radio-side, injects
//   4. each then sees the other's injection with SRC=H, now known radio-side,
//      and DROPS it -- the loop terminates here
// Replication is bounded at one extra copy rather than unbounded.
//
// WHY NOT TAG OUR OWN INJECTIONS. A tag would have to live in the frame, and
// altering frames is exactly what a transparent bridge must not do. Source
// learning needs no modification and is what an ordinary bridge already does.
//
// WHY NOT PACKET_IGNORE_OUTGOING. That suppresses a socket's own transmissions,
// so it stops a bridge re-capturing what IT injected. It cannot help here: the
// frame returning to A was injected by B, and to A's socket that is an ordinary
// inbound frame from another station.
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace sdr {

class LoopGuard {
public:
    using clock      = std::chrono::steady_clock;
    using time_point = clock::time_point;

    // How long a radio sighting suppresses wire-side forwarding. Must exceed
    // the round trip a looping frame takes, and stay short enough that a
    // station which genuinely moves to the local side is relearned quickly.
    static constexpr std::chrono::seconds DEFAULT_TTL{30};

    // Bounded so that a flood of distinct source addresses -- accidental or
    // hostile -- cannot grow this without limit on a device with 512 MB and no
    // swap. When full the oldest sighting is evicted, which degrades to
    // "forward it" rather than to a stall.
    static constexpr std::size_t DEFAULT_MAX_ENTRIES = 4096;

    explicit LoopGuard(std::chrono::seconds ttl = DEFAULT_TTL,
                       std::size_t max_entries = DEFAULT_MAX_ENTRIES)
        : ttl_(ttl), max_entries_(max_entries) {}

    // A frame arrived from the radio and is about to be injected onto the wire.
    void learnFromRadio(const uint8_t* frame, std::size_t len, time_point now) {
        uint64_t k;
        if (!sourceKey(frame, len, k)) return;
        if (seen_.size() >= max_entries_ && seen_.find(k) == seen_.end()) evictOldest();
        seen_[k] = now;
    }

    // A frame arrived on the wire. False means it is our own echo returning and
    // must not go back to the radio.
    bool shouldForwardToRadio(const uint8_t* frame, std::size_t len, time_point now) {
        uint64_t k;
        if (!sourceKey(frame, len, k)) return true;   // unusable source: forward
        auto it = seen_.find(k);
        if (it == seen_.end()) return true;
        if (now - it->second > ttl_) { seen_.erase(it); return true; }  // aged out
        return false;
    }

    void expire(time_point now) {
        for (auto it = seen_.begin(); it != seen_.end(); ) {
            if (now - it->second > ttl_) it = seen_.erase(it); else ++it;
        }
    }

    std::size_t size()      const { return seen_.size(); }
    uint64_t    suppressed() const { return suppressed_; }
    void        countSuppressed()  { ++suppressed_; }

private:
    // The SOURCE address identifies the station; the destination does not, so
    // broadcast, multicast, ARP, ND and unknown unicast are all handled by the
    // same rule with no special cases. A source address with the multicast bit
    // set is invalid per 802.3 and is never learned, so a malformed frame
    // cannot poison the table.
    static bool sourceKey(const uint8_t* f, std::size_t len, uint64_t& out) {
        if (len < 14) return false;                 // not a full Ethernet header
        const uint8_t* s = f + 6;
        if (s[0] & 0x01) return false;              // multicast/broadcast source
        uint64_t k = 0;
        for (int i = 0; i < 6; ++i) k = (k << 8) | s[i];
        if (k == 0) return false;                   // all-zero source
        out = k;
        return true;
    }

    void evictOldest() {
        auto oldest = seen_.begin();
        for (auto it = seen_.begin(); it != seen_.end(); ++it)
            if (it->second < oldest->second) oldest = it;
        if (oldest != seen_.end()) seen_.erase(oldest);
    }

    std::unordered_map<uint64_t, time_point> seen_;
    std::chrono::seconds ttl_;
    std::size_t          max_entries_;
    uint64_t             suppressed_ = 0;
};

} // namespace sdr
