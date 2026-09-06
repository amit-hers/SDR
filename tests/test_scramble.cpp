#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/framing/Scrambler.hpp"
#include "sdr/framing/Frame.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

// Longest run of identical bytes anywhere in a buffer. This is the quantity the
// differential modulator actually cares about: a constant byte is a constant
// phase INCREMENT, so a run of them holds the constellation still and the
// Gardner timing detector, which measures transitions, sees nothing to correct.
static size_t longestRun(const std::vector<uint8_t>& v) {
    size_t best = 0, run = 0;
    for (size_t k = 0; k < v.size(); ++k) {
        run = (k && v[k] == v[k-1]) ? run + 1 : 1;
        if (run > best) best = run;
    }
    return best;
}

static void test_involution() {
    std::vector<uint8_t> a(1024);
    for (size_t k = 0; k < a.size(); ++k) a[k] = (uint8_t)(k * 7 + 3);
    const std::vector<uint8_t> orig = a;
    sdr::Scrambler::apply(a.data(), a.size(), 12345);
    assert(a != orig);
    sdr::Scrambler::apply(a.data(), a.size(), 12345);
    assert(a == orig);
    std::cout << "  [scramble] involution: PASS\n";
}

static void test_seed_separation() {
    // Two frames of identical content must not go out as identical bytes, or a
    // repeating payload would still put a periodic pattern on the air.
    std::vector<uint8_t> a(256, 0x00), b(256, 0x00);
    sdr::Scrambler::apply(a.data(), a.size(), 1);
    sdr::Scrambler::apply(b.data(), b.size(), 2);
    assert(a != b);
    std::cout << "  [scramble] distinct seeds diverge: PASS\n";
}

// The measurement this whole mechanism exists for: 200 bytes of 0x00 lost
// 25.87% of frames over the air against 0.11% for pseudorandom content.
static void test_breaks_up_constant_runs() {
    const size_t N = 1200;
    std::vector<uint8_t> zeros(N, 0x00);
    assert(longestRun(zeros) == N);

    sdr::Framer framer;
    auto wire = framer.encode(zeros, 0, sdr::ModCode::QPSK, sdr::BwCode::BW_10, 7, 99);

    // Look only at the payload section. Preamble and postamble are deliberately
    // constant 0xAA, and that is harmless: 0xAA is symbol 0b10 four times over,
    // a phase increment of two -- the constellation alternates 180 degrees and
    // the detector gets a transition on every symbol.
    std::vector<uint8_t> body(wire.begin() + sdr::PREAMBLE_LEN + sdr::HEADER_SIZE,
                              wire.end() - sdr::POSTAMBLE_LEN);
    size_t run = longestRun(body);
    std::cout << "  [scramble] 1200 zero bytes -> longest wire run " << run << " B\n";
    // A 15-bit LFSR cannot emit more than 15 identical bits, so it cannot
    // produce even two identical bytes from a constant input.
    assert(run <= 2);
    std::cout << "  [scramble] constant run broken up: PASS\n";
}

static void test_roundtrip_zeros() {
    const std::vector<uint8_t> zeros(1200, 0x00);
    sdr::Framer framer;
    auto wire = framer.encode(zeros, 0, sdr::ModCode::QPSK, sdr::BwCode::BW_10, 7, 99);

    sdr::Deframer d;
    std::optional<sdr::DecodedFrame> r;
    for (uint8_t b : wire) { r = d.push(b, nullptr, nullptr); if (r) break; }
    assert(r.has_value());
    assert(r->payload == zeros);
    std::cout << "  [scramble] zero payload survives the round trip: PASS\n";
}

static void test_roundtrip_fec() {
    // Reed-Solomon zero-pads a short payload out to 223 bytes, which is the
    // other way this failure was reached on the air (34.14% frame loss at
    // 32 B + RS against 0.12% at 200 B + RS). Scrambling runs after FEC so it
    // covers that padding, and the rescue path has to reproduce the wire bytes
    // through AES and the scrambler to re-check the CRC.
    sdr::ReedSolomon rs;
    const std::vector<uint8_t> pay(32, 0x00);
    sdr::Framer framer;
    auto wire = framer.encode(pay, sdr::FL_FEC, sdr::ModCode::QPSK,
                              sdr::BwCode::BW_10, 7, 5, &rs);

    std::vector<uint8_t> body(wire.begin() + sdr::PREAMBLE_LEN + sdr::HEADER_SIZE,
                              wire.end() - sdr::POSTAMBLE_LEN);
    assert(longestRun(body) <= 2);

    sdr::Deframer d;
    std::optional<sdr::DecodedFrame> r;
    for (uint8_t b : wire) { r = d.push(b, &rs, nullptr); if (r) break; }
    assert(r.has_value());
    assert(r->payload == pay);
    std::cout << "  [scramble] RS zero padding scrambled and recovered: PASS\n";
}

static void test_fec_still_rescues() {
    // Descrambling must not disturb Reed-Solomon's ability to repair: the
    // scrambler is additive, so a wire error stays one error rather than being
    // fed back through a register and multiplied.
    sdr::ReedSolomon rs;
    std::vector<uint8_t> pay(200);
    for (size_t k = 0; k < pay.size(); ++k) pay[k] = (uint8_t)(k * 31 + 7);
    sdr::Framer framer;
    auto wire = framer.encode(pay, sdr::FL_FEC, sdr::ModCode::QPSK,
                              sdr::BwCode::BW_10, 7, 11, &rs);

    // Ten byte errors inside the RS codeword; RS(255,223) corrects sixteen.
    size_t base = sdr::PREAMBLE_LEN + sdr::HEADER_SIZE + 40;
    for (int k = 0; k < 10; ++k) wire[base + k * 3] ^= 0xA5;

    sdr::Deframer d;
    std::optional<sdr::DecodedFrame> r;
    for (uint8_t b : wire) { r = d.push(b, &rs, nullptr); if (r) break; }
    assert(r.has_value());
    assert(r->payload == pay);
    assert(d.fecRescued() == 1);
    std::cout << "  [scramble] 10 wire errors still rescued by RS: PASS\n";
}

void run_scramble() {
    std::cout << "[scramble]\n";
    test_involution();
    test_seed_separation();
    test_breaks_up_constant_runs();
    test_roundtrip_zeros();
    test_roundtrip_fec();
    test_fec_still_rescues();
}
