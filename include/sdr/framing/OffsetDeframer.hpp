#pragma once
// Recover frames from a demodulated byte stream whose byte grid is unknown.
//
// The fabric demodulator packs four 2-bit symbols into every byte, but it
// starts doing so at whatever symbol it happened to lock onto, so the byte
// boundaries it emits are offset from the transmitted ones by 0, 1, 2 or 3
// symbols -- 0, 2, 4 or 6 bits. A single Deframer sees the sync word split
// across two bytes in three cases out of four and finds nothing.
//
// Two properties of the fabric decide the design, and both were measured:
//
//  * ALL FOUR OFFSETS ARE RUN AND THEIR RESULTS MERGED, rather than searching
//    for the one true offset and keeping it. Taking only the best-scoring
//    offset discarded 70% of the frames that were actually recovered (471 of
//    1567 in the run that established this). The grid is not fixed for the
//    length of a stream.
//  * DECODING IS SCOPED TO ONE DMA PACKET. The byte grid is continuous only
//    within a single transfer: the demodulator keeps emitting through the gap
//    between packets and the number of bytes lost there is arbitrary, so the
//    symbol phase of one packet says nothing about the next. Analysing a
//    1024-byte capture as if it were 8192 reported 22.37% loss against a true
//    0.19% -- a fabricated regression. Each packet therefore starts from fresh
//    Deframers, at the cost of any frame straddling a boundary (about 4% at
//    ~25 frames per 32768-byte packet).
#include "sdr/framing/Deframer.hpp"
#include "sdr/framing/Frame.hpp"

#include <cstdint>
#include <cstddef>
#include <deque>
#include <utility>
#include <vector>

namespace sdr {

struct OffsetFrame {
    uint32_t             seq;
    uint8_t              flags;
    std::vector<uint8_t> payload;
};

// Shift a buffer left by `bits` so the transmitted byte grid is restored.
// `bits` is always a multiple of 2, because symbols are atomic and only four
// alignments are possible. The final byte has no successor to borrow from and
// is dropped, so the result is one byte shorter.
inline std::vector<uint8_t> realign(const uint8_t* in, size_t n, int bits) {
    if (bits == 0) return std::vector<uint8_t>(in, in + n);
    std::vector<uint8_t> out(n ? n - 1 : 0);
    for (size_t i = 0; i + 1 < n; ++i)
        out[i] = static_cast<uint8_t>((in[i] << bits) | (in[i + 1] >> (8 - bits)));
    return out;
}

class OffsetDeframer {
public:
    // Decode one DMA packet, deduplicated against recent history.
    std::vector<OffsetFrame> pushPacket(const uint8_t* data, size_t n) {
        std::vector<OffsetFrame> got;

        for (int off = 0; off < 4; ++off) {
            std::vector<uint8_t> s = realign(data, n, off * 2);
            Deframer d;
            for (size_t i = 0; i < s.size(); ++i) {
                // nullptr for FEC and cipher: this path carries neither, and
                // passing them would pull liquid-dsp and OpenSSL onto a target
                // that does not need them.
                auto r = d.push(s[i], nullptr, nullptr);
                if (r) {
                    got.push_back({r->seq, r->flags, std::move(r->payload)});
                    ++off_hits_[off];
                }
            }
            crc_errors_ += d.crcErrors();
        }

        // A frame can only be received once; two offsets reporting the same
        // seq are the same frame seen twice, not two receptions.
        std::vector<OffsetFrame> out;
        for (auto& g : got) {
            bool dup = false;
            for (uint32_t s : recent_) if (s == g.seq) { dup = true; break; }
            if (dup) { ++dups_; continue; }
            recent_.push_back(g.seq);
            if (recent_.size() > RECENT) recent_.pop_front();
            out.push_back(std::move(g));
        }
        return out;
    }

    uint64_t crcErrors()          const { return crc_errors_; }
    uint64_t duplicates()         const { return dups_; }
    uint64_t offsetHits(int off)  const { return off_hits_[off & 3]; }

private:
    // Deep enough to cover a whole packet's worth of frames plus slack, so a
    // duplicate arriving from a later offset is still recognised.
    static constexpr size_t RECENT = 256;

    std::deque<uint32_t> recent_;
    uint64_t             crc_errors_ {0};
    uint64_t             dups_       {0};
    uint64_t             off_hits_[4]{};
};

} // namespace sdr
