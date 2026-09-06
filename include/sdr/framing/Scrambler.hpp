#pragma once
#include <cstdint>
#include <cstddef>

namespace sdr {

// Energy dispersal for the payload section of a frame.
//
// WHY THIS IS NOT OPTIONAL. The fabric modulator encodes QPSK differentially,
// so a byte value maps to a PHASE INCREMENT, not to a phase. A run of identical
// bytes is therefore a run of identical symbols: the constellation stops moving
// and the Gardner timing detector, which measures transitions, has nothing left
// to work with. Measured over the air at 434 MHz, two frames alike in length,
// wire size and position, differing only in payload content:
//
//     200 bytes pseudorandom ... frame loss  0.11%
//     200 bytes of 0x00 ....... frame loss 25.87%
//
// Reed-Solomon arrives at the same failure from the other side, since it
// zero-pads a short payload out to 223 bytes: 32 B + RS measured 34.14% against
// 0.12% for 200 B + RS. H.264 is full of zero runs, so without this a video
// stream loses a quarter of its frames to its own quiet passages.
//
// ADDITIVE, NOT SELF-SYNCHRONISING. A multiplicative scrambler needs no seed
// agreement but feeds each error back through the register, turning one wire
// error into three. The sequence number is already in the (unscrambled) header,
// so both ends can derive the same seed and pay no error multiplication at all.
// Applied AFTER Reed-Solomon and AES so that it covers RS's zero padding, and
// before the CRC so the CRC still verifies exactly the bytes that were sent.
//
// Involutive: scrambling twice returns the original, so one function serves
// both directions.
class Scrambler {
public:
    // 15-bit maximal-length LFSR, x^15 + x^14 + 1 -- the DVB-S energy dispersal
    // polynomial. Its longest constant output run is 15 bits, about 7 QPSK
    // symbols, against the 764 that broke the timing loop.
    static void apply(uint8_t* p, size_t n, uint32_t seq) {
        // Any seed will do except zero, which is the LFSR's dead state and
        // would emit an unbroken run of the very kind this exists to prevent.
        uint16_t r = static_cast<uint16_t>((seq * 0x9E37u) ^ 0x4A80u) & 0x7FFF;
        if (r == 0) r = 0x7FFF;
        for (size_t k = 0; k < n; ++k) {
            uint8_t m = 0;
            for (int b = 0; b < 8; ++b) {
                uint16_t fb = ((r >> 14) ^ (r >> 13)) & 1u;
                m = static_cast<uint8_t>((m << 1) | fb);
                r = static_cast<uint16_t>(((r << 1) | fb) & 0x7FFF);
            }
            p[k] ^= m;
        }
    }
};

} // namespace sdr
