#include "sdr/crypto/AESCipher.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <cstring>
#include <array>

static std::array<uint8_t,32> make_key() {
    std::array<uint8_t,32> k{};
    for (int i = 0; i < 32; ++i) k[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
    return k;
}

static void test_encrypt_decrypt() {
    auto key = make_key();
    sdr::AESCipher aes(key.data());

    std::vector<uint8_t> plain = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                   0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    auto cipher = plain;
    aes.crypt(cipher.data(), cipher.size(), 1);     // encrypt (ctr=1)

    assert(cipher != plain);                         // must have changed

    aes.crypt(cipher.data(), cipher.size(), 1);     // decrypt same ctr
    assert(cipher == plain);
    std::cout << "  [crypto] AES-256-CTR encrypt/decrypt: PASS\n";
}

static void test_different_counters() {
    auto key = make_key();
    sdr::AESCipher aes(key.data());

    std::vector<uint8_t> plain(64, 0xAB);
    auto c1 = plain; aes.crypt(c1.data(), c1.size(), 1);
    auto c2 = plain; aes.crypt(c2.data(), c2.size(), 2);

    assert(c1 != c2);    // different counters → different ciphertext
    std::cout << "  [crypto] different counters produce different ciphertext: PASS\n";
}

static void test_large_payload() {
    auto key = make_key();
    sdr::AESCipher aes(key.data());

    std::vector<uint8_t> plain(1400);
    for (size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<uint8_t>(i & 0xFF);

    auto data = plain;
    aes.crypt(data.data(), data.size(), 0xDEADBEEF);
    assert(data != plain);
    aes.crypt(data.data(), data.size(), 0xDEADBEEF);
    assert(data == plain);
    std::cout << "  [crypto] large payload (1400B): PASS\n";
}

void run_crypto() {
    std::cout << "[crypto tests]\n";
    test_encrypt_decrypt();
    test_different_counters();
    test_large_payload();
    std::cout << "[crypto tests] ALL PASS\n\n";
}
