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
    std::string pluto_ip    {"192.168.2.1"};
    double      freq_tx_mhz {434.0};
    double      freq_rx_mhz {439.0};
    int         bw_mhz      {10};         // 1|2|5|10|20
    int         tx_atten_db {10};         // 0-89 dB
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
