#include "Config.hpp"
#include <fstream>
#include <sstream>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <iostream>

// Minimal JSON parser (avoids nlohmann/rapidjson dependency).
// Parses the flat key:value pairs we need from config.json.
namespace {

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n\"");
    auto e = s.find_last_not_of(" \t\r\n\"");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

// Returns value string for "key": value from flat JSON.
std::string jsonGet(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < json.size() && std::isspace(json[pos])) ++pos;
    if (pos >= json.size()) return {};

    if (json[pos] == '"') {
        auto e = json.find('"', pos + 1);
        return (e == std::string::npos) ? "" : json.substr(pos + 1, e - pos - 1);
    }
    // Number or bool: read until , or }
    auto e = json.find_first_of(",}\n", pos);
    return trim(json.substr(pos, e - pos));
}

bool jsonBool(const std::string& json, const std::string& key, bool def = false) {
    std::string v = jsonGet(json, key);
    if (v == "true" || v == "1") return true;
    if (v == "false" || v == "0") return false;
    return def;
}

double jsonDouble(const std::string& json, const std::string& key, double def = 0.0) {
    std::string v = jsonGet(json, key);
    if (v.empty()) return def;
    try { return std::stod(v); } catch (...) { return def; }
}

int jsonInt(const std::string& json, const std::string& key, int def = 0) {
    std::string v = jsonGet(json, key);
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

std::string jsonStr(const std::string& json, const std::string& key,
                    const std::string& def = {}) {
    std::string v = jsonGet(json, key);
    return v.empty() ? def : v;
}

} // anonymous namespace

namespace sdr {

Config Config::fromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Config: cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string json = ss.str();

    Config c;
    c.mode             = jsonStr   (json, "mode",              c.mode);
    c.pluto_ip         = jsonStr   (json, "pluto_ip",          c.pluto_ip);
    c.freq_tx_mhz      = jsonDouble(json, "freq_tx_mhz",       c.freq_tx_mhz);
    c.freq_rx_mhz      = jsonDouble(json, "freq_rx_mhz",       c.freq_rx_mhz);
    c.bw_mhz           = jsonInt   (json, "bw_mhz",            c.bw_mhz);
    c.tx_atten_db      = jsonDouble(json, "tx_atten_db",       c.tx_atten_db);
    c.gain_mode        = jsonStr   (json, "gain_mode",         c.gain_mode);
    c.modulation       = jsonStr   (json, "modulation",        c.modulation);
    c.tap_iface        = jsonStr   (json, "tap_iface",         c.tap_iface);
    c.bridge_iface     = jsonStr   (json, "bridge_iface",      c.bridge_iface);
    c.lan_iface        = jsonStr   (json, "lan_iface",         c.lan_iface);
    c.tap_mtu          = jsonInt   (json, "tap_mtu",           c.tap_mtu);
    c.encrypt          = jsonBool  (json, "encrypt",           c.encrypt);
    c.fec              = jsonBool  (json, "fec",               c.fec);
    c.aes_key_hex      = jsonStr   (json, "aes_key_hex",       c.aes_key_hex);
    c.burst_block      = jsonInt   (json, "burst_block",       c.burst_block);
    c.burst_threshold  = jsonDouble(json, "burst_threshold",   c.burst_threshold);
    c.burst_margin     = jsonInt   (json, "burst_margin",      c.burst_margin);
    c.burst_merge_gap  = jsonInt   (json, "burst_merge_gap",   c.burst_merge_gap);
    c.arq              = jsonBool  (json, "arq",               c.arq);
    c.arq_window       = jsonInt   (json, "arq_window",        c.arq_window);
    c.arq_timeout_ms   = jsonInt   (json, "arq_timeout_ms",    c.arq_timeout_ms);
    c.arq_max_retries  = jsonInt   (json, "arq_max_retries",   c.arq_max_retries);
    c.stats_interval_ms= jsonInt   (json, "stats_interval_ms", c.stats_interval_ms);
    c.monitor_port     = jsonInt   (json, "monitor_port",      c.monitor_port);
    c.node_id          = jsonStr   (json, "node_id",           c.node_id);
    c.scan_start_mhz   = jsonDouble(json, "scan_start_mhz",    c.scan_start_mhz);
    c.scan_step_mhz    = jsonDouble(json, "scan_step_mhz",     c.scan_step_mhz);
    c.scan_n           = jsonInt   (json, "scan_n",            c.scan_n);

    c.validate();
    return c;
}

void Config::validate() {
    // Parse node_id
    try {
        node_id_u32 = static_cast<uint32_t>(std::stoul(node_id, nullptr, 0));
    } catch (...) {
        node_id_u32 = 1;
    }

    // Parse AES key
    if (encrypt) {
        if (aes_key_hex.size() != 64)
            throw std::invalid_argument("Config: aes_key_hex must be 64 hex chars");
        for (int i = 0; i < 32; ++i) {
            auto byte_str = aes_key_hex.substr(static_cast<size_t>(i * 2), 2);
            aes_key_bytes[static_cast<size_t>(i)] =
                static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        }
    }

    // Clamp values
    if (tx_atten_db < 0.0)  tx_atten_db = 0.0;
    if (tx_atten_db > 89.0) tx_atten_db = 89.0;
    if (bw_mhz < 1)       bw_mhz = 1;
    if (bw_mhz > 20)      bw_mhz = 20;
    if (arq_window < 1)     arq_window = 1;
    if (arq_timeout_ms < 1) arq_timeout_ms = 1;
    if (arq_max_retries < 0) arq_max_retries = 0;

    // Above ~2 MHz the IQ stream exceeds what USB 2.0 sustains, so the
    // receiver spends most of its time not listening and silently misses
    // most bursts. Warn rather than clamp: a high rate is legitimate for
    // TX-only or scan use, it just cripples reception.
    if (bw_mhz > 2) {
        std::cerr << "[sdr] WARNING: bw_mhz=" << bw_mhz << " => "
                  << (bw_mhz * 4) << " MSPS => " << (bw_mhz * 16)
                  << " MB/s over USB, which exceeds USB 2.0's practical "
                     "throughput. Expect the receiver to miss most frames "
                     "(measured RX duty cycle: 99% at bw_mhz=1, 95% at 2, "
                     "but only ~38% at 5). Use bw_mhz 1-2 for reliable "
                     "reception.\n";
    }
}

} // namespace sdr
