// fuzz_decoders -- hostile input for the two decoders that take it from
// outside: the state protocol (from the network) and the deframer (from the
// air). Everything else in this project consumes data it produced itself.
//
//   ./sdr-fuzz [iterations]      exit 0 = clean, 1 = something was accepted
//
// Build it with sanitizers to get the memory-safety half of the check:
//   cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
//     -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
//
// Two properties are asserted, and they are the ones that matter for a radio
// that listens to whatever is on the air:
//   * no crafted or corrupted input is ever ACCEPTED as valid;
//   * pure noise never produces a frame, however long it is fed.
// Recovering frames from *corrupted real frames* is not a failure -- that is
// Reed-Solomon working, and it is counted separately so the two cannot be
// confused.
#include "sdr/telemetry/StatePacket.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/framing/Framer.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include "sdr/framing/Aggregate.hpp"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>
using namespace sdr;

int main(int argc, char** argv) {
    const int iters = argc > 1 ? atoi(argv[1]) : 20000;
    std::mt19937 rng(12345);
    ReedSolomon rs;
    size_t accepted = 0, frames_noise = 0, frames_corrupt = 0, rescued = 0;

    // 1. Wholly random buffers of every plausible length.
    for (int i = 0; i < iters; ++i) {
        size_t n = rng() % 2048;
        std::vector<uint8_t> b(n);
        for (auto& x : b) x = uint8_t(rng());
        if (StatePacket::decode(b.data(), b.size())) ++accepted;
    }
    // 2. Valid packets with N random bytes corrupted -- the case that matters,
    //    since a decoder usually rejects noise and fails on near-miss input.
    StatePacket s; s.node_id = 0x1234; s.freq_tx_hz = 434000000; s.rssi_cdbm = -6000;
    auto good = s.encode();
    for (int i = 0; i < iters; ++i) {
        auto b = good;
        int nmut = 1 + int(rng() % 6);
        // Mutations must be at DISTINCT bit positions. Picking positions freely
        // lets two flips land on the same bit and cancel, leaving the packet
        // untouched -- which then "accepts" and reads as a CRC weakness that is
        // really a fault in the fuzzer. That artefact produced 6 apparent
        // accepts in an earlier run.
        std::vector<size_t> used;
        for (int m = 0; m < nmut; ++m) {
            size_t bit;
            do { bit = rng() % (b.size() * 8); }
            while (std::find(used.begin(), used.end(), bit) != used.end());
            used.push_back(bit);
            b[bit / 8] ^= uint8_t(1u << (bit % 8));
        }
        if (StatePacket::decode(b.data(), b.size())) ++accepted;
    }
    // 3. Truncations and over-long length fields.
    for (int i = 0; i < iters/4; ++i) {
        auto b = good;
        b.resize(rng() % (good.size() + 1));
        if (!b.empty()) {
            if (b.size() > 7) { b[6] = uint8_t(rng()); b[7] = uint8_t(rng()); }
            if (StatePacket::decode(b.data(), b.size())) ++accepted;
        }
    }
    // 4. The deframer, byte by byte, on pure noise. It must never return a
    //    frame it cannot verify, and never index outside its buffers.
    {
        Deframer d;
        for (int i = 0; i < iters * 8; ++i)
            if (d.push(uint8_t(rng()), &rs, nullptr)) ++frames_noise;
    }
    // 5. The deframer on a real frame with random corruption.
    {
        Framer f;
        std::vector<uint8_t> pay(600);
        for (auto& x : pay) x = uint8_t(rng());
        auto w = f.encode(pay.data(), pay.size(), FL_FEC, ModCode::QPSK,
                          BwCode::BW_10, 7, 1, &rs, nullptr);
        for (int i = 0; i < iters/10; ++i) {
            auto b = w;
            int nmut = 1 + int(rng() % 40);
            for (int m = 0; m < nmut; ++m) b[rng() % b.size()] ^= uint8_t(1u << (rng() % 8));
            Deframer d;
            for (uint8_t byte : b) if (d.push(byte, &rs, nullptr)) ++frames_corrupt;
            rescued += d.fecRescued();
        }
    }
    // 6. aggregate::split walks length-prefixed records out of a payload that
    //    arrived over the air. A CRC-valid frame can still carry a malformed
    //    aggregate -- from a buggy sender, or a Reed-Solomon repair that
    //    produced a codeword the CRC happens to accept -- so it must never read
    //    past the buffer or loop forever on a zero length.
    size_t records = 0;
    for (int i = 0; i < iters; ++i) {
        size_t n = rng() % 2048;
        std::vector<uint8_t> b(n);
        for (auto& x : b) x = uint8_t(rng());
        aggregate::split(b.data(), b.size(), [&](const uint8_t* p, size_t len) {
            // Every emitted record must lie wholly inside the buffer.
            assert(p >= b.data() && p + len <= b.data() + b.size());
            ++records;
        });
    }
    std::printf("aggregate records emitted from noise:     %zu   (bounds asserted)\n", records);
    std::printf("StatePacket accepted from hostile input: %zu   (must be 0)\n", accepted);
    std::printf("frames returned from PURE NOISE:          %zu   (must be 0)\n", frames_noise);
    std::printf("frames recovered from corrupted frames:   %zu   (of which %zu RS-rescued)\n",
                frames_corrupt, rescued);
    if (accepted || frames_noise) {
        std::printf("FAIL: a decoder accepted input it could not verify\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
