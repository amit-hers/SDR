#include "sdr/telemetry/StatePacket.hpp"
#include <cstring>
#include <sstream>
#include <iomanip>

namespace sdr {
namespace {

// Same polynomial and convention as the frame CRC, so there is one CRC in the
// project rather than two that look alike and are not.
uint32_t crc32(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *d++;
        for (int i = 0; i < 8; ++i) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
    }
    return ~c;
}

// Explicit little-endian writers. Appending is how the body grows.
void put8 (std::vector<uint8_t>& v, uint8_t x)  { v.push_back(x); }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8*i)) & 0xFF); }
void put64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back((x >> (8*i)) & 0xFF); }

// Readers that know how much is left. `ok` latches false on the first
// truncation, so a short body yields defaults for the tail rather than reading
// past the end -- which is exactly the case an older receiver hits when a newer
// sender has NOT yet been deployed, and vice versa.
struct Reader {
    const uint8_t* p; size_t left; bool ok{true};
    uint8_t  u8 () { if (left < 1) { ok = false; return 0; } uint8_t  x = p[0]; p += 1; left -= 1; return x; }
    uint16_t u16() { if (left < 2) { ok = false; return 0; } uint16_t x = uint16_t(p[0]) | (uint16_t(p[1]) << 8); p += 2; left -= 2; return x; }
    uint32_t u32() { if (left < 4) { ok = false; return 0; } uint32_t x = 0; for (int i = 0; i < 4; ++i) x |= uint32_t(p[i]) << (8*i); p += 4; left -= 4; return x; }
    uint64_t u64() { if (left < 8) { ok = false; return 0; } uint64_t x = 0; for (int i = 0; i < 8; ++i) x |= uint64_t(p[i]) << (8*i); p += 8; left -= 8; return x; }
};

} // namespace

std::vector<uint8_t> StatePacket::encode() const {
    std::vector<uint8_t> body;
    body.reserve(128);
    put32(body, node_id);      put32(body, uptime_s);   put8(body, mode);
    put32(body, freq_tx_hz);   put32(body, freq_rx_hz); put32(body, bw_hz);
    put8 (body, sps);          put8(body, modulation);
    put16(body, uint16_t(tx_atten_cdb));
    put64(body, frames_tx);    put64(body, frames_rx_good); put64(body, frames_rx_bad);
    put64(body, dropped);      put64(body, bytes_tx);       put64(body, bytes_rx);
    put64(body, fec_corrected);
    put16(body, uint16_t(rssi_cdbm)); put16(body, uint16_t(snr_cdb));
    put16(body, tx_kbps);      put16(body, rx_kbps);    put8(body, tx_duty_pct);
    put16(body, uint16_t(temp_cc));
    put32(body, fpga_magic);   put32(body, fpga_version);
    put8 (body, fpga_abi);     put8(body, regmap_ver);

    std::vector<uint8_t> out;
    out.reserve(HEADER_BYTES + body.size() + CRC_BYTES);
    put32(out, MAGIC);
    put16(out, PROTO_VERSION);
    put16(out, uint16_t(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    // CRC covers header and body: a corrupted length must not verify either.
    put32(out, crc32(out.data(), out.size()));
    return out;
}

std::optional<StatePacket> StatePacket::decode(const uint8_t* data, size_t len) {
    if (!data || len < HEADER_BYTES + CRC_BYTES) return std::nullopt;

    Reader h{data, len};
    const uint32_t magic = h.u32();
    const uint16_t ver   = h.u16();
    const uint16_t blen  = h.u16();
    if (magic != MAGIC)     return std::nullopt;
    // A future MAJOR version may reorder fields, so refuse rather than
    // misinterpret. Same version with a longer body is fine and expected.
    if (ver > PROTO_VERSION) return std::nullopt;
    if (blen > MAX_BODY)     return std::nullopt;
    if (len < HEADER_BYTES + blen + CRC_BYTES) return std::nullopt;

    const size_t crc_off = HEADER_BYTES + blen;
    uint32_t got = 0;
    for (int i = 0; i < 4; ++i) got |= uint32_t(data[crc_off + i]) << (8*i);
    if (got != crc32(data, crc_off)) return std::nullopt;

    StatePacket s;
    Reader r{data + HEADER_BYTES, blen};
    s.node_id      = r.u32(); s.uptime_s     = r.u32(); s.mode        = r.u8();
    s.freq_tx_hz   = r.u32(); s.freq_rx_hz   = r.u32(); s.bw_hz       = r.u32();
    s.sps          = r.u8();  s.modulation   = r.u8();
    s.tx_atten_cdb = int16_t(r.u16());
    s.frames_tx    = r.u64(); s.frames_rx_good = r.u64(); s.frames_rx_bad = r.u64();
    s.dropped      = r.u64(); s.bytes_tx     = r.u64(); s.bytes_rx    = r.u64();
    s.fec_corrected= r.u64();
    s.rssi_cdbm    = int16_t(r.u16()); s.snr_cdb = int16_t(r.u16());
    s.tx_kbps      = r.u16(); s.rx_kbps      = r.u16(); s.tx_duty_pct = r.u8();
    s.temp_cc      = int16_t(r.u16());
    s.fpga_magic   = r.u32(); s.fpga_version = r.u32();
    s.fpga_abi     = r.u8();  s.regmap_ver   = r.u8();
    // r.ok being false only means this sender is older than we are; the fields
    // it did not send keep their defaults. That is the compatibility contract,
    // not an error.
    return s;
}

std::string StatePacket::toJSON() const {
    auto f2 = [](double v) {
        std::ostringstream o; o << std::fixed << std::setprecision(2) << v; return o.str();
    };
    static const char* MODES[] = {"unknown","bridge","mesh","p2p-tx","p2p-rx","scan"};
    static const char* MODS[]  = {"?","BPSK","QPSK","16QAM","64QAM"};
    std::ostringstream o;
    o << "{\n"
      << "  \"node_id\": " << node_id << ",\n"
      << "  \"uptime_s\": " << uptime_s << ",\n"
      << "  \"mode\": \"" << (mode < 6 ? MODES[mode] : "unknown") << "\",\n"
      << "  \"freq_tx_mhz\": " << f2(freq_tx_hz / 1e6) << ",\n"
      << "  \"freq_rx_mhz\": " << f2(freq_rx_hz / 1e6) << ",\n"
      << "  \"bw_mhz\": " << f2(bw_hz / 1e6) << ",\n"
      << "  \"samples_per_symbol\": " << int(sps) << ",\n"
      << "  \"modulation\": \"" << (modulation < 5 ? MODS[modulation] : "?") << "\",\n"
      << "  \"tx_atten_db\": " << f2(tx_atten_cdb / 100.0) << ",\n"
      << "  \"frames_tx\": " << frames_tx << ",\n"
      << "  \"frames_rx_good\": " << frames_rx_good << ",\n"
      << "  \"frames_rx_bad\": " << frames_rx_bad << ",\n"
      << "  \"dropped\": " << dropped << ",\n"
      << "  \"bytes_tx\": " << bytes_tx << ",\n"
      << "  \"bytes_rx\": " << bytes_rx << ",\n"
      << "  \"fec_corrected\": " << fec_corrected << ",\n"
      << "  \"rssi_dbm\": " << f2(rssi_cdbm / 100.0) << ",\n"
      << "  \"snr_db\": " << f2(snr_cdb / 100.0) << ",\n"
      << "  \"tx_kbps\": " << tx_kbps << ",\n"
      << "  \"rx_kbps\": " << rx_kbps << ",\n"
      << "  \"tx_duty_pct\": " << int(tx_duty_pct) << ",\n"
      << "  \"temp_c\": " << f2(temp_cc / 100.0) << ",\n"
      << "  \"fpga_magic\": \"0x" << std::hex << std::setw(8) << std::setfill('0') << fpga_magic << std::dec << "\",\n"
      << "  \"fpga_version\": \"0x" << std::hex << std::setw(8) << std::setfill('0') << fpga_version << std::dec << "\",\n"
      << "  \"fpga_abi\": " << int(fpga_abi) << ",\n"
      << "  \"register_map_version\": " << int(regmap_ver) << "\n"
      << "}\n";
    return o.str();
}

} // namespace sdr
