#pragma once
// A low-rate, active end-to-end health probe through the REAL bridge/RF
// path -- not a synthetic loopback -- so p50/p95/p99/max RTT and delivery
// ratio reflect what a real payload frame actually experiences, including
// modem acquisition, FEC, and queueing, not just what passive counters
// (rx.frames, crc_errors, ...) can infer.
//
// RIDES THE SAME FL_CTRL CHANNEL HELLO ALREADY USES, for the same reason
// PeerHandshake.hpp gives: the bridge already fills idle airtime with
// control frames to hold the demodulator's timing loop, so this adds no new
// protocol and no extra airtime beyond occasionally substituting one
// control filler frame for another.
//
// NO CLOCK SYNCHRONIZATION NEEDED. The timestamp a request carries is
// meaningful only to the unit that sent it -- the REPLY echoes it back
// verbatim, unmodified, so RTT is (local now) - (local send time), entirely
// on the sender's own clock. Whatever clock the peer used to decide when to
// reply, or whether the two clocks agree at all, never matters. This is the
// ordinary ping/echo pattern, and it is why this is an RTT probe rather
// than a one-way latency measurement -- one-way needs synchronized clocks
// (roadmap: "Time Synchronization and Mission Clock"), which does not exist
// yet; RTT does not need it at all.
#include <cstdint>
#include <cstring>
#include <vector>

namespace sdr {

// Wire bytes "PREQ" / "PRPY" respectively, little-endian (same convention
// HELLO_MAGIC uses -- see PeerHandshake.hpp).
static constexpr uint32_t PROBE_REQUEST_MAGIC = 0x51455250;
static constexpr uint32_t PROBE_REPLY_MAGIC   = 0x59505250;
static constexpr uint8_t  PROBE_VERSION       = 1;
static constexpr std::size_t PROBE_SIZE       = 4 + 1 + 4 + 8;   // magic+version+seq+send_time_us = 17 bytes

struct ProbeMessage {
    uint32_t seq          = 0;
    uint64_t send_time_us = 0;   // sender's own monotonic clock; meaningless to anyone else
};

enum class ProbeKind { NONE, REQUEST, REPLY };

inline std::vector<uint8_t> encodeProbe(uint32_t magic, const ProbeMessage& m) {
    std::vector<uint8_t> out(PROBE_SIZE, 0);
    uint8_t* p = out.data();
    std::memcpy(p, &magic, 4); p += 4;
    *p++ = PROBE_VERSION;
    std::memcpy(p, &m.seq, 4); p += 4;
    std::memcpy(p, &m.send_time_us, 8); p += 8;
    return out;
}

// Distinguishing a probe frame from a HELLO needs no extra tag: HELLO_SIZE
// is 60 bytes and PROBE_SIZE is 17, so decodeHello() already rejects a
// probe frame as too short before this is ever tried, and vice versa --
// this still checks the magic itself rather than relying on size alone, in
// case the two ever happen to collide after a future change to either.
inline ProbeKind decodeProbe(const uint8_t* b, std::size_t n, ProbeMessage& out) {
    if (n < PROBE_SIZE) return ProbeKind::NONE;
    uint32_t magic; std::memcpy(&magic, b, 4);
    if (magic != PROBE_REQUEST_MAGIC && magic != PROBE_REPLY_MAGIC) return ProbeKind::NONE;
    const uint8_t* p = b + 4;
    if (*p++ != PROBE_VERSION) return ProbeKind::NONE;   // a future version is not assumed compatible
    std::memcpy(&out.seq, p, 4); p += 4;
    std::memcpy(&out.send_time_us, p, 8);
    return magic == PROBE_REQUEST_MAGIC ? ProbeKind::REQUEST : ProbeKind::REPLY;
}

} // namespace sdr
