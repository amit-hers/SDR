#pragma once
#include <cstdint>
#include <cstddef>

namespace sdr::dtc {

// Host <-> radio wire protocol for a DTC-style radio.
//
// Runs over the existing Ethernet link to the radio, alongside (not through)
// libiio. Deliberately small and fixed-layout: the radio side of this is
// firmware on a Cortex-A9 feeding fabric, so parsing must be trivial and
// allocation-free.
//
// Design rules, each of which exists because the IQ-streaming path got it
// wrong at least once:
//
//   * Every message carries a length. A truncated transfer must be detectable,
//     not silently absorbed -- txPush() clamping to its buffer and discarding
//     the remainder cost 3300 frames a run before it was found.
//   * Data and control are separate streams. Control must not queue behind a
//     burst of frames, or link adaptation cannot react while the link is busy.
//   * The radio reports its own accounting. The host must be able to check
//     offered == on_air + dropped without trusting a single aggregate number.
//   * Sequence numbers are end-to-end and echoed. They are the only key that
//     joins what the host offered to what the peer received.

static constexpr uint32_t DTC_MAGIC   = 0x44544301u;  // "DTC\x01"
static constexpr uint16_t DTC_VERSION = 1;

// Data plane is UDP; control plane is TCP. Data tolerates loss (a lost frame
// is indistinguishable from one the air lost), control does not.
static constexpr uint16_t DTC_DATA_PORT = 30001;
static constexpr uint16_t DTC_CTRL_PORT = 30002;

enum class MsgType : uint16_t {
    // ── data plane ───────────────────────────────────────────────────────
    TX_FRAME   = 0x0001,  // host -> radio: payload to transmit
    RX_FRAME   = 0x0002,  // radio -> host: decoded payload
    TX_CREDIT  = 0x0003,  // radio -> host: buffer space freed, N frames
    // ── control plane ────────────────────────────────────────────────────
    CONFIGURE  = 0x0100,  // host -> radio: apply Config, atomically
    CONFIG_ACK = 0x0101,  // radio -> host: applied, or rejected with reason
    STATS_REQ  = 0x0102,
    STATS_RSP  = 0x0103,
    LOG        = 0x0104,  // radio -> host: diagnostic text, rate-limited
};

// Every message begins with this. Fixed 16 bytes, little-endian, no padding.
struct __attribute__((packed)) Header {
    uint32_t magic;       // DTC_MAGIC
    uint16_t version;     // DTC_VERSION
    uint16_t type;        // MsgType
    uint32_t length;      // bytes FOLLOWING this header
    uint32_t seq;         // frame seq for data; request id for control
};
static_assert(sizeof(Header) == 16, "DTC header must be exactly 16 bytes");

// TX_FRAME body: header then payload bytes. The radio does the framing,
// preamble, sync word, CRC, FEC, encryption, modulation and shaping -- the
// host sends payload only, which is the whole point.
struct __attribute__((packed)) TxFrame {
    uint8_t  flags;       // FL_* from Frame.hpp
    uint8_t  reserved[3];
    uint16_t payload_len;
    uint16_t _pad;
    // payload_len bytes follow
};
static_assert(sizeof(TxFrame) == 8, "TxFrame body must be 8 bytes");

// RX_FRAME body: what the radio decoded, plus the per-frame quality the host
// needs for link adaptation. Sending these means the host never has to see a
// sample to decide whether to change modulation.
struct __attribute__((packed)) RxFrame {
    uint8_t  flags;
    uint8_t  mod;         // ModCode the payload actually used
    uint16_t payload_len;
    uint32_t node_id;
    float    rssi_dbm;
    float    snr_db;
    float    evm_pct;     // payload EVM, measured against ITS OWN constellation
    uint32_t burst_id;    // which detected burst this came from
    // payload_len bytes follow
};
static_assert(sizeof(RxFrame) == 24, "RxFrame body must be 24 bytes");

// STATS_RSP body. Mirrors IFramePhy::Stats. The three frame counters must
// satisfy offered == on_air + dropped; a radio that cannot show that is not
// accepted, because unaccounted loss is precisely what took longest to find
// in the host implementation.
struct __attribute__((packed)) Stats {
    uint64_t frames_offered;
    uint64_t frames_on_air;
    uint64_t frames_dropped;
    uint64_t frames_decoded;
    uint64_t frames_crc_failed;
    uint64_t bursts_detected;
    uint64_t bytes_delivered;
    uint64_t samples_generated;   // fabric-side, for the same invariant
    uint64_t samples_on_air;
    uint64_t samples_dropped;
    float    tx_duty_pct;
    float    snr_db;
    float    rssi_dbm;
    float    temp_c;
};

// Largest DTC datagram: header + RxFrame + a maximum payload. Sized so one
// frame always fits in one datagram -- reassembly on the radio side would be
// state the fabric does not need to carry.
static constexpr size_t MAX_MSG =
    sizeof(Header) + sizeof(RxFrame) + 1400 /* MAX_PAYLOAD */;
static_assert(MAX_MSG < 1500 + 64, "a DTC frame should fit a jumbo-free path");

} // namespace sdr::dtc
