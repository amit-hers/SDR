#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <optional>

namespace sdr {

// SDR state report -- one datagram describing what the radio is doing.
//
// Carried two ways, deliberately the same bytes in both:
//   * over UDP to a local or remote operator (see TelemetryServer);
//   * over the RF link itself as a frame with FL_CTRL set, where it is already
//     inside a CRC-checked frame.
//
// FIXED LITTLE-ENDIAN LAYOUT, WRITTEN FIELD BY FIELD. Not a memcpy of a struct:
// struct padding and host endianness are not part of a wire format, and a
// report that only decodes on the machine that produced it is not a protocol.
//
// EXTENSIBILITY IS THE POINT OF `body_len`. A newer sender may append fields; an
// older receiver reads the ones it knows and skips the remainder, rather than
// rejecting the packet. So fields are only ever APPENDED, never reordered or
// resized, and `PROTO_VERSION` rises only on a change that breaks that promise.
struct StatePacket {
    static constexpr uint32_t MAGIC         = 0x53445253;  // "SDRS"
    static constexpr uint16_t PROTO_VERSION = 1;
    // magic(4) + version(2) + body_len(2) + body + crc32(4)
    static constexpr size_t   HEADER_BYTES  = 8;
    static constexpr size_t   CRC_BYTES     = 4;
    // Generous ceiling; a report is ~100 B. Bounds the decoder against a
    // hostile or corrupt length field.
    static constexpr size_t   MAX_BODY      = 1024;

    // ── identity ──────────────────────────────────────────────────────────
    uint32_t node_id   {0};
    uint32_t uptime_s  {0};
    uint8_t  mode      {0};     // 0 unknown, 1 bridge, 2 mesh, 3 p2p-tx, 4 p2p-rx, 5 scan

    // ── radio configuration ───────────────────────────────────────────────
    uint32_t freq_tx_hz {0};
    uint32_t freq_rx_hz {0};
    uint32_t bw_hz      {0};
    uint8_t  sps        {0};
    uint8_t  modulation {0};    // ModCode: 1 BPSK, 2 QPSK, 3 16QAM, 4 64QAM
    int16_t  tx_atten_cdb {0};  // centi-dB, so 10.25 dB is representable

    // ── link counters ─────────────────────────────────────────────────────
    uint64_t frames_tx      {0};
    uint64_t frames_rx_good {0};
    uint64_t frames_rx_bad  {0};
    uint64_t dropped        {0};
    uint64_t bytes_tx       {0};
    uint64_t bytes_rx       {0};
    uint64_t fec_corrected  {0};

    // ── quality ───────────────────────────────────────────────────────────
    int16_t  rssi_cdbm  {-10000};   // centi-dBm
    int16_t  snr_cdb    {0};        // centi-dB
    uint16_t tx_kbps    {0};
    uint16_t rx_kbps    {0};
    uint8_t  tx_duty_pct{0};
    int16_t  temp_cc    {0};        // centi-degrees C

    // ── FPGA identity, 0 when there is no fabric modem ────────────────────
    uint32_t fpga_magic  {0};
    uint32_t fpga_version{0};
    uint8_t  fpga_abi    {0};
    uint8_t  regmap_ver  {0};

    // Serialise to the wire. Always emits the current version's full body.
    std::vector<uint8_t> encode() const;

    // Parse. Returns nullopt when the magic, length, or CRC do not check out --
    // a corrupt report must be indistinguishable from no report, never from a
    // report of zeros.
    static std::optional<StatePacket> decode(const uint8_t* data, size_t len);

    // Human-readable, and the same field names the JSON stats use so an
    // operator does not have to learn two vocabularies.
    std::string toJSON() const;
};

} // namespace sdr
