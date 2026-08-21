#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace sdr {

// ── Sync word ─────────────────────────────────────────────────────────────
static constexpr uint32_t FRAME_SYNC   = 0xC0FFEE77U;
static constexpr uint8_t  FRAME_VER    = 0x03;
static constexpr size_t   HEADER_SIZE  = 18;   // sync+ver+flags+mod+bw+nodeid+seq+len
static constexpr size_t   CRC_SIZE     = 4;
static constexpr size_t   MAX_PAYLOAD  = 1400; // bytes (sets TAP MTU = 1400-14 = 1386)
static constexpr size_t   FRAME_OVERHEAD = HEADER_SIZE + CRC_SIZE; // 22 bytes

// ── Preamble ──────────────────────────────────────────────────────────────
// Every burst is transmitted independently (no continuous carrier between
// frames), so AGC/TimingSync/CostasLoop start cold on each one. Measured
// settling transient is ~8 bytes even under ideal conditions — longer than
// the 4-byte sync word itself — so without a training run before it, the
// sync word can never survive intact. Framer::encode() prepends this many
// PREAMBLE_BYTE bytes; Deframer's HUNT state already tolerates and skips
// arbitrary leading bytes while searching for FRAME_SYNC, so no deframer
// change is needed. Generous margin over the measured ~8-byte minimum.
static constexpr size_t  PREAMBLE_LEN    = 32;
static constexpr uint8_t PREAMBLE_BYTE   = 0xAA;

// Each burst is also independently transmitted with nothing following it —
// the matched-filter/decimator chain (RRC taps + symsync) has group delay,
// so without trailing padding the last few bytes (including the CRC) can be
// clipped off the end of the burst. Smaller than PREAMBLE_LEN: this only
// needs to flush the filter pipeline, not let control loops converge.
static constexpr size_t  POSTAMBLE_LEN   = 16;

static constexpr size_t  WIRE_FRAME_OVERHEAD =
    PREAMBLE_LEN + FRAME_OVERHEAD + POSTAMBLE_LEN;

// ── Flags byte ────────────────────────────────────────────────────────────
static constexpr uint8_t FL_ENCRYPT = 0x01;
static constexpr uint8_t FL_FEC     = 0x02;
static constexpr uint8_t FL_ACK     = 0x04;
static constexpr uint8_t FL_CTRL    = 0x08;

// ── Modulation codes ──────────────────────────────────────────────────────
enum class ModCode : uint8_t {
    BPSK  = 1,
    QPSK  = 2,
    QAM16 = 3,
    QAM64 = 4,
    AUTO  = 0xFF,
};

// ── Split-modulation frame layout ─────────────────────────────────────────
// A burst is transmitted in two sections with *different* modulations:
//
//   [ PREAMBLE ][ SYNC ][ HEADER ] [ PAYLOAD ][ CRC ][ POSTAMBLE ]
//   \___________ always BPSK ____/ \______ header-declared mod ___/
//
// The acquisition section must be a fixed, known waveform, because the
// receiver has to find and decode it *before* it can learn what the payload
// modulation is -- the mod code lives in the header. Modulating the preamble
// with the payload's scheme is a bootstrap paradox: PreambleSync correlates
// against a BPSK reference, so a QPSK/16QAM/64QAM preamble scores at the
// noise floor (measured: 1.000 for BPSK vs 0.071/0.061/0.052) and the frame
// is never seen at all.
//
// BPSK is 1 bit/symbol, so the acquisition section is exactly
// BOOTSTRAP_BYTES*8 symbols and the payload section starts on an exact
// symbol boundary regardless of the payload's bits/symbol.
static constexpr size_t BOOTSTRAP_BYTES = PREAMBLE_LEN + HEADER_SIZE;   // 50
static constexpr size_t BOOTSTRAP_SYMS  = BOOTSTRAP_BYTES * 8;          // 400

// Symbols from the first sync-word symbol to the first payload symbol.
// (HEADER_SIZE includes the 4-byte sync word.)
static constexpr size_t HEADER_SYMS = HEADER_SIZE * 8;                  // 144

inline int modCodeBits(ModCode m) {
    switch (m) {
        case ModCode::BPSK:  return 1;
        case ModCode::QPSK:  return 2;
        case ModCode::QAM16: return 4;
        case ModCode::QAM64: return 6;
        default:             return 0;   // AUTO/unknown: not a wire scheme
    }
}

inline const char* modCodeName(ModCode m) {
    switch (m) {
        case ModCode::BPSK:  return "BPSK";
        case ModCode::QPSK:  return "QPSK";
        case ModCode::QAM16: return "16QAM";
        case ModCode::QAM64: return "64QAM";
        case ModCode::AUTO:  return "AUTO";
    }
    return "?";
}

// A concrete scheme that can actually appear on the wire (AUTO cannot).
inline bool isWireMod(ModCode m) { return modCodeBits(m) > 0; }

// Symbols needed to carry n bytes at the given modulation.
inline size_t symsForBytes(size_t n_bytes, ModCode m) {
    int bps = modCodeBits(m);
    if (bps <= 0) return 0;
    return (n_bytes * 8 + static_cast<size_t>(bps) - 1) / static_cast<size_t>(bps);
}

// ── Bandwidth codes ───────────────────────────────────────────────────────
// HamGeek Pluto+ (XC7z020 + AD9363 in AD9361 mode): up to 61.44 MSPS
enum class BwCode : uint8_t {
    BW_1P25 = 0,
    BW_2P5  = 1,
    BW_5    = 2,
    BW_10   = 3,
    BW_20   = 4,
    BW_40   = 5,   // HamGeek Pluto+ only
};

inline long long bwToSps(BwCode bw) {
    switch (bw) {
        case BwCode::BW_1P25: return  1'250'000LL;
        case BwCode::BW_2P5:  return  2'500'000LL;
        case BwCode::BW_5:    return  5'000'000LL;
        case BwCode::BW_10:   return 10'000'000LL;
        case BwCode::BW_40:   return 40'000'000LL;
        default:              return 20'000'000LL;
    }
}

inline BwCode mhzToBw(int mhz) {
    if (mhz <= 1)  return BwCode::BW_1P25;
    if (mhz <= 2)  return BwCode::BW_2P5;
    if (mhz <= 5)  return BwCode::BW_5;
    if (mhz <= 10) return BwCode::BW_10;
    if (mhz <= 20) return BwCode::BW_20;
    return BwCode::BW_40;
}

// ── Decoded frame ─────────────────────────────────────────────────────────
struct DecodedFrame {
    std::vector<uint8_t> payload;
    uint8_t  flags{0};
    ModCode  mod{ModCode::QPSK};
    BwCode   bw{BwCode::BW_10};
    uint32_t node_id{0};
    uint32_t seq{0};
};

} // namespace sdr
