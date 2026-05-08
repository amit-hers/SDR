#pragma once
#include "Frame.hpp"
#include <optional>
#include <array>

namespace sdr {

class ReedSolomon;
class AESCipher;

// Lock-free byte-by-byte state machine.
// Feed each demodulated byte via push(); a complete, CRC-verified frame is
// returned as DecodedFrame when the sync word + header + payload + CRC all match.
class Deframer {
public:
    Deframer() = default;

    // Returns a decoded frame on success, std::nullopt while still collecting.
    // Throws std::runtime_error on an unrecoverable CRC mismatch (caller may log & continue).
    std::optional<DecodedFrame> push(uint8_t byte,
                                     const ReedSolomon* fec = nullptr,
                                     const AESCipher*   aes = nullptr);

    void reset();

    uint64_t crcErrors()  const { return crc_errors_; }
    uint64_t goodFrames() const { return good_frames_; }

private:
    enum class State : uint8_t { HUNT, HEADER, PAYLOAD };

    State    state_     {State::HUNT};
    uint32_t shift_     {0};
    uint8_t  hdr_buf_   [HEADER_SIZE]{};
    int      hdr_pos_   {0};
    uint16_t plen_      {0};
    uint8_t  flags_     {0};
    ModCode  mod_       {ModCode::QPSK};
    BwCode   bw_        {BwCode::BW_10};
    uint32_t node_id_   {0};
    uint32_t seq_       {0};

    std::array<uint8_t, MAX_PAYLOAD + 4 + 255> payload_buf_{};  // +4 CRC, +255 RS slack
    int      pay_pos_   {0};
    int      pay_total_ {0};  // plen + CRC_SIZE (+ RS padding if FL_FEC)

    uint64_t crc_errors_  {0};
    uint64_t good_frames_ {0};
};

} // namespace sdr
