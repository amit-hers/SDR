#include "sdr/framing/Framer.hpp"
#include "sdr/framing/Deframer.hpp"
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

void run_framing() {
    std::cout << "[framing tests]\n";
    test_roundtrip();
    test_corrupt_crc();
    test_large_payload();
    test_multi_frame();
    std::cout << "[framing tests] ALL PASS\n\n";
}
