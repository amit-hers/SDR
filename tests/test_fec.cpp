#include "sdr/fec/ReedSolomon.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <cstring>

static void test_clean_roundtrip() {
    sdr::ReedSolomon rs;
    const char* msg = "Reed-Solomon FEC test payload 1234567890";
    std::vector<uint8_t> data(reinterpret_cast<const uint8_t*>(msg),
                               reinterpret_cast<const uint8_t*>(msg) + std::strlen(msg));

    auto encoded = rs.encode(data.data(), data.size());
    auto decoded = rs.decode(encoded.data(), encoded.size());
    assert(decoded == data);
    std::cout << "  [fec] clean roundtrip: PASS\n";
}

static void test_error_correction() {
    sdr::ReedSolomon rs;
    std::vector<uint8_t> data(100, 0x55);
    auto encoded = rs.encode(data.data(), data.size());

    // Inject 15 byte errors (≤16 correctable per 255-byte block)
    for (int i = 0; i < 15; ++i)
        encoded[static_cast<size_t>(i * 3)] ^= 0xFF;

    auto decoded = rs.decode(encoded.data(), encoded.size());
    assert(decoded == data);
    std::cout << "  [fec] 15-error correction: PASS\n";
}

static void test_full_block() {
    sdr::ReedSolomon rs;
    // Exactly one block (223 bytes)
    std::vector<uint8_t> data(sdr::ReedSolomon::BLOCK_IN, 0xAA);
    auto enc = rs.encode(data.data(), data.size());
    assert(enc.size() == sdr::ReedSolomon::BLOCK_OUT);
    auto dec = rs.decode(enc.data(), enc.size());
    assert(dec == data);
    std::cout << "  [fec] full block (223B): PASS\n";
}

static void test_multi_block() {
    sdr::ReedSolomon rs;
    // 500 bytes → 3 blocks
    std::vector<uint8_t> data(500);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i & 0xFF);
    auto enc = rs.encode(data.data(), data.size());
    auto dec = rs.decode(enc.data(), enc.size());
    assert(dec == data);
    std::cout << "  [fec] multi-block (500B → 3 blocks): PASS\n";
}

void run_fec() {
    std::cout << "[fec tests]\n";
    test_clean_roundtrip();
    test_error_correction();
    test_full_block();
    test_multi_block();
    std::cout << "[fec tests] ALL PASS\n\n";
}
