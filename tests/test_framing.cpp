#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
#include "sdr/fec/ReedSolomon.hpp"
#include "sdr/framing/Frame.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include <string>

static void test_roundtrip() {
    const char* msg = "Hello, RF world!";
    std::vector<uint8_t> payload(reinterpret_cast<const uint8_t*>(msg),
                                  reinterpret_cast<const uint8_t*>(msg) + std::strlen(msg));

    sdr::Framer framer;
    auto encoded = framer.encode(payload, 0x00, sdr::ModCode::QPSK, sdr::BwCode::BW_10,
                                  0xDEADBEEF, 42);
    assert(!encoded.empty());

    sdr::Deframer deframer;
    std::optional<sdr::DecodedFrame> result;
    for (uint8_t b : encoded) {
        result = deframer.push(b, nullptr, nullptr);
        if (result) break;
    }
    assert(result.has_value());
    assert(result->payload == payload);
    assert(result->node_id == 0xDEADBEEF);
    assert(result->seq == 42);
    std::cout << "  [framing] roundtrip: PASS\n";
}

static void test_corrupt_crc() {
    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    sdr::Framer framer;
    auto encoded = framer.encode(payload, 0, sdr::ModCode::BPSK, sdr::BwCode::BW_10, 1, 0);

    // Flip a byte in the payload area
    encoded[encoded.size() - 6] ^= 0xFF;

    sdr::Deframer deframer;
    std::optional<sdr::DecodedFrame> result;
    for (uint8_t b : encoded)
        result = deframer.push(b, nullptr, nullptr);

    assert(!result.has_value());
    std::cout << "  [framing] corrupt CRC rejected: PASS\n";
}

static void test_large_payload() {
    std::vector<uint8_t> payload(sdr::MAX_PAYLOAD, 0xAB);
    sdr::Framer framer;
    auto encoded = framer.encode(payload, 0, sdr::ModCode::QAM16, sdr::BwCode::BW_10, 7, 99);

    sdr::Deframer deframer;
    std::optional<sdr::DecodedFrame> result;
    for (uint8_t b : encoded) {
        result = deframer.push(b, nullptr, nullptr);
        if (result) break;
    }
    assert(result.has_value());
    assert(result->payload == payload);
    std::cout << "  [framing] max payload roundtrip: PASS\n";
}

static void test_multi_frame() {
    sdr::Framer framer;
    sdr::Deframer deframer;

    std::vector<uint8_t> stream;
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> pl = {static_cast<uint8_t>(i), static_cast<uint8_t>(i+1)};
        auto f = framer.encode(pl, 0, sdr::ModCode::BPSK, sdr::BwCode::BW_10, 1,
                               static_cast<uint32_t>(i));
        stream.insert(stream.end(), f.begin(), f.end());
    }

    int decoded = 0;
    for (uint8_t b : stream) {
        auto res = deframer.push(b, nullptr, nullptr);
        if (res) ++decoded;
    }
    assert(decoded == 3);
    std::cout << "  [framing] multi-frame stream: PASS\n";
}

// Reed-Solomon must run BEFORE the CRC verdict, not after it.
//
// The deframer used to reject on CRC mismatch and only then reach the FEC
// decode, so RS ran exclusively on frames that were already intact and could
// never repair anything. This pins the corrected behaviour: a frame with
// damage inside the RS-protected payload is recovered byte-exact and counted
// as a rescue, damage beyond RS capacity is still rejected, and an undamaged
// frame is not counted as rescued.
static void test_fec_rescue() {
    sdr::ReedSolomon rs;
    std::vector<uint8_t> payload(600);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 7 + 3);

    struct Case { int nerr; bool recover; bool rescued; };
    const Case cases[] = {
        {0,  true,  false},   // intact: recovered, but not a "rescue"
        {1,  true,  true },
        {8,  true,  true },
        {16, true,  true },
        {40, false, false},   // beyond RS capacity
    };

    for (const auto& c : cases) {
        sdr::Framer fr;
        auto frame = fr.encode(payload.data(), payload.size(), sdr::FL_FEC,
                               sdr::ModCode::QPSK, sdr::mhzToBw(1), 0x1234, 42, &rs, nullptr);
        const size_t start = sdr::PREAMBLE_LEN + sdr::HEADER_SIZE + 10;
        for (int k = 0; k < c.nerr && start + size_t(k) * 7 < frame.size() - 8; ++k)
            frame[start + size_t(k) * 7] ^= 0xFF;

        sdr::Deframer df;
        bool exact = false;
        for (uint8_t b : frame) {
            auto r = df.push(b, &rs, nullptr);
            if (r) { exact = (r->payload == payload); break; }
        }
        assert(exact == c.recover);
        assert((df.fecRescued() > 0) == c.rescued);
    }
    std::cout << "  [framing] Reed-Solomon rescues corrupted frames: PASS\n";
}

void run_framing() {
    std::cout << "[framing tests]\n";
    test_roundtrip();
    test_corrupt_crc();
    test_large_payload();
    test_multi_frame();
    test_fec_rescue();
    std::cout << "[framing tests] ALL PASS\n\n";
}
