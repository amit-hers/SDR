#pragma once
#include "Frame.hpp"
#include <cstdint>
#include <vector>

namespace sdr {

class ReedSolomon;
class AESCipher;

class Framer {
public:
    // Encode payload into a complete on-wire frame.
    // If fec != nullptr and FL_FEC is set in flags, RS-encodes the payload first.
    // If aes != nullptr and FL_ENCRYPT is set, AES-256-CTR encrypts (uses seq as ctr).
    std::vector<uint8_t> encode(const uint8_t* payload, size_t payload_len,
                                uint8_t  flags,
                                ModCode  mod,
                                BwCode   bw,
                                uint32_t node_id,
                                uint32_t seq,
                                const ReedSolomon* fec = nullptr,
                                const AESCipher*   aes = nullptr);

    // Convenience overload for vectors.
    std::vector<uint8_t> encode(const std::vector<uint8_t>& payload,
                                uint8_t  flags,
                                ModCode  mod,
                                BwCode   bw,
                                uint32_t node_id,
                                uint32_t seq,
                                const ReedSolomon* fec = nullptr,
                                const AESCipher*   aes = nullptr) {
        return encode(payload.data(), payload.size(),
                      flags, mod, bw, node_id, seq, fec, aes);
    }
};

} // namespace sdr
