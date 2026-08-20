#pragma once
#include <string>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace sdr {

struct Config {
    // ── Operating mode ──────────────────────────────────────────────────────
    std::string mode        {"bridge"};   // bridge|mesh|p2p-tx|p2p-rx|scan

    // ── Hardware ────────────────────────────────────────────────────────────
    // Plain IP, or an explicit libiio backend URI ("usb:1.56.5") -- see
    // PlutoSDR::connect. The USB backend sustains a much higher streaming
    // rate than the network backend, so prefer it where possible.
    std::string pluto_ip    {"192.168.2.1"};
    double      freq_tx_mhz {434.0};
    double      freq_rx_mhz {439.0};
    // Sample rate is bw_mhz * 4 (RRC_SPS), and every sample is 4 bytes over
    // USB. USB 2.0 tops out near 35 MB/s in practice, so anything above
    // ~2 MHz here starves the receiver of listening time: measured RX duty
    // cycle is 99% at 1 MHz, 95% at 2 MHz (USB backend), but only 38% at
    // 5 MHz and 27% at 5 MHz over the network backend. Bursts arriving while
    // the receiver isn't listening are simply never seen, so a "faster"
    // setting here yields *fewer* delivered frames, not more.
    int         bw_mhz      {1};          // 1|2|5|10|20 (1-2 recommended)
    double      tx_atten_db {10.0};       // 0-89 dB, 0.25dB steps
    std::string gain_mode   {"fast_attack"};
    std::string modulation  {"AUTO"};     // AUTO|BPSK|QPSK|16QAM|64QAM

    // ── Bridge / mesh interface ──────────────────────────────────────────────
    std::string tap_iface    {"sdr0"};
    std::string bridge_iface {"br0"};
    std::string lan_iface    {"eth0"};
    int         tap_mtu      {1386};      // MAX_PAYLOAD(1400) - Ethernet header(14)

    // ── Security ─────────────────────────────────────────────────────────────
    bool        encrypt       {false};
    bool        fec           {false};
    std::string aes_key_hex;              // 64 hex chars = 32 bytes

    // ── Burst detection (RX) ─────────────────────────────────────────────────
    // Tuning knobs for BurstDetector. Defaults match BurstDetector::Config;
    // exposed here because the right threshold depends on the noise floor of
    // the deployment, and because `bursts_detected` vs `bursts_demodulated`
    // in the stats makes the effect directly measurable.
    int         burst_block      {256};   // samples per power block
    double      burst_threshold  {3.0};   // multiple of median block power
    int         burst_margin     {512};   // context kept either side
    int         burst_merge_gap  {512};   // merge windows closer than this

    // ── ARQ (reliable delivery, BridgeMode only) ─────────────────────────────
    bool        arq             {false};
    int         arq_window      {16};
    int         arq_timeout_ms  {80};
    int         arq_max_retries {5};

    // ── System ───────────────────────────────────────────────────────────────
    int         stats_interval_ms {1000};
    int         monitor_port      {8080};
    std::string node_id           {"0x00000001"};

    // ── Channel scan (mode=scan) ─────────────────────────────────────────────
    double      scan_start_mhz {430.0};
    double      scan_step_mhz  {1.0};
    int         scan_n         {20};

    // Parsed AES key (populated by validate())
    std::array<uint8_t, 32> aes_key_bytes{};
    uint32_t                node_id_u32{1};

    // Load from JSON file; throws on error.
    static Config fromFile(const std::string& path);

    // Parse / validate fields. Throws std::invalid_argument on bad values.
    void validate();
};

} // namespace sdr
