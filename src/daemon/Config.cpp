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
    c.samples_per_symbol = jsonInt (json, "samples_per_symbol", c.samples_per_symbol);
    c.tx_atten_db      = jsonDouble(json, "tx_atten_db",       c.tx_atten_db);
    c.gain_mode        = jsonStr   (json, "gain_mode",         c.gain_mode);
    c.tx_duty_max      = jsonDouble(json, "tx_duty_max",       c.tx_duty_max);
    c.cfo_method       = jsonStr   (json, "cfo_method",        c.cfo_method);
    c.rt_priority      = jsonInt   (json, "rt_priority",       c.rt_priority);
    c.pin_cores        = jsonBool  (json, "pin_cores",         c.pin_cores);
    c.rx_bw_factor     = jsonDouble(json, "rx_bw_factor",      c.rx_bw_factor);
    c.rx_buffer_samples= jsonInt   (json, "rx_buffer_samples", c.rx_buffer_samples);
    c.rx_queue_depth   = jsonInt   (json, "rx_queue_depth",    c.rx_queue_depth);
    c.carrier_sense    = jsonBool  (json, "carrier_sense",     c.carrier_sense);
    c.carrier_sense_hold_ms = jsonInt(json, "carrier_sense_hold_ms", c.carrier_sense_hold_ms);
    c.carrier_sense_max_defer_ms = jsonInt(json, "carrier_sense_max_defer_ms", c.carrier_sense_max_defer_ms);
    c.modulation       = jsonStr   (json, "modulation",        c.modulation);
    c.stats_path       = jsonStr   (json, "stats_path",        c.stats_path);
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
    c.burst_noise_q    = jsonDouble(json, "burst_noise_q",     c.burst_noise_q);
    c.arq              = jsonBool  (json, "arq",               c.arq);
    c.arq_window       = jsonInt   (json, "arq_window",        c.arq_window);
    c.arq_timeout_ms   = jsonInt   (json, "arq_timeout_ms",    c.arq_timeout_ms);
    c.arq_max_retries  = jsonInt   (json, "arq_max_retries",   c.arq_max_retries);
    c.stats_interval_ms= jsonInt   (json, "stats_interval_ms", c.stats_interval_ms);
    c.spectrum_interval_ms = jsonInt(json, "spectrum_interval_ms", c.spectrum_interval_ms);
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
    if (samples_per_symbol != 2 && samples_per_symbol != 4)
        throw std::invalid_argument(
            "Config: samples_per_symbol must be 2 or 4 (got " +
            std::to_string(samples_per_symbol) + ")");
    if (arq_window < 1)     arq_window = 1;
    if (arq_timeout_ms < 1) arq_timeout_ms = 1;
    if (arq_max_retries < 0) arq_max_retries = 0;
    if (tx_duty_max < 0.0) tx_duty_max = 0.0;
    if (tx_duty_max > 1.0) tx_duty_max = 1.0;
    if (rx_buffer_samples < 4096)   rx_buffer_samples = 4096;
    if (rx_buffer_samples > 262144) rx_buffer_samples = 262144;  // hardware buffer
    if (rx_queue_depth < 2)   rx_queue_depth = 2;
    if (rx_queue_depth > 256) rx_queue_depth = 256;
    if (rx_bw_factor < 1.0) rx_bw_factor = 1.0;
    if (rx_bw_factor > 3.0) rx_bw_factor = 3.0;
    if (rt_priority < 0) rt_priority = 0;
    if (rt_priority > 90) rt_priority = 90;

    // Warn based on actual host traffic rather than symbol rate. Interleaved
    // int16 I/Q costs four bytes/sample. Around 32 MB/s is the measured edge
    // for sustained USB reception; real Gigabit Ethernet may go higher.
    const int sample_rate_msps = bw_mhz * samples_per_symbol;
    const int stream_rate_mbs  = sample_rate_msps * 4;
    if (stream_rate_mbs > 32) {
        std::cerr << "[sdr] WARNING: bw_mhz=" << bw_mhz << " => "
                  << sample_rate_msps << " MSPS => " << stream_rate_mbs
                  << " MB/s over USB, which exceeds USB 2.0's practical "
                     "sustained receive throughput. Prefer the board's real "
                     "Gigabit Ethernet backend or reduce bw_mhz / "
                     "samples_per_symbol.\n";
    }

    // Modulation. Acquisition is always BPSK; this selects the payload
    // scheme for TRANSMIT only.
    for (auto& ch : modulation) ch = static_cast<char>(std::toupper(ch));
    if (modulation == "AUTO") {
        std::cerr
            << "[sdr] WARNING: modulation=AUTO is not supported and has been "
               "forced to BPSK.\n"
               "[sdr]          Link adaptation requires the two nodes to agree "
               "on a scheme. The previous\n"
               "[sdr]          implementation let each node pick from its own "
               "local RSSI, which cannot\n"
               "[sdr]          produce agreement, and it also modulated the "
               "preamble with the payload\n"
               "[sdr]          scheme -- making the burst invisible to the "
               "BPSK correlator (measured:\n"
               "[sdr]          0/216 preambles detected under AUTO vs 91/92 "
               "under forced BPSK).\n"
               "[sdr]          Set modulation to BPSK or QPSK explicitly.\n";
        modulation = "BPSK";
    } else if (modulation == "16QAM" || modulation == "64QAM") {
        std::cerr << "[sdr] WARNING: modulation=" << modulation
                  << " has not been validated over the air on this link. "
                     "Acquisition stays BPSK, so the burst will still be "
                     "detected, but payload demodulation is unproven. Use "
                     "BPSK or QPSK for reliable operation.\n";
    } else if (modulation != "BPSK" && modulation != "QPSK") {
        throw std::invalid_argument(
            "Config: modulation must be BPSK|QPSK|16QAM|64QAM (got '"
            + modulation + "')");
    }
}

ModCode Config::txModCode() const {
    if (modulation == "BPSK")  return ModCode::BPSK;
    if (modulation == "QPSK")  return ModCode::QPSK;
    if (modulation == "16QAM") return ModCode::QAM16;
    if (modulation == "64QAM") return ModCode::QAM64;
    return ModCode::BPSK;   // validate() guarantees we never get here
}

} // namespace sdr
