#include "sdr/framing/Framer.hpp"
#include "sdr/framing/OffsetDeframer.hpp"
#include "sdr/framing/Frame.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace sdr;

namespace {

// Simulate the fabric demodulator's arbitrary symbol phase.
//
// The demodulator packs four 2-bit symbols per byte starting from whichever
// symbol it locked onto, so the bytes it emits are the transmitted bit stream
// delayed by 0, 2, 4 or 6 bits. Reproduce that by prepending `bits` bits of
// filler and repacking -- the inverse of what realign() undoes.
std::vector<uint8_t> delayBits(const std::vector<uint8_t>& in, int bits) {
    if (bits == 0) return in;
    std::vector<uint8_t> out(in.size() + 1, 0);
    // Leading filler is deliberately NOT zero: zeros would make an accidental
    // sync match likelier and could flatter the result.
    out[0] = static_cast<uint8_t>(0xA5 >> bits << bits);
    for (size_t i = 0; i < in.size(); ++i) {
        out[i]     = static_cast<uint8_t>(out[i] | (in[i] >> bits));
        out[i + 1] = static_cast<uint8_t>(in[i] << (8 - bits));
    }
    return out;
}

std::vector<uint8_t> makePayload(size_t len, uint32_t seed) {
    std::vector<uint8_t> p(len);
    uint32_t x = seed * 2654435761u + 1;
    for (auto& b : p) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = static_cast<uint8_t>(x >> 24); }
    return p;
}

// Every offset must be recovered, because the fabric picks one arbitrarily and
// a receiver that only handles offset 0 decodes nothing three times in four.
void test_every_offset_recovers() {
    const int N = 20;
    for (int off = 0; off < 4; ++off) {
        Framer framer;
        std::vector<uint8_t> stream;
        std::vector<std::vector<uint8_t>> sent;
        for (int i = 0; i < N; ++i) {
            auto pay = makePayload(200 + i, static_cast<uint32_t>(i));
            auto w = framer.encode(pay, 0, ModCode::QPSK, BwCode::BW_5,
                                   7, static_cast<uint32_t>(i), nullptr, nullptr);
            stream.insert(stream.end(), w.begin(), w.end());
            sent.push_back(std::move(pay));
        }
        auto wire = delayBits(stream, off * 2);

        OffsetDeframer d;
        auto got = d.pushPacket(wire.data(), wire.size());

        assert(got.size() == static_cast<size_t>(N));
        for (int i = 0; i < N; ++i) {
            assert(got[static_cast<size_t>(i)].seq == static_cast<uint32_t>(i));
            assert(got[static_cast<size_t>(i)].payload == sent[static_cast<size_t>(i)]);
        }
        std::cout << "    offset " << off * 2 << " bits: " << got.size()
                  << "/" << N << " frames, payloads exact\n";
    }
}

// The same frame seen by two offsets is one reception, not two. Without the
// dedup an IP bridge would inject duplicate packets into the kernel.
void test_duplicates_are_dropped() {
    Framer framer;
    auto pay = makePayload(300, 99);
    auto w = framer.encode(pay, 0, ModCode::QPSK, BwCode::BW_5, 7, 42, nullptr, nullptr);

    OffsetDeframer d;
    auto a = d.pushPacket(w.data(), w.size());
    assert(a.size() == 1);
    // Feeding the identical bytes again is the same frame arriving twice.
    auto b = d.pushPacket(w.data(), w.size());
    assert(b.empty());
    assert(d.duplicates() >= 1);
    std::cout << "    duplicate seq suppressed (" << d.duplicates() << " dropped)\n";
}

// Flags must survive, because the bridge decides whether a frame is user data
// or a keepalive from FL_CTRL alone. Losing it would put filler on the TUN.
void test_flags_survive() {
    Framer framer;
    std::vector<uint8_t> ka(16, 0);
    auto w = framer.encode(ka, FL_CTRL, ModCode::QPSK, BwCode::BW_5, 7, 5, nullptr, nullptr);
    OffsetDeframer d;
    auto got = d.pushPacket(w.data(), w.size());
    assert(got.size() == 1);
    assert((got[0].flags & FL_CTRL) != 0);
    std::cout << "    FL_CTRL preserved through the offset decode\n";
}

// A corrupted frame must be rejected, not delivered. The CRC is the only thing
// standing between a bit error and a malformed IP packet in the kernel.
void test_corruption_rejected() {
    Framer framer;
    auto pay = makePayload(400, 7);
    auto w = framer.encode(pay, 0, ModCode::QPSK, BwCode::BW_5, 7, 11, nullptr, nullptr);
    w[w.size() / 2] ^= 0xFF;

    OffsetDeframer d;
    auto got = d.pushPacket(w.data(), w.size());
    for (auto& f : got) assert(f.payload != pay);   // never silently wrong
    assert(d.crcErrors() >= 1);
    std::cout << "    corrupted frame rejected (" << d.crcErrors() << " CRC failures)\n";
}

// realign() drops the last byte because it has no successor to borrow from.
// That is why decoding is scoped per DMA packet and a straddling frame is lost.
void test_realign_is_inverse_of_delay() {
    std::vector<uint8_t> t = makePayload(64, 3);
    for (int off = 1; off < 4; ++off) {
        auto delayed  = delayBits(t, off * 2);
        auto restored = realign(delayed.data(), delayed.size(), off * 2);
        assert(restored.size() >= t.size());
        assert(std::memcmp(restored.data(), t.data(), t.size()) == 0);
    }
    std::cout << "    realign() inverts the symbol-phase delay exactly\n";
}

} // namespace

void run_offsets() {
    std::cout << "  [offsets] four-offset deframing\n";
    test_realign_is_inverse_of_delay();
    test_every_offset_recovers();
    test_duplicates_are_dropped();
    test_flags_survive();
    test_corruption_rejected();
}
