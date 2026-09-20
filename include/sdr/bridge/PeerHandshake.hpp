#pragma once
// Know which peer you are talking to BEFORE forwarding customer traffic.
//
// WHY THIS EXISTS. Two units shipped with the default node_id 1 discarded every
// frame the other sent, as self-reception, silently: frames decoded, counters
// looked healthy, zero bytes were delivered, and the discard was not reported.
// It presented as a dead radio and cost days. A link that verifies who is on
// the other end cannot fail that way -- it says PEER_INCOMPATIBLE and names the
// field.
//
// The same check catches the other silent-total-loss configurations seen here:
// TX and RX not crossed between the units, mismatched diff_mode, mismatched
// sample rate, and a userspace/bitstream ABI mismatch.
//
// CARRIED ON THE EXISTING KEEPALIVE. The bridge already transmits FL_CTRL
// frames continuously to hold the demodulator's timing loop. Identity rides
// that channel, so there is no new protocol on the air and no extra airtime.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sdr {

struct PeerIdentity {
    uint32_t node_id      = 0;
    uint32_t fpga_abi     = 0;
    uint32_t fpga_map     = 0;
    uint32_t pkt_bytes    = 0;
    uint32_t sample_rate  = 0;
    uint32_t tx_freq      = 0;
    uint32_t rx_freq      = 0;
    uint32_t sw_version   = 0;   // build hash, advisory only
    uint8_t  diff_mode    = 0;
    // THE AUTHORITATIVE IDENTITY. node_id is a 31-bit hash of this, carried in
    // every frame header because the header has no room for more; the MAC
    // itself is carried here, where there is room. Two units can only share a
    // node_id by hash collision, and when they do this is what distinguishes
    // them.
    uint8_t  mac[6]       = {0,0,0,0,0,0};
    char     serial[17]   = {0}; // NUL-terminated, advisory
};

enum class PeerVerdict { UNKNOWN, COMPATIBLE, INCOMPATIBLE };

struct PeerCheck {
    PeerVerdict verdict = PeerVerdict::UNKNOWN;
    std::vector<std::string> reasons;   // why it is incompatible
    std::vector<std::string> warnings;  // advisory only; does not block
};

static constexpr uint32_t HELLO_MAGIC   = 0x48524453;  // "SDRH" little-endian
static constexpr uint8_t  HELLO_VERSION = 1;
static constexpr std::size_t HELLO_SIZE = 4 + 1 + 4*8 + 1 + 6 + 16;   // 60 bytes

inline void put32(uint8_t*& p, uint32_t v) { std::memcpy(p, &v, 4); p += 4; }
inline uint32_t get32(const uint8_t*& p) { uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; }

inline std::vector<uint8_t> encodeHello(const PeerIdentity& id) {
    std::vector<uint8_t> out(HELLO_SIZE, 0);
    uint8_t* p = out.data();
    put32(p, HELLO_MAGIC);
    *p++ = HELLO_VERSION;
    put32(p, id.node_id);   put32(p, id.fpga_abi);    put32(p, id.fpga_map);
    put32(p, id.pkt_bytes); put32(p, id.sample_rate); put32(p, id.tx_freq);
    put32(p, id.rx_freq);   put32(p, id.sw_version);
    *p++ = id.diff_mode;
    std::memcpy(p, id.mac, 6); p += 6;
    std::memcpy(p, id.serial, 16);
    return out;
}

inline bool decodeHello(const uint8_t* b, std::size_t n, PeerIdentity& out) {
    if (n < HELLO_SIZE) return false;
    const uint8_t* p = b;
    if (get32(p) != HELLO_MAGIC) return false;
    if (*p++ != HELLO_VERSION)   return false;   // a future version is not assumed compatible
    out.node_id = get32(p); out.fpga_abi = get32(p); out.fpga_map = get32(p);
    out.pkt_bytes = get32(p); out.sample_rate = get32(p); out.tx_freq = get32(p);
    out.rx_freq = get32(p); out.sw_version = get32(p);
    out.diff_mode = *p++;
    std::memcpy(out.mac, p, 6); p += 6;
    std::memcpy(out.serial, p, 16); out.serial[16] = 0;
    return true;
}

// Compare a peer against ourselves. Anything that causes SILENT TOTAL LOSS is
// a refusal; anything merely surprising is a warning.
inline PeerCheck checkPeer(const PeerIdentity& me, const PeerIdentity& peer) {
    PeerCheck c;
    auto bad = [&](const std::string& s) { c.reasons.push_back(s); };

    const bool same_mac = std::memcmp(me.mac, peer.mac, 6) == 0;

    // Our own signal coming back, not a peer. A unit with a coupled antenna
    // hears itself; that is expected and is not an incompatibility.
    if (same_mac) {
        c.warnings.push_back("this HELLO carries our own MAC: hearing our own transmitter");
        c.verdict = PeerVerdict::UNKNOWN;
        return c;
    }

    // The failure this was written for, and the hash collision behind it.
    // node_id is 31 bits derived from a 48-bit MAC, so two units CAN derive the
    // same id. Different MACs with the same id is that collision, and it is
    // fatal in a way the operator cannot otherwise see: each unit discards the
    // other's frames as self-reception, so neither ever receives this HELLO.
    // It is named here for the case where one side is reachable by other means.
    if (peer.node_id == me.node_id)
        bad("node_id COLLISION: both units derived " + std::to_string(me.node_id) +
            " from different MACs. Each discards the other's frames as "
            "self-reception, so neither will ever see the other's HELLO. "
            "Reprovision one unit.");

    // Frequencies must be CROSSED. Equal on one unit means it jams itself;
    // uncrossed between units means they never hear each other.
    if (me.tx_freq != peer.rx_freq)
        bad("our tx " + std::to_string(me.tx_freq) + " != peer rx " + std::to_string(peer.rx_freq));
    if (me.rx_freq != peer.tx_freq)
        bad("our rx " + std::to_string(me.rx_freq) + " != peer tx " + std::to_string(peer.tx_freq));

    if (me.sample_rate != peer.sample_rate)
        bad("sample rate " + std::to_string(me.sample_rate) + " vs " + std::to_string(peer.sample_rate));
    if (me.diff_mode != peer.diff_mode)
        bad("diff_mode " + std::to_string(me.diff_mode) + " vs " + std::to_string(peer.diff_mode));
    if (me.pkt_bytes != peer.pkt_bytes)
        bad("PKT_BYTES " + std::to_string(me.pkt_bytes) + " vs " + std::to_string(peer.pkt_bytes));
    if (me.fpga_abi != peer.fpga_abi)
        bad("FPGA ABI " + std::to_string(me.fpga_abi) + " vs " + std::to_string(peer.fpga_abi));
    if (me.fpga_map != peer.fpga_map)
        bad("register map " + std::to_string(me.fpga_map) + " vs " + std::to_string(peer.fpga_map));

    // Advisory: different software can interoperate, but an operator chasing a
    // fault should be told the two ends differ.
    if (me.sw_version != peer.sw_version)
        c.warnings.push_back("software build differs between the units");

    c.verdict = c.reasons.empty() ? PeerVerdict::COMPATIBLE : PeerVerdict::INCOMPATIBLE;
    return c;
}

} // namespace sdr
