#pragma once
#include "sdr/framing/Frame.hpp"
#include <complex>
#include <functional>
#include <cstdint>
#include <vector>

namespace sdr {

class ReedSolomon;

// Modulates and demodulates the split-modulation burst layout described in
// Frame.hpp: a fixed-BPSK acquisition section (preamble + sync + header)
// followed by a payload section whose scheme is declared *inside* that header.
//
// This is what makes adaptive modulation possible at all. With a single
// scheme for the whole burst, the receiver would need to already know the
// payload modulation in order to read the field that tells it the payload
// modulation.
class SplitModem {
public:
    // ── TX ────────────────────────────────────────────────────────────────
    // `frame` is exactly what Framer::encode() returned. Bytes [0,
    // BOOTSTRAP_BYTES) go out as BPSK; everything after (payload, CRC,
    // postamble) uses payload_mod.
    //
    // Throws std::invalid_argument if payload_mod is not a wire scheme, so a
    // stray ModCode::AUTO becomes a loud failure rather than a silently
    // unreceivable transmission.
    static void modulate(const std::vector<uint8_t>& frame,
                         ModCode payload_mod,
                         std::vector<std::complex<float>>& syms);

    // ── RX ────────────────────────────────────────────────────────────────
    struct Result {
        bool     found       {false};  // sync word located
        bool     header_ok   {false};  // version/length/mod all sane
        bool     complete    {false};  // enough symbols present for the payload
        bool     inverted    {false};  // sync arrived on the opposite BPSK phase
        size_t   sync_sym    {0};      // symbol index of the first sync symbol
        size_t   end_sym     {0};      // one past the last payload symbol
        ModCode  payload_mod {ModCode::BPSK};
        uint8_t  flags       {0};
        uint16_t plen        {0};

        // Canonical (de-inverted) bytes from the sync word through the CRC,
        // ready to push into a Deframer.
        std::vector<uint8_t> bytes;
    };

    // Hook applied to the payload symbols after they are extracted (and
    // de-rotated for a 180-degree error) but before they reach the modem.
    //
    // This exists so carrier tracking can run on the payload *alone*. A
    // decision-directed loop needs a detector matched to the constellation
    // it is tracking, and the acquisition section is always BPSK while the
    // payload may not be -- running one loop across both feeds it symbols
    // its detector is wrong for. Measured on hardware: a Costas loop run
    // over the whole stream took QPSK from 25.3% CRC (open-loop LS only)
    // down to 9.7%, while being essential for BPSK.
    using PayloadTap = std::function<void(std::vector<std::complex<float>>&)>;

    // Demodulates one frame starting within the first `max_search_syms`
    // symbols. `fec_enabled` must match the receiver's FEC configuration --
    // it decides whether the header's payload length refers to pre- or
    // post-RS bytes, and therefore how many payload symbols to consume.
    static Result demodulate(const std::vector<std::complex<float>>& syms,
                             bool   fec_enabled,
                             size_t max_search_syms = 512,
                             const PayloadTap& payload_tap = {});

private:
    // Demodulates `n` symbols as BPSK into a bit vector (1 symbol = 1 bit).
    static void bpskBits(const std::complex<float>* syms, size_t n,
                         std::vector<uint8_t>& bits);
};

} // namespace sdr
