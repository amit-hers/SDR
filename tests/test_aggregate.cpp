// Aggregation round-trip tests.
//
// A fault here is invisible from the radio statistics: frames decode, CRC
// passes, frames_rx_good increments -- and the packets inside simply never
// reach the TAP. That is exactly what was observed on hardware (215 decoded
// frames carrying ~645 packets delivered 42 datagrams), so this is pinned
// down in software rather than inferred from link counters.
#include "sdr/framing/Aggregate.hpp"
#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/framing/Frame.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace sdr;

static std::vector<uint8_t> mkpkt(size_t n, uint8_t seed) {
    std::vector<uint8_t> p(n);
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>((i * 31 + seed) & 0xFF);
    return p;
}

void run_aggregate() {
    std::printf("[aggregate] pack/split round-trip\n");
    {
        std::vector<std::vector<uint8_t>> pkts = {
            mkpkt(300, 1), mkpkt(300, 2), mkpkt(300, 3)
        };
        std::vector<uint8_t> buf;
        for (auto& p : pkts) aggregate::append(buf, p.data(), p.size());
        assert(buf.size() == 3 * (300 + AGGR_LEN_PREFIX));

        std::vector<std::vector<uint8_t>> out;
        size_t n = aggregate::split(buf.data(), buf.size(),
            [&](const uint8_t* d, size_t l) { out.emplace_back(d, d + l); });
        assert(n == 3);
        assert(out == pkts);
    }

    std::printf("[aggregate] survives the full frame round-trip\n");
    {
        // The path that actually runs on the link: pack -> Framer -> Deframer
        // -> split. Deframer trims the payload to the header's length, so a
        // mismatch there silently truncates the last record.
        std::vector<std::vector<uint8_t>> pkts = {
            mkpkt(64, 9), mkpkt(517, 10), mkpkt(300, 11), mkpkt(1, 12)
        };
        std::vector<uint8_t> buf;
        for (auto& p : pkts) aggregate::append(buf, p.data(), p.size());

        Framer framer;
        auto frame = framer.encode(buf, FL_AGGR, ModCode::QPSK, BwCode::BW_1P25, 1, 5);

        Deframer df;
        std::optional<DecodedFrame> got;
        for (size_t i = PREAMBLE_LEN; i < frame.size(); ++i)
            if (auto d = df.push(frame[i], nullptr, nullptr)) { got = std::move(d); break; }
        assert(got.has_value());
        assert(got->flags & FL_AGGR);
        assert(got->payload.size() == buf.size());

        std::vector<std::vector<uint8_t>> out;
        size_t n = aggregate::split(got->payload.data(), got->payload.size(),
            [&](const uint8_t* d, size_t l) { out.emplace_back(d, d + l); });
        assert(n == pkts.size());
        assert(out == pkts);
    }

    std::printf("[aggregate] sizes near the seal threshold\n");
    {
        // Packets are sealed when the next one would exceed the target, so
        // the largest aggregate is just under it plus one packet's worth.
        for (size_t sz : {1u, 2u, 63u, 64u, 65u, 600u, 1198u}) {
            std::vector<uint8_t> buf;
            auto p = mkpkt(sz, 7);
            aggregate::append(buf, p.data(), p.size());
            assert(buf.size() == aggregate::costOf(sz));
            std::vector<std::vector<uint8_t>> out;
            size_t n = aggregate::split(buf.data(), buf.size(),
                [&](const uint8_t* d, size_t l) { out.emplace_back(d, d + l); });
            assert(n == 1);
            assert(out[0] == p);
        }
    }

    std::printf("[aggregate] malformed records stop the walk, no overrun\n");
    {
        std::vector<uint8_t> buf;
        auto p = mkpkt(100, 4);
        aggregate::append(buf, p.data(), p.size());
        // Claim a length longer than what remains.
        buf.push_back(0xFF); buf.push_back(0xFF);
        buf.push_back(0x01);

        size_t n = aggregate::split(buf.data(), buf.size(),
            [&](const uint8_t*, size_t) {});
        assert(n == 1);          // the good record only
    }

    std::printf("[aggregate] a single max-size packet still round-trips\n");
    {
        // A packet at the TAP MTU exceeds AGGR_TARGET_BYTES on its own; it
        // must still be carried rather than dropped.
        auto p = mkpkt(1386, 21);
        assert(aggregate::costOf(p.size()) > AGGR_TARGET_BYTES);
        std::vector<uint8_t> buf;
        aggregate::append(buf, p.data(), p.size());
        assert(buf.size() <= MAX_PAYLOAD);
        std::vector<std::vector<uint8_t>> out;
        aggregate::split(buf.data(), buf.size(),
            [&](const uint8_t* d, size_t l) { out.emplace_back(d, d + l); });
        assert(out.size() == 1 && out[0] == p);
    }

    std::printf("[aggregate] OK\n");
}
