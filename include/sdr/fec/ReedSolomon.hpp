#pragma once
#include <vector>
#include <cstdint>
#include <liquid/liquid.h>

namespace sdr {

// RS(255,223): corrects up to 16 byte errors per 255-byte block.
// Overhead: (255-223)/223 ≈ 14.3%.
// Uses liquid-dsp fec_create(LIQUID_FEC_RS_M8).
class ReedSolomon {
public:
    ReedSolomon();
    ~ReedSolomon();
    ReedSolomon(const ReedSolomon&)            = delete;
    ReedSolomon& operator=(const ReedSolomon&) = delete;

    // Pad data to multiple of 223, output multiple of 255.
    std::vector<uint8_t> encode(const uint8_t* data, size_t len) const;

    // Decode; corrects burst errors. Returns original payload.
    // Throws std::runtime_error if error is uncorrectable.
    std::vector<uint8_t> decode(const uint8_t* data, size_t len) const;

    // Encoded size for a given raw length.
    static size_t encodedSize(size_t raw_len);
    // Decoded (original) size for a given encoded length.
    static size_t decodedSize(size_t enc_len);

    static constexpr int BLOCK_IN  = 223;
    static constexpr int BLOCK_OUT = 255;

private:
    fec enc_;
    fec dec_;
};

} // namespace sdr
