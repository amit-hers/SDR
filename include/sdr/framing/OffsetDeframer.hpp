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
//    Deframers.
//
//  * A BOUNDARY WINDOW RECOVERS THE FRAMES THAT STRADDLE THE CUT (2026-09-24).
//    Restarting per packet loses any frame spanning the boundary, costed at
//    ~4% for ~25 frames in a 32768-byte packet. At the deployed 8192-byte
//    packet with full-size 1514-byte frames it is not 4% but up to
//    1584/8192 = 19% -- and it is NOT random per frame: the RX packet phase is
//    fixed for a demodulator session while the transmitter places a data frame
//    at a repeatable offset in its block, so a given frame size either always
//    straddles or never does. Measured: full-size pings alternating between 0%
//    and 100% loss across reboots with every counter healthy, which is what
//    made large-frame and TCP behaviour look episodic and unexplainable.
//    So each packet's tail is retained and re-decoded joined to the head of
//    the next. Correctness is gated by the frame CRC exactly as on the primary
//    path, and the existing (node, seq) dedup absorbs frames seen twice; a
//    join spanning a real byte-loss gap simply fails CRC and costs nothing.
//    The window is deframed only for the offsets whose decoder was actually
//    mid-frame (or mid-sync) when the packet ended -- an exact test, not a
//    scan, so the three garbage offsets cost nothing. Gating on a sync-word
//    scan instead cost 12 points of decode CPU (41% -> 53% measured on
//    hardware), which matters: the four-offset decode has only ~1.9x margin
//    at 17.28 MS/s.
//    The REAL fix is in the fabric: there is no FIFO between qpsk_demod and
//    the RX DMA, so bytes are lost whenever a transfer is being re-armed, and
//    that is why the byte grid breaks at every packet in the first place.
#include "sdr/framing/Deframer.hpp"
#include "sdr/framing/Frame.hpp"

#include <cstdint>
#include <cstddef>
#include <deque>
#include <utility>
#include <vector>

namespace sdr {

struct OffsetFrame {
    uint32_t             node_id;
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
        std::vector<OffsetFrame> got, joined;
        bool ends_open[4] = {false, false, false, false};

        for (int off = 0; off < 4; ++off) {
            std::vector<uint8_t> s = realign(data, n, off * 2);
            Deframer d;
            for (size_t i = 0; i < s.size(); ++i) {
                // nullptr for FEC and cipher: this path carries neither, and
                // passing them would pull liquid-dsp and OpenSSL onto a target
                // that does not need them.
                auto r = d.push(s[i], nullptr, nullptr);
                if (r) {
                    got.push_back({r->node_id, r->seq, r->flags, std::move(r->payload)});
                    ++off_hits_[off];
                }
            }
            crc_errors_ += d.crcErrors();
            // Did this offset end mid-frame? That, and only that, means a
            // frame straddles the cut here and is worth joining across it.
            ends_open[off] = d.inFrame() || d.syncPending();
        }

        // ── Boundary window: frames that began in the previous packet ──────
        if (!tail_.empty() && n) {
            const size_t head = n < JOIN_WINDOW ? n : JOIN_WINDOW;
            std::vector<uint8_t> join;
            join.reserve(tail_.size() + head);
            join.insert(join.end(), tail_.begin(), tail_.end());
            join.insert(join.end(), data, data + head);
            for (int off = 0; off < 4; ++off) {
                // Exact gate: only the offsets left mid-frame by the PREVIOUS
                // packet can have a frame crossing into this one.
                if (!open_at_cut_[off]) continue;
                std::vector<uint8_t> js = realign(join.data(), join.size(), off * 2);
                Deframer jd;
                for (size_t i = 0; i < js.size(); ++i) {
                    auto r = jd.push(js[i], nullptr, nullptr);
                    if (r) joined.push_back({r->node_id, r->seq, r->flags, std::move(r->payload)});
                }
                // Deliberately NOT added to crc_errors_: this window re-reads
                // bytes the primary pass already counted, and a join across a
                // real gap is expected to fail. Counting it would turn a
                // recovery attempt into a reported fault.
            }
        }

        // A frame can only be received once; two offsets reporting the same
        // (node, seq) are the same frame seen twice, not two receptions.
        // Sequence counters are local to each node, so seq alone incorrectly
        // drops a peer frame whenever its counter matches our own transmitter.
        std::vector<OffsetFrame> out;
        auto accept = [&](OffsetFrame& g, bool from_join) {
            for (const auto& r : recent_)
                if (r.first == g.node_id && r.second == g.seq) {
                    // Join duplicates are counted apart: the window re-reads
                    // frames the primary pass already delivered, so folding
                    // them into `duplicates` would make a healthy link look
                    // as though it received everything twice.
                    if (from_join) ++join_dups_; else ++dups_;
                    return;
                }
            recent_.push_back({g.node_id, g.seq});
            if (recent_.size() > RECENT) recent_.pop_front();
            if (from_join) ++joined_;
            out.push_back(std::move(g));
        };
        for (auto& g : got)    accept(g, false);
        for (auto& g : joined) accept(g, true);

        const size_t keep = n < JOIN_WINDOW ? n : JOIN_WINDOW;
        tail_.assign(data + (n - keep), data + n);
        for (int off = 0; off < 4; ++off) open_at_cut_[off] = ends_open[off];
        return out;
    }

    uint64_t crcErrors()          const { return crc_errors_; }
    uint64_t duplicates()         const { return dups_; }
    uint64_t offsetHits(int off)  const { return off_hits_[off & 3]; }
    // Frames that existed only across a packet boundary and would otherwise
    // have been lost with no counter moving at all.
    uint64_t boundaryRecovered()  const { return joined_; }
    uint64_t boundaryDuplicates() const { return join_dups_; }

private:
    // Longest frame that can be on the wire, so any frame beginning within
    // this distance of the cut may straddle it: preamble + header + payload +
    // CRC + postamble, plus one byte for realign()'s dropped tail.
    static constexpr size_t JOIN_WINDOW =
        PREAMBLE_LEN + HEADER_SIZE + MAX_PAYLOAD + CRC_SIZE + POSTAMBLE_LEN + 1;


    // Deep enough to cover a whole packet's worth of frames plus slack, so a
    // duplicate arriving from a later offset is still recognised.
    static constexpr size_t RECENT = 256;

    std::deque<std::pair<uint32_t, uint32_t>> recent_;
    std::vector<uint8_t> tail_;
    bool                 open_at_cut_[4]{};
    uint64_t             crc_errors_ {0};
    uint64_t             dups_       {0};
    uint64_t             joined_     {0};
    uint64_t             join_dups_  {0};
    uint64_t             off_hits_[4]{};
};

} // namespace sdr
