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
