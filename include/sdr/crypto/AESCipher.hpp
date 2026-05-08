#pragma once
#include <cstdint>
#include <array>
#include <stdexcept>

namespace sdr {

// AES-256-CTR stream cipher. Key is 32 bytes.
// Counter is the frame sequence number (64-bit, big-endian IV).
// encrypt() and decrypt() are the same operation in CTR mode.
class AESCipher {
public:
    static constexpr size_t KEY_SIZE = 32;

    explicit AESCipher(const uint8_t* key);  // KEY_SIZE bytes
    AESCipher() = default;

    void setKey(const uint8_t* key);  // KEY_SIZE bytes

    // In-place CTR encrypt/decrypt. ctr is the frame sequence number used as IV.
    void crypt(uint8_t* data, size_t len, uint64_t ctr) const;

    bool hasKey() const { return has_key_; }

private:
    std::array<uint8_t, KEY_SIZE> key_{};
    bool has_key_{false};
};

} // namespace sdr
